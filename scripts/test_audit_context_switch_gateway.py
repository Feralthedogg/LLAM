#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Feralthedogg
"""Tests for the C-level context-switch gateway audit."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from scripts.audit_context_switch_gateway import audit


class ContextSwitchGatewayAuditTests(unittest.TestCase):
    def write(self, root: Path, relative: str, text: str) -> None:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_accepts_gateway_context_implementation_and_declaration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(
                root,
                "src/core/base/errno.c",
                "void gateway(void) { llam_ctx_switch(0, 0); }\n",
            )
            self.write(
                root,
                "src/core/context/context_portable.c",
                "void llam_ctx_switch(void *a, void *b) { (void)a; (void)b; }\n",
            )
            self.write(
                root,
                "src/internal/llam_internal.h",
                "void llam_ctx_switch(void *a, void *b);\n",
            )

            diagnostics, count = audit(root)

            self.assertEqual(diagnostics, [])
            self.assertEqual(count, 1)

    def test_rejects_direct_call_from_another_runtime_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(
                root,
                "src/core/base/errno.c",
                "void gateway(void) { llam_ctx_switch(0, 0); }\n",
            )
            self.write(
                root,
                "src/core/task/yield.c",
                "void bad(void) { llam_ctx_switch(0, 0); }\n",
            )

            diagnostics, count = audit(root)

            self.assertEqual(count, 1)
            self.assertEqual(
                diagnostics,
                [
                    "direct context switch outside gateway: "
                    "src/core/task/yield.c:1"
                ],
            )

    def test_rejects_extra_call_from_context_implementation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(
                root,
                "src/core/base/errno.c",
                "void gateway(void) { llam_ctx_switch(0, 0); }\n",
            )
            self.write(
                root,
                "src/core/context/context_portable.c",
                "void llam_ctx_switch(void *a, void *b) {}\n"
                "void bypass(void) { llam_ctx_switch(0, 0); }\n",
            )

            diagnostics, count = audit(root)

            self.assertEqual(count, 1)
            self.assertEqual(
                diagnostics,
                [
                    "direct context switch outside gateway: "
                    "src/core/context/context_portable.c:2"
                ],
            )

    def test_requires_a_live_gateway_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write(root, "src/core/base/errno.c", "void empty(void) {}\n")

            diagnostics, count = audit(root)

            self.assertEqual(count, 0)
            self.assertEqual(
                diagnostics,
                [
                    "switch gateway has no context switch: "
                    "src/core/base/errno.c"
                ],
            )


if __name__ == "__main__":
    unittest.main()
