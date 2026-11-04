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

"""Exercise relocated installed managed tooling against a real shared runtime."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(command: list[str], *, env: dict[str, str], success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=60)
    if success and result.returncode:
        raise AssertionError(f"{command!r}\n{result.stdout}\n{result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError(f"unexpected acceptance: {command!r}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--llvm-bin", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    if os.name == "nt":
        print("test_managed_sdk: native Windows shell launcher is not qualified", file=sys.stderr)
        return 77
    env = os.environ.copy()
    for name in ("LLAM_CLANG", "LLAM_MANAGED_PLUGIN", "LLAM_PREEMPT_PLUGIN", "LLAM_MANAGED_REQUIRED",
                 "LLAM_PREEMPT_REQUIRED", "LLAM_ASYNC_REQUIRED", "LLAM_PREEMPT_METADATA_INSPECT",
                 "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        env.pop(name, None)
    env.update(LLAM_LLVM_BIN=str(args.llvm_bin.resolve()), PYTHONDONTWRITEBYTECODE="1")
    clang = str(args.llvm_bin.resolve() / "clang")
    runtime = args.runtime.resolve()
    if not runtime.is_file():
        raise AssertionError("shared runtime target must be built before the SDK consumer test")
    native_flags = []
    if sys.platform == "darwin":
        native_flags = ["-isysroot", run(["xcrun", "--show-sdk-path"], env=env).stdout.strip()]
    with tempfile.TemporaryDirectory(prefix=".llam-managed-sdk-", dir=ROOT) as temporary:
        work = Path(temporary)
        prefix = work / "installed"
        run([args.cmake, "--install", str(args.build_dir.resolve()), "--config", args.config,
             "--prefix", str(prefix)], env=env)
        relocated = work / "relocated sdk"
        prefix.rename(relocated)
        installed_runtimes = list(relocated.rglob(runtime.name))
        includes = list(relocated.rglob("include/llam/managed.h"))
        if len(installed_runtimes) != 1 or len(includes) != 1:
            raise AssertionError("complete installed SDK must contain its public headers and shared runtime")
        runtime = installed_runtimes[0].resolve(strict=True)
        include_root = includes[0].parent.parent
        if not runtime.is_relative_to(relocated):
            raise AssertionError("installed runtime escapes the relocated SDK")
        if any(not header.resolve(strict=True).is_relative_to(relocated)
               for header in (include_root / "llam").glob("*.h")):
            raise AssertionError("installed headers escape the relocated SDK")
        drivers = list(relocated.rglob("llam-clang"))
        if len(drivers) != 1:
            raise AssertionError("installed SDK must contain one combined llam-clang driver")
        driver = str(drivers[0])
        launchers = list(relocated.rglob("llam-managed-clang"))
        plugins = [*relocated.rglob("llam_managed_pass.so"), *relocated.rglob("llam_managed_pass.dylib")]
        if len(launchers) != 1 or len(plugins) != 1:
            raise AssertionError("managed launcher/plugin was omitted from PreemptTools")
        runtime_contracts = list(relocated.rglob("managed_runtime_contracts.def"))
        verifiers = list(relocated.rglob("managed_verify.py"))
        if len(runtime_contracts) != 1 or len(verifiers) != 1:
            raise AssertionError("installed verifier/runtime contract data is incomplete")

        task = work / "task.c"
        task.write_text('#include "llam/managed.h"\n'
                        'extern void *sdk_block(void *);extern unsigned long sdk_mix(unsigned long);\n'
                        'void sdk_task(void *arg){unsigned long *value=arg;void *result=0;\n'
                        'if(llam_call_blocking_result(sdk_block,arg,&result)||result!=arg){*value=0;return;}\n'
                        '*value=LLAM_MANAGED_TARGET(sdk_mix)(*value);}\n')
        helper = work / "helper.c"
        helper.write_text('unsigned long sdk_mix(unsigned long n){return n*3+1;}\n')
        consumer = work / "consumer.c"
        consumer.write_text('#include "llam/managed.h"\n#include <stdio.h>\n'
            'void sdk_task(void *);void *sdk_block(void *arg){++*(unsigned long*)arg;return arg;}\n'
            'int main(void){llam_runtime_opts_t ro;llam_preempt_info_t info;\n'
            'llam_runtime_t *rt=0;unsigned long value=7;\n'
            'if(llam_runtime_opts_init(&ro,sizeof(ro)))return 1;ro.deterministic=1;\n'
            'if(llam_runtime_create(&ro,sizeof(ro),&rt)){perror("runtime_create");return 2;}\n'
            'if(llam_runtime_preempt_info_ex(rt,&info,sizeof(info))||!info.registered_modules||info.registered_entries<2)return 3;\n'
            'if(info.requested_policy!=LLAM_PREEMPT_POLICY_ASYNC_REQUIRED||info.compatibility_processors)return 7;\n'
            'llam_task_t *task=llam_runtime_spawn_ex(rt,sdk_task,&value,0,0);\n'
            'if(!task){perror("managed_spawn");return 4;}\n'
            'if(llam_runtime_run_handle(rt)){perror("managed_run");return 5;}\n'
            'if(llam_join(task)){perror("managed_join");return 6;}\n'
            'llam_runtime_destroy(rt);return value!=25;}\n')
        include = ["-I", str(include_root)]
        host_object = consumer.with_suffix(".o")
        # The installed off mode remains an exact native-compiler escape hatch.
        off_env = {**env, "LLAM_CLANG": clang}
        run([driver, "--llam-managed=off", *native_flags, *include, "-O2", "-c", str(consumer),
             "-o", str(host_object)], env=off_env)
        objects = []
        for source in (task, helper):
            output = source.with_suffix(".o")
            run([driver, *include, "-c", str(source), "-o", str(output)], env=env)
            objects.append(str(output))
        image = work / "sdk-consumer"
        run([driver, *objects, str(host_object), str(runtime),
             "-Wl,-rpath," + str(runtime.parent), "-o", str(image)], env=env)
        run([sys.executable, str(verifiers[0]), str(image), "--objdump", str(args.llvm_bin.resolve() / "llvm-objdump")], env=env)
        run([str(image)], env=env)

        unknown = work / "unknown.c"
        unknown.write_text('extern long unknown_native(long);long entry(long n){return unknown_native(n);}\n')
        rejected = unknown.with_suffix(".o")
        failure = run([driver, "-c", str(unknown), "-o", str(rejected)], env=env, success=False)
        if "unqualified" not in failure.stderr or rejected.exists():
            raise AssertionError("installed required mode failed without preserving the effect rejection contract")
        legacy = work / "legacy.o"
        run([driver, "--llam-managed=auto", "-c", str(unknown), "-o", str(legacy)], env=env)
        run([driver, "--llam-managed=required", "--llam-preempt=off", "-c", str(helper),
             "-o", str(work / "conflict.o")], env=env, success=False)
        # Loss of the installed plugin is an error even in auto mode; no plain
        # retry may disguise an installation failure as a managed compilation.
        unavailable = plugins[0].with_suffix(plugins[0].suffix + ".unavailable")
        plugins[0].rename(unavailable)
        failure = run([driver, "--llam-managed=auto", "-c", str(helper),
                       "-o", str(work / "missing-plugin.o")], env=env, success=False)
        if "managed pass is unavailable" not in failure.stderr:
            raise AssertionError(f"missing-plugin diagnostic changed: {failure.stderr}")
        if list(work.rglob(".llam-managed-*")):
            raise AssertionError("compiler left temporary pipeline directories")
    print("test_managed_sdk: relocated installed headers/runtime/driver, cross-TU closure, real blocking callback and rejection checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
