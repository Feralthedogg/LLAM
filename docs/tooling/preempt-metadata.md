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
See LICENSE.
-->

# Compiler preemption metadata

LLAM ships an optional LLVM pass for inserting cooperative safepoints and
recording a small set of manually annotated asynchronous-safe code ranges. The
tooling is opt-in; the runtime's normal build and default cooperative policy do
not change. Rust, Python, JVM, .NET, and Go language-binding wrappers are not
included.

The LLVM pass currently emits async-safe ranges only for Linux x86-64 ELF
outputs. Cooperative safepoint insertion is also optional and only occurs at
natural-loop latches that LLVM can prove are legal insertion points. A function
with no such latch is left unchanged in `auto` mode and rejected in `required`
mode.

For development diagnostics only, `LLAM_PREEMPT_WIN64_AUDIT=1` lets the
matching LLVM pass run its existing restrictive IR predicate for x86-64
Windows GNU/COFF input. Accepted functions receive a private
`llam.preempt.win64_ir_accepted` tag, but this mode emits **no** PE LLPM
certificate and does not enable asynchronous preemption. Final machine-code
and linker qualification remain separate; `required` in this diagnostic mode
does not mean a usable Windows certificate was produced.

## Source annotations

Annotate only deliberately reviewed functions:

```c
__attribute__((annotate("llam-preempt-async-safe")))
unsigned long pure_leaf(unsigned long value) {
    return value * 3UL + 1UL;
}

__attribute__((annotate("llam-preempt-cooperative")))
unsigned long poll_in_loop(unsigned long limit) {
    unsigned long total = 0UL;
    for (unsigned long index = 0UL; index < limit; ++index) {
        total += index;
    }
    return total;
}
```

The pass seeds an early `noinline` attribute on async-safe candidates, then
validates them after optimization. Its deliberately conservative verifier
rejects memory reads or writes, calls (including inline assembly), atomics,
stack allocation, variadic access, exception handling, indirect control flow,
and unsupported function attributes. A rejected annotation is reported and
omitted in `auto`; `required` turns it into a compile error. This IR check is a
structural filter, not a proof of higher-level application semantics: only
annotate leaf code that has also been reviewed for LLAM/runtime interaction,
thread affinity, and hidden compiler/platform assumptions.

Cooperative annotations insert `llam_task_safepoint()` immediately before the
terminator of each natural-loop latch. LLAM's own Make/CMake runtime targets do
not load this plugin; `llam-clang` instruments only the source files passed to
it. The pass is not a substitute for explicit safepoints in code that has no
natural loop.

## Build the tooling

LLVM and a matching clang are optional. `off` is the default; `auto` builds the
LLVM pass when the supported host, C++ compiler, and LLVM development package
are available; `required` makes those build-time requirements fatal.

```sh
cmake -S . -B build-preempt -DLLAM_PREEMPT_TOOLING=auto
cmake --build build-preempt --target llam_preempt_pass llam_preempt_metadata_inspect
ctest --test-dir build-preempt --output-on-failure -R 'preempt|llam_clang'
cmake --install build-preempt --component PreemptTools --prefix /tmp/llam-sdk
```

GNU Make offers the same CMake-backed option:

```sh
make LLAM_PREEMPT_TOOLING=auto preempt-tools
```

The optional Meson project is scoped to the preemption tooling; it does not
replace the runtime's CMake/Make build:

```sh
meson setup /tmp/llam-preempt-meson -Dpreempt_tooling=auto
meson compile -C /tmp/llam-preempt-meson
meson test -C /tmp/llam-preempt-meson --print-errorlogs
```

## Compile and verify

`tools/llam-clang` forwards ordinary clang arguments. Its default is `off`.
`auto` uses the discovered plugin when available and otherwise invokes clang
unchanged. It selects `-O1` when the caller supplied no optimization level.

```sh
LLAM_CLANG=clang-14 \
LLAM_PREEMPT_PLUGIN="$PWD/build-preempt/llam_preempt_pass.so" \
tools/llam-clang --llam-preempt=auto -c app.c -o app.o
```

Use `required` on the final compile-and-link command when the output must carry
verified async-safe metadata:

