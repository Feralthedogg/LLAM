#!/usr/bin/env python3
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

"""Pinned LLVM 23.1.2 managed C driver, including mandatory final-link checks."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from managed_verify import VerificationError, require, verify


@dataclass
class Invocation:
    mode: str
    llvm: Path
    plugin: Path
    output: Path
    compile_only: bool
    sources: list[Path]
    objects: list[str]
    compile_flags: list[str]
    link_flags: list[str]


def parse(arguments: list[str]) -> Invocation:
    mode = "required"
    llvm = Path(os.environ.get("LLAM_LLVM_BIN", ""))
    plugin_value = os.environ.get("LLAM_MANAGED_PLUGIN", "")
    output = None
    compile_only = False
    sources, objects, compile_flags, link_flags = [], [], [], []
    args = iter(arguments)
    for arg in args:
        if arg.startswith("--llam-managed="):
            mode = arg.partition("=")[2]
            require(mode in ("auto", "required"), "managed mode must be auto or required")
        elif arg.startswith("--llam-llvm-bin="):
            llvm = Path(arg.partition("=")[2])
        elif arg.startswith("--llam-managed-plugin="):
            plugin_value = arg.partition("=")[2]
        elif arg == "-o":
            output = Path(next(args))
        elif arg.startswith("-o") and len(arg) > 2:
            output = Path(arg[2:])
        elif arg == "-c":
            compile_only = True
        elif arg in ("-I", "-D", "-U", "-include", "-isystem", "-iquote", "-idirafter", "-MF", "-MT", "-MQ"):
            compile_flags.extend((arg, next(args)))
        elif arg in ("-MD", "-MMD", "-MP"):
            compile_flags.append(arg)
        elif arg in ("-target", "--target", "-isysroot", "--sysroot", "-arch"):
            value = next(args)
            compile_flags.extend((arg, value))
            link_flags.extend((arg, value))
        elif arg in ("-L", "-l", "-framework", "-F"):
            link_flags.extend((arg, next(args)))
        elif (arg.startswith(("-Wl,", "-L", "-l", "-F", "-fuse-ld=")) or
              arg in ("-shared", "-dynamiclib", "-static", "-pie", "-rdynamic", "-pthread")):
            link_flags.append(arg)
            if arg == "-pthread":
                compile_flags.append(arg)
        elif arg.startswith(("--target=", "--sysroot=", "-mmacosx-version-min=", "-mios-version-min=")):
            compile_flags.append(arg)
            link_flags.append(arg)
        elif arg.endswith(".c") and not arg.startswith("-"):
            sources.append(Path(arg))
        elif (not arg.startswith("-") and
              (arg.endswith((".o", ".obj", ".a", ".so", ".dylib", ".lib")) or
               re.fullmatch(r".+\.so(?:\.[0-9]+)+", Path(arg).name))):
            objects.append(arg)
        elif arg.startswith(("-O", "-g", "-D", "-I", "-U", "-W", "-std=", "-m", "-f")):
            compile_flags.append(arg)
        else:
            raise VerificationError(f"unsupported driver argument: {arg}")
    forbidden = re.compile(r"(@|flto|fpass-plugin|fplugin|ffunction-sections|fdata-sections|"
                           r"fsanitize|fprofile|fpatchable-function|fno-pic|fno-PIC|"
                           r"--icf|--gc-sections|-dead_strip|--emit-relocs|--relax|--section-start|"
                           r"-order_file|--symbol-ordering-file|--script|-T,|--defsym|--wrap)")
    require(not any(forbidden.search(arg) for arg in arguments),
            "LTO, instrumentation, section rewriting, interposition and link folding require another qualified profile")
    require(bool(sources or objects), "no C sources or object inputs")
    require(not compile_only or (len(sources) == 1 and not objects), "-c requires exactly one C source")
    require(not any(arg == "-O0" for arg in compile_flags), "managed profile requires -O1 or higher")
    if not any(arg.startswith("-O") for arg in compile_flags):
        compile_flags.append("-O2")
    if llvm == Path("."):
        clang = shutil.which(os.environ.get("LLAM_CLANG", "clang"))
        require(clang is not None, "clang is unavailable")
        llvm = Path(clang).resolve().parent
    if not plugin_value:
        here = Path(__file__).resolve().parent
        prefix = here.parent.parent
        candidates = [here / "llam_managed_pass.so", here / "llam_managed_pass.dylib",
                      prefix / "build-preempt-tools" / "llam_managed_pass.so",
                      prefix / "build-preempt-tools" / "llam_managed_pass.dylib"]
        library_dirs = [prefix / "lib" / "llam", prefix / "lib64" / "llam",
                        *sorted(prefix.glob("lib/*/llam"))]
        candidates.extend(directory / ("llam_managed_pass" + suffix)
                          for directory in library_dirs for suffix in (".so", ".dylib"))
        plugin_value = str(next((p for p in candidates if p.is_file()), ""))
    plugin = Path(plugin_value)
    require(plugin.is_file(), "LLVM managed pass is unavailable; set LLAM_MANAGED_PLUGIN")
    if output is None:
        output = sources[0].with_suffix(".o") if compile_only else Path("a.out")
    require(output.parent.is_dir() and not output.is_symlink(), "output parent must exist; symlink output refused")
    return Invocation(mode, llvm, plugin.resolve(), output, compile_only, sources, objects,
                      compile_flags, link_flags)


def run(command: list[str], env: dict[str, str]) -> None:
    subprocess.run(command, env=env, check=True)


def compile_invocation(inv: Invocation) -> None:
    clang, llc, objdump = (inv.llvm / name for name in ("clang", "llc", "llvm-objdump"))
    for tool in (clang, llc, objdump):
        version = subprocess.run([str(tool), "--version"], check=True, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        require(re.search(r"\b23\.1\.2\b", version) is not None, f"{tool.name} must be LLVM 23.1.2")
    env = os.environ.copy()
    env.pop("LLAM_MANAGED_REQUIRED", None)
    if inv.mode == "required":
        env["LLAM_MANAGED_REQUIRED"] = "1"
    target_flags = []
    i = 0
    while i < len(inv.link_flags):
        arg = inv.link_flags[i]
        if arg in ("-target", "--target", "-arch"):
            target_flags.extend(inv.link_flags[i:i + 2])
            i += 1
        elif arg.startswith("--target="):
            target_flags.append(arg)
        i += 1
    triple = subprocess.run([str(clang), *target_flags, "-dumpmachine"], check=True, text=True,
                            stdout=subprocess.PIPE).stdout
    if "apple" in triple and sys.platform == "darwin" and not any(
            arg in ("-isysroot", "--sysroot") or arg.startswith("--sysroot=") for arg in inv.compile_flags):
        sdk = os.environ.get("SDKROOT") or subprocess.run(
            ["xcrun", "--show-sdk-path"], check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
        inv.compile_flags.extend(("-isysroot", sdk))
        inv.link_flags.extend(("-isysroot", sdk))
    # A complete frame pointer also avoids LLVM 23's unroundtrippable unnamed
    # CFI label on AArch64 frameless stack leaves. Keep the CFI and physical
    # prologue intact; never repair a witness by deleting unwind directives.
    profile_flags = ["-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer"]
    if "windows" not in triple and "mingw" not in triple:
        # TLS models and dso_local IR facts are selected by Clang, before llc's
        # relocation model takes effect. Compile-only objects must remain valid
        # inputs to the same qualified shared-image link path.
        profile_flags.append("-fPIC")
    if triple.startswith(("aarch64", "arm64")):
        # The admitted ARM profile includes LDAR/STLR and optional LSE, not the
        # newer RCpc encodings selected by some host CPU defaults. Their stronger
        # baseline equivalents retain C acquire/release semantics.
        profile_flags.extend(("-Xclang", "-target-feature", "-Xclang", "-rcpc",
                              "-Xclang", "-target-feature", "-Xclang", "-rcpc-immo"))
    with tempfile.TemporaryDirectory(prefix=".llam-managed-", dir=inv.output.parent) as temp:
        root = Path(temp)
        compiled = []
        for index, source in enumerate(inv.sources):
            ir, mir, labelled, obj = (root / f"{index}.{suffix}" for suffix in ("ll", "mir", "labelled.mir", "o"))
            run([str(clang), *inv.compile_flags, *profile_flags, "-fno-crash-diagnostics", "-fno-jump-tables",
                 "-fno-optimize-sibling-calls", "-fpass-plugin=" + str(inv.plugin),
                 "-S", "-emit-llvm", str(source), "-o", str(ir)], env)
            run([str(llc), "--enable-new-pm=0", "-O2", "-relocation-model=pic",
                 "-stop-after=unpack-mi-bundles", str(ir), "-o", str(mir)], env)
            run([str(llc), "--enable-new-pm=0", "-relocation-model=pic", "-load=" + str(inv.plugin),
                 "-run-pass=llam-managed-machine", "-verify-machineinstrs", str(mir), "-o", str(labelled)], env)
            run([str(llc), "--enable-new-pm=0", "-relocation-model=pic", "-start-after=unpack-mi-bundles",
                 "-verify-machineinstrs", "-filetype=obj", str(labelled), "-o", str(obj)], env)
            compiled.append(str(obj))
        if inv.compile_only:
            os.replace(compiled[0], inv.output)
            return
        result = root / ("linked-image.exe" if "windows" in triple or "mingw" in triple else "linked-image")
        platform_flags = []
        if "linux" in triple or "bsd" in triple:
            platform_flags.append("-Wl,--build-id=uuid")
            platform_flags.append("-Wl,-z,separate-code,-z,relro,-z,now")
            platform_flags.append("-Wl,--export-dynamic-symbol=__llam_managed_anchor_*")
        elif "windows" in triple or "mingw" in triple:
            platform_flags.append("-Wl,--build-id")
        elif "apple" in triple and not any(flag in ("-shared", "-dynamiclib") for flag in inv.link_flags):
            platform_flags.append("-Wl,-export_dynamic")
        run([str(clang), *compiled, *inv.objects, *inv.link_flags, *platform_flags,
             "-o", str(result)], env)
        report = verify(result, objdump, seal=True)
        require(inv.mode != "required" or report["managed_entries"] > 0, "no eligible managed entries")
        if report["format"] == "macho" and sys.platform == "darwin":
            run(["/usr/bin/codesign", "--force", "--sign", "-", str(result)], env)
        verify(result, objdump)
        os.replace(result, inv.output)


def main(arguments: list[str]) -> int:
    if arguments == ["--help"]:
        print(__doc__)
        print("usage: llam-managed-clang [--llam-managed=auto|required] [clang flags] sources/objects -o output")
        return 0
    try:
        compile_invocation(parse(arguments))
        return 0
    except (VerificationError, OSError, UnicodeError, StopIteration) as error:
        print(f"llam-managed-clang: {error}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as error:
        return error.returncode if error.returncode > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
