#!/usr/bin/env bash
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.

# Build the R8 test image only; it is not a production compiler driver.
set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 NEW_OUTPUT_DIR MATCHED_CLANG LLAM_PASS_PLUGIN PATCHED_LLC MATCHED_LLD_LINK" >&2
    exit 2
fi

r8_out=$1
r8_clang=$2
r8_plugin=$3
r8_llc=$4
r8_lld_link=$5
r8_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ ! -f $r8_plugin || ! -x $r8_clang || ! -x $r8_llc ||
      ! -x $r8_lld_link ]]; then
    echo "R8 fixture requires readable plugin and executable matched LLVM tools" >&2
    exit 2
fi
if [[ -e $r8_out ]]; then
    echo "R8 fixture output path already exists; choose a new directory" >&2
    exit 2
fi
mkdir -p "$r8_out"
r8_out=$(cd "$r8_out" && pwd)

"$r8_clang" --version | sed -n '1p'
"$r8_llc" --version | sed -n '1,2p'
"$r8_lld_link" --version

for r8_name in scalar loop matrix; do
    LLAM_PREEMPT_WIN64_AUDIT=1 LLAM_PREEMPT_REQUIRED=1 \
        "$r8_clang" --target=x86_64-w64-windows-gnu \
        -O2 -fno-unroll-loops -g0 \
        "-fpass-plugin=$r8_plugin" -S -emit-llvm \
        "$r8_root/experiments/windows/llam_async_certified_$r8_name.c" \
        -o "$r8_out/$r8_name.ll"
    "$r8_llc" -mtriple=x86_64-w64-windows-gnu -filetype=obj \
        -function-sections -llam-preempt-certify-v1 \
        -align-loops=0 \
        -x86-experimental-pref-innermost-loop-alignment=0 \
        "$r8_out/$r8_name.ll" -o "$r8_out/$r8_name.obj"
done

"$r8_lld_link" /dll /noentry /opt:ref /opt:noicf /machine:x64 \
    "/out:$r8_out/llam-r8-cert-loop-two.dll" \
    "/implib:$r8_out/llam-r8-cert-loop-two.lib" \
    /export:llam_r8_cpu_loop_v1 \
    /export:llam_win_good64 \
    /export:llam_transport_ordinary \
    "$r8_out/scalar.obj" "$r8_out/loop.obj" "$r8_out/matrix.obj"

if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$r8_out"/*.ll "$r8_out"/*.obj \
        "$r8_out/llam-r8-cert-loop-two.dll"
elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$r8_out"/*.ll "$r8_out"/*.obj \
        "$r8_out/llam-r8-cert-loop-two.dll"
fi
echo "R8 test DLL: $r8_out/llam-r8-cert-loop-two.dll"
