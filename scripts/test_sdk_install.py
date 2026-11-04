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

"""Consume a real CMake installation after prefix override and relocation."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def run(command, **kwargs):
    result = subprocess.run(command, text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=120, **kwargs)
    if result.returncode:
        raise AssertionError(f"{command!r}\n{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="llam-sdk-install-") as temporary:
        root = Path(temporary)
        installed = root / "installed"
        moved = root / "relocated"
        targets = ["llam_runtime", "llam_runtime_shared", "llam_preempt_metadata_inspect"]
        if os.name != "nt":
            targets += ["demo", "stress", "bench", "server", "server_lossless", "server_flood"]
        run([args.cmake, "--build", str(args.build_dir), "--config", args.config,
             "--target", *targets, "-j", "4"])
        run([args.cmake, "--install", str(args.build_dir), "--config", args.config,
             "--prefix", str(installed)])
        shutil.move(installed, moved)
        pc_files = list(moved.rglob("llam.pc"))
        if len(pc_files) != 1:
            raise AssertionError("installation must contain one pkg-config file")
        configs = list(moved.rglob("llam-config.cmake"))
        if len(configs) != 1:
            raise AssertionError("installation must contain one CMake package config")
        config_dir = configs[0].parent
        pc = pc_files[0]
        library_dir = pc.parent.parent
        env = os.environ.copy()
        env["PKG_CONFIG_LIBDIR"] = str(pc.parent)
        env["PKG_CONFIG_PATH"] = ""
        env["PKG_CONFIG_SYSROOT_DIR"] = ""
        env["PATH"] = str(moved / "bin") + os.pathsep + env.get("PATH", "")

        consumer = root / "consumer"
        consumer.mkdir()
        source = consumer / "main.c"
        source.write_text(
            "#include <llam/runtime.h>\n"
            "static void task(void *arg) { *(int *)arg = 1; }\n"
            "int main(void) { llam_runtime_opts_t opts; llam_runtime_t *rt; llam_task_t *t; int ran = 0; "
            "if (llam_runtime_opts_init(&opts, sizeof(opts))) return 1; opts.deterministic = 1; "
            "if (llam_runtime_create_compat(&opts, sizeof(opts), &rt)) return 2; "
            "t = llam_runtime_spawn_ex(rt, task, &ran, 0, 0); "
            "if (!t || llam_runtime_run_handle(rt) || llam_join(t)) return 3; "
            "llam_runtime_destroy(rt); return ran != 1; }\n",
            encoding="utf-8",
        )
        (consumer / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(sdk_consumer C)\n"
            f'find_package(llam 3.0.0 EXACT CONFIG REQUIRED PATHS "{config_dir.as_posix()}" NO_DEFAULT_PATH)\n'
            "add_executable(sdk_consumer main.c)\n"
            "target_link_libraries(sdk_consumer PRIVATE llam::runtime)\n",
            encoding="utf-8",
        )
        consumer_build = root / "consumer-build"
        cache = (args.build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
        generator = next(line.split("=", 1)[1] for line in cache.splitlines()
                         if line.startswith("CMAKE_GENERATOR:INTERNAL="))
        compiler = next((line.split("=", 1)[1] for line in cache.splitlines()
                         if line.startswith("CMAKE_C_COMPILER:")), None)
        configure = [args.cmake, "-S", str(consumer), "-B", str(consumer_build), "-G", generator,
                     f"-DCMAKE_BUILD_TYPE={args.config}"]
        if compiler:
            configure.append(f"-DCMAKE_C_COMPILER={compiler}")
        platform = next((line.split("=", 1)[1] for line in cache.splitlines()
                         if line.startswith("CMAKE_GENERATOR_PLATFORM:INTERNAL=")), "")
        if platform:
            configure += ["-A", platform]
        run(configure, env=env)
        run([args.cmake, "--build", str(consumer_build), "--config", args.config], env=env)
        executables = [p for p in consumer_build.rglob("sdk_consumer*")
                       if p.is_file() and p.name in ("sdk_consumer", "sdk_consumer.exe")]
        if len(executables) != 1:
            raise AssertionError("CMake consumer executable missing")
        run([str(executables[0])], env=env)

        pkg_config = shutil.which("pkg-config")
        if os.name != "nt" and pkg_config:
            if compiler is None:
                raise AssertionError("configured C compiler missing from CMake cache")
            for variable, expected in (("prefix", moved), ("libdir", library_dir),
                                       ("includedir", moved / "include")):
                actual = Path(run([pkg_config, f"--variable={variable}", "llam"], env=env)).resolve()
                if actual != expected.resolve():
                    raise AssertionError(f"pkg-config {variable}: {actual} != {expected}")
            flags = shlex.split(run([pkg_config, "--cflags", "--libs", "llam"], env=env))
            executable = root / "pkg_consumer"
            run([compiler, str(source), *flags, f"-Wl,-rpath,{library_dir}", "-o", str(executable)], env=env)
            run([str(executable)], env=env)
        else:
            print("pkg-config compilation skipped: unavailable on this host")

    print("test_sdk_install: relocated CMake and pkg-config consumers passed")


if __name__ == "__main__":
    main()
