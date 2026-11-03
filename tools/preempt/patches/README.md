<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSES/Apache-2.0.txt.
-->

# Experimental LLVM 23 Win64 LLPM emitter

`llvm-23.1.2-win64-llpm.patch` is an experimental, opt-in modification of
official LLVM 23.1.2 X86 code generation. It is **not** part of the normal
LLAM or upstream LLVM build. The patch changes only
`X86AsmPrinter.cpp` and `X86AsmBackend.cpp`; their original upstream
Apache-2.0 WITH LLVM-exception notices remain in those files. The patch's
LLAM-authored additions are Apache-2.0, recorded in the adjacent `.license`.

The tested canonical source base is Homebrew's `brew unpack --patch llvm`
output for LLVM 23.1.2. Its official release tarball is
`llvm-project-23.1.2.src.tar.xz` (SHA-256
`c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a`),
followed by Homebrew's two declared upstream patch inputs:

- `1381ad497b9a6d3da630cbef53cbfa9ddf117bb6...40a8c7c0ff3f688b690e4c74db734de67f0f89e9.diff`
  SHA-256 `f6dafd762737eb79761ab7ef814a9fc802ec4bb8d20f46691f07178053b0eb36`.
- `b8007a8e4020b8bca2b12e941660e10bf5bf6716.patch`
  SHA-256 `e41e300eb6f5cca9172ab344e572c3fb24f0d05885ae23dd7cb4f9c2528839f7`.

The two X86 source files changed by the LLAM patch are byte-identical in the
official release and Homebrew-patched source, but the tested full build base
includes the two declared Homebrew patches. Apply **only** the packaged LLAM
patch after that base, and run `git apply --check` first. Another LLVM version
or source-patch combination is not qualified.

For the tested macOS cross-build, starting in the LLAM repository:

```sh
brew unpack --patch --destdir /tmp/llam-llvm23-source llvm
cd /tmp/llam-llvm23-source/llvm-23.1.2
git apply --check /absolute/path/to/LLAM/tools/preempt/patches/llvm-23.1.2-win64-llpm.patch
git apply /absolute/path/to/LLAM/tools/preempt/patches/llvm-23.1.2-win64-llpm.patch
cmake -S llvm -B /tmp/llam-llvm23-build -G 'Unix Makefiles' \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_ZSTD=OFF
cmake --build /tmp/llam-llvm23-build --target llc --parallel 4
brew developer off
```

Replace `/absolute/path/to/LLAM` with the repository's actual absolute
directory and use fresh temporary source/build directories. The two
Homebrew patch checksums above must be verified if its formula changes.

The extension implements separate hidden `llc` modes:

- `-llam-preempt-probe-end`: non-certifying endpoint marker probe.
- `-llam-preempt-probe-llpm`: developer-only manually certified transport
  fixtures; never combine it with certification mode.
- `-llam-preempt-late-audit` / `-llam-preempt-late-require`: narrow final-machine
  diagnostics/refusal, without certification output.
- `-llam-preempt-certify-v1`: emit LLPM v1 only for real annotated source that
  passed LLAM's restrictive WinGNU IR check and the pinned final-machine,
  emission-profile, code-boundary and one-only COFF ownership checks.

The current experimental chain is:

1. Compile annotated x86-64 Windows GNU source at `-O2` through the matching
   LLVM 23 clang and LLAM pass with `LLAM_PREEMPT_WIN64_AUDIT=1`. This produces
   `llam.preempt.win64_ir_accepted` only after the restrictive IR predicate;
   it does not emit a PE certificate by itself.
2. Run the patched `llc` with `-llam-preempt-certify-v1`, `-function-sections`,
   and the required profile-specific options. For the currently qualified
   finite recurrence, those include `-align-loops=0` and
   `-x86-experimental-pref-innermost-loop-alignment=0`. Refused annotated
   functions must fail the build; do not publish an earlier object as current.
3. Link the generated COFF objects with qualified LLD 23.1.2, using
   `/opt:ref /opt:noicf`. GNU ld `--gc-sections`, ICF, LTO, code splitting,
   and another compiler/linker version are not qualified for this profile.
4. Validate the final loaded PE image with LLAM's unchanged private LLPM
   collector before any separate expert runtime activation.

`scripts/build_windows_async_certified_fixture.sh` accepts a *new* output
directory followed by explicit paths to LLVM 23.1.2 clang, the matching LLAM
pass plugin, the patched `llc`, and LLD `lld-link`. It preserves the optimized
IR, COFF objects and final DLL together and prints their hashes. The plugin
must be built from `tools/preempt/llam_preempt_pass.cpp` against the matching
LLVM 23.1.2 headers/libraries; the normal LLAM build does not implicitly
select this Windows certification path. The current CMake
`LLAM_PREEMPT_TOOLING=required` driver remains Linux-x86-64-only, so the
macOS cross-build used an explicit warning-clean plugin compile against
Homebrew LLVM 23.1.2:

```sh
/opt/homebrew/opt/llvm/bin/clang++ -std=c++17 -stdlib=libc++ \
  -fno-exceptions -fPIC -shared -Wall -Wextra -Wpedantic -Werror \
  -isystem /opt/homebrew/opt/llvm/include -Itools/preempt \
  tools/preempt/llam_preempt_pass.cpp \
  -L/opt/homebrew/opt/llvm/lib -lLLVM-23 -o /tmp/llam-win64-pass.dylib
```

To reconstruct the patched X86-only
`llc` from the declared source base, use CMake with
`LLVM_TARGETS_TO_BUILD=X86`, `CMAKE_BUILD_TYPE=Release`,
`LLVM_ENABLE_ASSERTIONS=ON`, and `LLVM_INCLUDE_TESTS=OFF`, then build target
`llc`. The exact clang/plugin/llc/linker paths belong in the test record.
`scripts/test_windows_async_certifier.sh` takes those four paths plus
`llvm-readobj` and a new output directory; it tests actual early acceptance,
late refusals and stale-output removal, mode separation, associative COFF
records, `/opt:ref` ownership, and audit-only non-emission. Its Linux check
observes ELF object metadata **only**; it does not replace the existing
Linux-host compiler/metadata/parser regression. The CMake option
`LLAM_WINDOWS_ASYNC_CERTIFIED_DLL` registers the separate native Windows
collector/runtime controls when pointed at the generated DLL.

The certificate is a limited compiler-body assertion, not proof that a caller
holds no native locks or that arbitrary C, callbacks, XSTATE modes, or multiple
LLAM M threads are safe to interrupt. The Windows runtime path remains
default-off, expert-only and process-fail-fast on uncertain context mutation.
The first compiler-certified objects have passed linked-field and native
collector-only checks; automatic preemption using those objects remains a
separate qualification gate. See the R8 entries in
`research/windows-iocp-async-2026-09-26/evidence.md`.