```sh
LLAM_CLANG=clang-14 \
LLAM_PREEMPT_PLUGIN="$PWD/build-preempt/llam_preempt_pass.so" \
LLAM_PREEMPT_METADATA_INSPECT="$PWD/build-preempt/llam_preempt_metadata_inspect" \
tools/llam-clang --llam-preempt=required app.c -o app
```

`required` rejects `-O0`, unavailable/mismatched compiler tooling, unsupported hosts,
invalid async-safe annotations, and a final linked output that lacks at least
one verified async-safe range. Compile-only commands are allowed to produce an
object without such a range; the final link is where the whole-program
requirement is checked. Missing capability never falls back silently. Direct
clang use can set `LLAM_PREEMPT_REQUIRED=1` (or `LLAM_ASYNC_REQUIRED=1`) to make
the LLVM pass reject unsafe annotations, but only `llam-clang
--llam-preempt=required` performs the final
link check.

To inspect a linked executable manually:

```sh
objcopy --dump-section .llam.preempt=preempt.bin app
build-preempt/llam_preempt_metadata_inspect --required preempt.bin
```

The version-1 record uses the `.llam.preempt` ELF section. Its fixed-size
little-endian header contains magic, version, architecture, header/record size,
build identifier, range count, and flags. Each fixed-size range record stores
signed 32-bit PC-relative start/end displacements from its own offset fields,
range flags, and a reserved word. Multiple records may be concatenated. The
parser checks integer overflow, record and image bounds, duplicate/overlapping
ranges, reserved bits, architecture, and unknown optional/required flags.
Absent optional metadata means legacy cooperative behavior; malformed or
unknown-required metadata is never treated as a safe range.

The runtime does not automatically load or register this section yet. The
experimental async backend remains separately opt-in and requires its explicit
safe-range registration/capability path; possession of compiler metadata alone
does not enable asynchronous preemption. A loader that owns the module image
may use the private C hook
`llam_preempt_module_register(runtime, image_base, image_size, metadata,
metadata_size, required_mode)` before async capture is enabled. It validates
all concatenated records against the supplied image bounds and atomically adds
only `ASYNC_SAFE` ranges; optional absent/cooperative-only metadata is a no-op,
while required absent/unsupported/duplicate metadata returns `ENOTSUP` or
`EEXIST`. Registration is frozen once async capture starts, and runtime
shutdown releases the copied range table. Dynamic unload must remove the
module from the caller's lookup path and keep its metadata/image alive until
no capture can reference it; there is no public DSO unload API in this phase.
SDK packages record the selected tooling mode and whether the LLVM plugin was
included under `share/llam/preempt/tooling.conf`.

The expert-only Win64 public opt-in takes caller-certified PE-image PC ranges.
For an already loaded x64 PE image, the experimental build also has a
**private** LLPM v1 collector for a final `.llampm` read-only section. The
caller must hold a valid module reference when calling it. The collector
temporarily retains the module, bounds-checks its loaded PE headers and
section, parses the records at their mapped addresses, and copies only
nonoverlapping `ASYNC_SAFE` executable-image ranges. An absent section is a
clean no-metadata result; malformed or unsupported records fail closed. The
caller may pass the collected ranges to the separate public expert opt-in
while both module references overlap, then release the collector result.
The public opt-in still requires explicit caller certification and process
fail-fast acknowledgement. This collector is not a public ABI, a semantic
safety verifier, or an automatic activator. The normal LLAM tooling still does
not emit Windows certificates; the Linux ELF pass does not emit `.llampm`.

An isolated LLVM 23.1.2 X86 patch (`tools/preempt/patches/README.md` in the checkout)
now prototypes a narrow WinGNU scalar-leaf certificate emitter. Its explicit
certification mode requires the diagnostic Windows IR-verifier tag and a
fail-closed final-machine, emission-profile, endpoint, and COFF ownership
check. The generated records have passed an LLD-linked native collector-only
oracle; this does **not** enable automatic preemption or qualify the patch as
part of the default toolchain. The supported experimental link profile is
LLD 23.1.2 `/opt:ref /opt:noicf`; other toolchains and broader source shapes
remain unqualified.
