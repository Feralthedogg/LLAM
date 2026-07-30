#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
"""Unit tests for linked hardening-marker inspection."""

from __future__ import annotations

import unittest

from scripts.audit_hardening_artifact import audit_elf, audit_macho, audit_pe


class HardeningArtifactAuditTests(unittest.TestCase):
    def test_accepts_complete_elf_projection(self) -> None:
        program_headers = """
  GNU_STACK      0x000000 0x000000 0x000000 0x0 0x0 RW  0x10
  GNU_RELRO      0x001000 0x001000 0x001000 0x100 0x100 R 0x1
"""
        dynamic = "0x000000000000001e (FLAGS) BIND_NOW\n"
        symbols = "UND __stack_chk_fail@GLIBC_2.4\n"

        self.assertEqual(audit_elf(program_headers, dynamic, symbols), [])

    def test_rejects_executable_or_partial_elf_projection(self) -> None:
        program_headers = (
            "GNU_STACK 0x0 0x0 0x0 0x0 0x0 RWE 0x10\n"
        )

        self.assertEqual(
            audit_elf(program_headers, "", ""),
            [
                "non-executable GNU_STACK",
                "GNU_RELRO",
                "BIND_NOW",
                "stack-protector reference",
            ],
        )

    def test_accepts_hardened_macho_projection(self) -> None:
        self.assertEqual(
            audit_macho(
                "Mach header\nPIE NOUNDEFS DYLDLINK TWOLEVEL\n",
                "                 U ___stack_chk_fail\n",
            ),
            [],
        )

    def test_rejects_executable_stack_and_missing_canary(self) -> None:
        self.assertEqual(
            audit_macho("MH_ALLOW_STACK_EXECUTION\n", ""),
            ["non-executable stack", "stack-protector reference"],
        )

    def test_accepts_hardened_pe_projection(self) -> None:
        headers = """
            Dynamic base
            NX compatible
            Guard CF
"""
        symbols = "__security_check_cookie\n"

        self.assertEqual(audit_pe(headers, symbols), [])

    def test_accepts_llvm_pe_projection(self) -> None:
        headers = """
          IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE
          IMAGE_DLL_CHARACTERISTICS_NX_COMPAT
          IMAGE_DLL_CHARACTERISTICS_GUARD_CF
"""

        self.assertEqual(
            audit_pe(headers, "__security_cookie\n"),
            [],
        )

    def test_rejects_partial_pe_projection(self) -> None:
        self.assertEqual(
            audit_pe("Dynamic base\n", ""),
            [
                "NX-compatible image",
                "Control Flow Guard",
                "stack-protector reference",
            ],
        )


if __name__ == "__main__":
    unittest.main()
