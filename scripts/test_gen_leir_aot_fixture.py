#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg
"""Contract tests for deterministic LEIR AOT fixture generation."""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "gen_leir_aot_fixture.py"
CHECKED_IN = ROOT / "experiments" / "leir" / "generated"
TEMPLATE = (
    ROOT
    / "experiments"
    / "leir"
    / "leir_aot_connect_write_template.inc"
)
NEW_PROJECT_LICENSE = (
    "SPDX-License-Identifier: "
    "LicenseRef-LLAM-Commercial-Reciprocity-1.0"
)


class LeirAotFixtureGeneratorTests(unittest.TestCase):
    def run_generator(
        self,
        output_dir: Path,
        *extra: str,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(GENERATOR),
                "--output-dir",
                str(output_dir),
                *extra,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_generation_is_deterministic_and_current(self) -> None:
        # Break caught: compiler output changes with process state or the
        # checked-in interoperability fixture becomes stale.
        with tempfile.TemporaryDirectory() as first_raw, \
             tempfile.TemporaryDirectory() as second_raw:
            first = Path(first_raw)
            second = Path(second_raw)
            first_result = self.run_generator(first)
            second_result = self.run_generator(second)

            self.assertEqual(
                first_result.returncode, 0, first_result.stderr
            )
            self.assertEqual(
                second_result.returncode, 0, second_result.stderr
            )
            for name in (
                "leir_aot_connect_write.c",
                "leir_aot_connect_write.h",
            ):
                expected = (CHECKED_IN / name).read_bytes()
                self.assertEqual((first / name).read_bytes(), expected)
                self.assertEqual((second / name).read_bytes(), expected)

    def test_generated_path_has_no_completion_interpreter(self) -> None:
        # Break caught: code generation silently falls back to an opcode loop,
        # node-table walk, or hot-path allocator.
        with tempfile.TemporaryDirectory() as output_raw:
            output = Path(output_raw)
            result = self.run_generator(output)

            self.assertEqual(result.returncode, 0, result.stderr)
            source = (output / "leir_aot_connect_write.c").read_text()
            implementation = TEMPLATE.read_text()
            combined = source + implementation
            self.assertIn("0xca50ff7ddbf91222", source)
            for forbidden in (
                "switch (",
                "for (",
                "while (",
                "malloc(",
                "calloc(",
                "realloc(",
                "leir_phase0_program_t",
            ):
                self.assertNotIn(forbidden, combined)

    def test_generated_outputs_use_the_new_project_license(self) -> None:
        # Break caught: generated interoperability code silently inherits the
        # retired Apache header instead of the license selected for new files.
        with tempfile.TemporaryDirectory() as output_raw:
            output = Path(output_raw)
            result = self.run_generator(output)

            self.assertEqual(result.returncode, 0, result.stderr)
            for path in (
                output / "leir_aot_connect_write.c",
                output / "leir_aot_connect_write.h",
                TEMPLATE,
            ):
                header = "\n".join(
                    path.read_text(encoding="utf-8").splitlines()[:5]
                )
                self.assertIn(NEW_PROJECT_LICENSE, header, str(path))
                self.assertNotIn(
                    "SPDX-License-Identifier: Apache-2.0",
                    header,
                    str(path),
                )

    def test_generated_module_links_from_a_cpp_consumer(self) -> None:
        # Break caught: the language-neutral generated ABI leaks private C-only
        # runtime headers or gives generated C symbols C++ linkage.
        c_compiler = shutil.which(os.environ.get("CC", "cc"))
        cpp_compiler = shutil.which(os.environ.get("CXX", "c++"))
        if c_compiler is None or cpp_compiler is None:
            self.skipTest("C and C++ compilers are required")

        with tempfile.TemporaryDirectory() as output_raw:
            output = Path(output_raw)
            module_object = output / "module.o"
            consumer = output / "consumer.cc"
            executable = output / "consumer"
            consumer.write_text(
                '#include "generated/leir_aot_connect_write.h"\n'
                "int main() {\n"
                "    return "
                "leir_aot_connect_write_module_v1.abi_version == "
                "LEIR_AOT_MODULE_ABI_V1 && "
                "leir_aot_connect_write_module_v1.backend_kind == "
                "LEIR_AOT_MODULE_BACKEND_AGNOSTIC ? 0 : 1;\n"
                "}\n",
                encoding="utf-8",
            )
            compile_module = subprocess.run(
                [
                    c_compiler,
                    "-std=c11",
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(ROOT / "src"),
                    "-I",
                    str(ROOT / "src" / "internal"),
                    "-I",
                    str(ROOT / "experiments" / "leir"),
                    "-c",
                    str(CHECKED_IN / "leir_aot_connect_write.c"),
                    "-o",
                    str(module_object),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compile_module.returncode, 0, compile_module.stderr
            )
            link_consumer = subprocess.run(
                [
                    cpp_compiler,
                    "-std=c++17",
                    "-I",
                    str(ROOT / "include"),
                    "-I",
                    str(ROOT / "experiments" / "leir"),
                    str(consumer),
                    str(module_object),
                    "-o",
                    str(executable),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                link_consumer.returncode, 0, link_consumer.stderr
            )
            linked = subprocess.run(
                [str(executable)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(linked.returncode, 0, linked.stderr)

    def test_check_mode_rejects_stale_output(self) -> None:
        # Break caught: CI's stale-output check rewrites artifacts or accepts
        # a module that no longer matches the semantic digest.
        with tempfile.TemporaryDirectory() as output_raw:
            output = Path(output_raw)
            generated = self.run_generator(output)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            source = output / "leir_aot_connect_write.c"
            source.write_text(source.read_text() + "\n/* stale */\n")

            checked = self.run_generator(output, "--check")

            self.assertEqual(checked.returncode, 1)
            self.assertIn("stale generated file", checked.stderr)
            self.assertTrue(source.read_text().endswith("/* stale */\n"))


if __name__ == "__main__":
    unittest.main()
