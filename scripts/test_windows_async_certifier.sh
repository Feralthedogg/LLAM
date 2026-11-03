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
# See LICENSE.

# Offline, pinned LLVM23 admission/COFF regression. Native R7/runtime CTest
# remains a separate gate; this script cannot certify Windows execution.
set -euo pipefail
ulimit -c 0

if [[ $# -ne 6 ]]; then
    echo "usage: $0 NEW_OUTPUT_DIR CLANG LLAM_PASS_PLUGIN PATCHED_LLC LLD_LINK LLVM_READOBJ" >&2
    exit 2
fi

r8_out=$1
r8_clang=$2
r8_plugin=$3
r8_llc=$4
r8_lld=$5
r8_readobj=$6
r8_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ -e $r8_out || ! -x $r8_readobj ]]; then
    echo "Choose a new output path and an executable llvm-readobj" >&2
    exit 2
fi
mkdir -p "$r8_out"
r8_out=$(cd "$r8_out" && pwd)
"$r8_root/scripts/build_windows_async_certified_fixture.sh" \
    "$r8_out/positive" "$r8_clang" "$r8_plugin" "$r8_llc" "$r8_lld" \
    >"$r8_out/positive-build.log" 2>&1

"$r8_readobj" --sections --symbols --relocations \
    "$r8_out/positive/loop.obj" >"$r8_out/loop-readobj.txt"
grep -q 'Selection: NoDuplicates (0x1)' "$r8_out/loop-readobj.txt"
grep -q 'Selection: Associative (0x5)' "$r8_out/loop-readobj.txt"
if [[ $(grep -c 'IMAGE_REL_AMD64_REL32' "$r8_out/loop-readobj.txt") -ne 2 ]]; then
    echo "Certified loop needs exactly two COFF REL32 fields" >&2
    exit 1
fi
"$r8_readobj" --sections "$r8_out/positive/llam-r8-cert-loop-two.dll" \
    >"$r8_out/two-readobj.txt"
grep -A2 'Name: .llampm ' "$r8_out/two-readobj.txt" | \
    grep -q 'VirtualSize: 0x60'

# Only the scalar leaf and ordinary H are rooted here. The selected loop and
# the matrix's three selected leaves must lose their metadata with their code.
"$r8_lld" /dll /noentry /opt:ref /opt:noicf /machine:x64 \
    "/out:$r8_out/one.dll" "/implib:$r8_out/one.lib" \
    /export:llam_win_good64 /export:llam_transport_ordinary \
    "$r8_out/positive/scalar.obj" "$r8_out/positive/loop.obj" \
    "$r8_out/positive/matrix.obj"
"$r8_readobj" --sections "$r8_out/one.dll" >"$r8_out/one-readobj.txt"
grep -A2 'Name: .llampm ' "$r8_out/one-readobj.txt" | \
    grep -q 'VirtualSize: 0x30'

r8_negative_source="$r8_root/experiments/windows/llam_async_certification_negative.c"

compile_negative() {
    local name=$1 macro=$2
    LLAM_PREEMPT_WIN64_AUDIT=1 LLAM_PREEMPT_REQUIRED=1 \
        "$r8_clang" --target=x86_64-w64-windows-gnu \
        -O2 -fno-unroll-loops -g0 "-D$macro" \
        "-fpass-plugin=$r8_plugin" -S -emit-llvm \
        "$r8_negative_source" -o "$r8_out/$name.ll"
    grep -q '"llam.preempt.win64_ir_accepted"' "$r8_out/$name.ll"
}

expect_llc_failure() {
    local name=$1 expected=$2
    shift 2
    cp "$r8_out/positive/scalar.obj" "$r8_out/$name.obj"
    if ("$r8_llc" -mtriple=x86_64-w64-windows-gnu -filetype=obj \
        -function-sections -llam-preempt-certify-v1 \
        -align-loops=0 \
        -x86-experimental-pref-innermost-loop-alignment=0 \
        "$@" "$r8_out/$name.ll" -o "$r8_out/$name.obj" \
        >"$r8_out/$name.log" 2>&1) 2>/dev/null; then
        echo "Unexpected certification acceptance: $name" >&2
        exit 1
    fi
    if ! grep -q -F "$expected" "$r8_out/$name.log" ||
       [[ -e $r8_out/$name.obj ]]; then
        echo "Wrong refusal or stale output: $name" >&2
        exit 1
    fi
    echo "$name: refused ($expected), no stale object"
}

