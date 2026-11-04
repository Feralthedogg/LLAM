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

"""Argument-forwarding and fail-closed mode tests for ``tools/llam-clang``."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "tools" / "llam-clang"


def write_executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)


def invoke(
    mode: str,
    *arguments: str,
    platform: str = "Linux",
    reject_plugin: bool = False,
) -> tuple[int, list[str], str]:
    with tempfile.TemporaryDirectory(prefix="llam-clang-test-") as temporary:
        root = Path(temporary)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        compiler = fake_bin / "fake-clang"
        uname = fake_bin / "uname"
        capture = root / "arguments.txt"
        required_capture = root / "required.txt"
        plugin = root / "llam_preempt_pass.so"
        plugin.write_bytes(b"fake pass plugin")
        write_executable(
            compiler,
            "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LLAM_CLANG_CAPTURE_FILE\"\nprintf '%s' \"${LLAM_PREEMPT_REQUIRED:-}\" > \"$LLAM_CLANG_REQUIRED_CAPTURE_FILE\"\ncase \"$*\" in *-fpass-plugin=*) "
            + ("exit 42" if reject_plugin else "exit 0")
            + ";; esac\nexit 0\n",
        )
        write_executable(
            uname,
            "#!/bin/sh\ncase \"$1\" in\n  -s) printf '%s\\n' \"${LLAM_TEST_UNAME_OS:-Linux}\" ;;\n  -m) printf '%s\\n' \"${LLAM_TEST_UNAME_ARCH:-x86_64}\" ;;\n  *) exit 2 ;;\nesac\n",
        )
        env = os.environ.copy()
        env["PATH"] = f"{fake_bin}{os.pathsep}{env.get('PATH', '')}"
        env["LLAM_CLANG"] = str(compiler)
        env["LLAM_PREEMPT_PLUGIN"] = str(plugin)
        env["LLAM_CLANG_CAPTURE_FILE"] = str(capture)
        env["LLAM_CLANG_REQUIRED_CAPTURE_FILE"] = str(required_capture)
        env["LLAM_TEST_UNAME_OS"] = platform
        env["LLAM_TEST_UNAME_ARCH"] = "x86_64"
        result = subprocess.run(
            [str(WRAPPER), f"--llam-preempt={mode}", *arguments],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        forwarded = capture.read_text(encoding="utf-8").splitlines() if capture.exists() else []
        required_value = required_capture.read_text(encoding="utf-8") if required_capture.exists() else ""
        return result.returncode, forwarded, required_value


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="llam-clang-default-") as temporary:
        root = Path(temporary)
        wrapper = root / "llam-clang"
        shutil.copyfile(WRAPPER, wrapper)
        wrapper.chmod(0o755)
        capture = root / "managed-arguments.txt"
        write_executable(root / "llam-managed-clang",
                         '#!/bin/sh\nprintf \'%s\\n\' "$@" > "$LLAM_DEFAULT_CAPTURE"\n')
        env = dict(os.environ, LLAM_DEFAULT_CAPTURE=str(capture))
        result = subprocess.run([str(wrapper), "-c", "tasks.c", "-o", "tasks.o"],
                                env=env, capture_output=True, text=True, check=False)
        if result.returncode != 0 or capture.read_text().splitlines() != [
                "--llam-managed=required", "-c", "tasks.c", "-o", "tasks.o"]:
            raise SystemExit("default compiler invocation did not require managed admission")

    rc, forwarded, required = invoke("off", "app.c", "-DVALUE=7", "-o", "app")
    if rc != 0 or forwarded != ["app.c", "-DVALUE=7", "-o", "app"] or required:
        raise SystemExit("off mode did not forward the original arguments unchanged")

    rc, forwarded, required = invoke("auto", "app.c", "-o", "app")
    if rc != 0 or "-fpass-plugin=" not in " ".join(forwarded) or "-O1" not in forwarded or required:
        raise SystemExit("auto mode did not add the available pass and an optimization level")

    rc, forwarded, required = invoke("required", "-c", "app.c", "-o", "app.o")
    if rc != 0 or "-fpass-plugin=" not in " ".join(forwarded) or required != "1":
        raise SystemExit("required mode did not set the pass-required environment contract")
    if "-c" not in forwarded or "app.o" not in forwarded:
        raise SystemExit("required compile-only mode did not preserve its output arguments")

    rc, forwarded, required = invoke("required", "-O0", "app.c", "-o", "app")
    if rc != 2 or forwarded or required:
        raise SystemExit("required mode silently accepted -O0")

    rc, forwarded, required = invoke("auto", "app.c", "-o", "app", reject_plugin=True)
    if rc != 0 or forwarded != ["app.c", "-o", "app"] or required:
        raise SystemExit("auto mode did not retry plain compilation after plugin rejection")

    rc, forwarded, required = invoke("auto", "app.c", "-o", "app", platform="Darwin")
    if rc != 0 or forwarded != ["app.c", "-o", "app"] or required:
        raise SystemExit("auto mode failed closed to plain compilation on an unsupported host")

    print("test_llam_clang: forwarding and required-mode checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