compile_negative stack LLAM_R8_NEG_STACK
expect_llc_failure stack 'MEMORY_OR_EFFECT ADD64rm'
compile_negative helper LLAM_R8_NEG_HELPER
expect_llc_failure helper 'CALL_OR_TAIL CALL64pcrel32'
compile_negative callee_save LLAM_R8_NEG_CALLEE_SAVE
expect_llc_failure callee_save 'MEMORY_OR_EFFECT PUSH64r'
compile_negative mixed_good_first LLAM_R8_NEG_MIXED_GOOD_FIRST
expect_llc_failure mixed_good_first 'MEMORY_OR_EFFECT ADD64rm'
compile_negative mixed_bad_first LLAM_R8_NEG_MIXED_BAD_FIRST
expect_llc_failure mixed_bad_first 'MEMORY_OR_EFFECT ADD64rm'

cp "$r8_out/positive/loop.ll" "$r8_out/sls.ll"
expect_llc_failure sls 'EMISSION_PROFILE SLS hardening' \
    -mattr=+harden-sls-ret
cp "$r8_out/positive/loop.ll" "$r8_out/padding.ll"
expect_llc_failure padding 'EMISSION_PROFILE MC branch/prefix padding' \
    -x86-align-branch-boundary=32 -x86-align-branch=jcc
cp "$r8_out/positive/loop.ll" "$r8_out/mode_combination.ll"
expect_llc_failure mode_combination 'modes are exclusive' \
    -llam-preempt-probe-llpm

cp "$r8_out/positive/loop.obj" "$r8_out/no_function_sections.obj"
if ("$r8_llc" -mtriple=x86_64-w64-windows-gnu -filetype=obj \
        -llam-preempt-certify-v1 -align-loops=0 \
        -x86-experimental-pref-innermost-loop-alignment=0 \
        "$r8_out/positive/loop.ll" \
        -o "$r8_out/no_function_sections.obj" \
        >"$r8_out/no_function_sections.log" 2>&1) 2>/dev/null ||
   ! grep -q 'one one-only code section' \
        "$r8_out/no_function_sections.log" ||
   [[ -e $r8_out/no_function_sections.obj ]]; then
    echo "Missing function-sections profile did not fail closed" >&2
    exit 1
fi

cp "$r8_out/positive/loop.obj" "$r8_out/msvc_target.obj"
if ("$r8_llc" -mtriple=x86_64-pc-windows-msvc -filetype=obj \
        -function-sections -llam-preempt-certify-v1 \
        "$r8_out/positive/loop.ll" -o "$r8_out/msvc_target.obj" \
        >"$r8_out/msvc_target.log" 2>&1) 2>/dev/null ||
   ! grep -q 'requires Windows GNU/COFF target' \
        "$r8_out/msvc_target.log" ||
   [[ -e $r8_out/msvc_target.obj ]]; then
    echo "Unsupported MSVC target did not fail closed" >&2
    exit 1
fi

if LLAM_PREEMPT_REQUIRED=1 LLAM_PREEMPT_WIN64_AUDIT=0 \
    "$r8_clang" --target=x86_64-w64-windows-gnu -O2 \
    "-fpass-plugin=$r8_plugin" -S -emit-llvm \
    "$r8_root/experiments/windows/llam_async_certified_scalar.c" \
    -o "$r8_out/audit-off.ll" >"$r8_out/audit-off.log" 2>&1 ||
   ! grep -q 'unsupported target' "$r8_out/audit-off.log"; then
    echo "Windows early-verifier opt-in absent did not fail closed" >&2
    exit 1
fi

"$r8_llc" -mtriple=x86_64-w64-windows-gnu -filetype=obj \
    -function-sections -llam-preempt-late-audit \
    -align-loops=0 -x86-experimental-pref-innermost-loop-alignment=0 \
    "$r8_out/positive/loop.ll" -o "$r8_out/audit-only.obj" \
    >"$r8_out/audit-only.log" 2>&1
"$r8_readobj" --sections "$r8_out/audit-only.obj" \
    >"$r8_out/audit-only-readobj.txt"
if grep -q '.llampm' "$r8_out/audit-only-readobj.txt"; then
    echo "Audit-only mode emitted a certificate" >&2
    exit 1
fi

LLAM_PREEMPT_REQUIRED=1 "$r8_clang" \
    --target=x86_64-pc-linux-gnu -O2 \
    "-fpass-plugin=$r8_plugin" -S -emit-llvm \
    "$r8_root/experiments/windows/llam_async_certified_scalar.c" \
    -o "$r8_out/linux-elf.ll"
"$r8_llc" -mtriple=x86_64-pc-linux-gnu -filetype=obj \
    "$r8_out/linux-elf.ll" -o "$r8_out/linux-elf.o"
"$r8_readobj" --sections "$r8_out/linux-elf.o" \
    >"$r8_out/linux-elf-readobj.txt"
grep -q '.llam.preempt' "$r8_out/linux-elf-readobj.txt"

echo "R8 certifier offline checks passed; Linux ELF *emission* observed, not full Linux regression"
