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

"""Adversarial native-domain boundary audit and finite misuse exits."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import check_native_domain_boundary as boundary

NATIVE_BINARY: Path | None = None


class NativeBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources, cls.headers = boundary.load_sources(boundary.ROOT)

    def mutated(self, path: str, before: str, after: str) -> list[str]:
        sources = dict(self.sources)
        self.assertIn(before, sources[path])
        sources[path] = sources[path].replace(before, after, 1)
        return boundary.audit(sources, self.headers)

    def test_repository(self) -> None:
        self.assertEqual(boundary.audit(self.sources, self.headers), [])

    def test_repeated_audit_rechecks_changed_source_content(self) -> None:
        self.assertEqual(boundary.audit(self.sources, self.headers), [])
        self.assertTrue(self.mutated("src/core/task/yield_join_sleep.c", "LLAM_NATIVE_API_GUARD();", ""))
        self.assertEqual(boundary.audit(self.sources, self.headers), [])

    def test_repeated_audit_rechecks_current_review_policy(self) -> None:
        self.assertEqual(boundary.audit(self.sources, self.headers), [])
        key = ("src/core/base/abi.c", "llam_version_string")
        with patch.dict(boundary.REVIEWED, {key: "0" * 64}):
            errors = boundary.audit(self.sources, self.headers)
            self.assertTrue(any("reviewed native-domain contract changed: llam_version_string" in e
                                for e in errors))
        self.assertEqual(boundary.audit(self.sources, self.headers), [])

    def test_source_inventory_uses_utf8_and_portable_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("src/core/example.c", "src/internal/runtime_state.h",
                         "src/internal/managed_domain.h"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("/* native → managed */\n", encoding="utf-8")
            for name in boundary.exports.PUBLIC_HEADERS:
                path = root / "include/llam" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("/* public → ABI */\n", encoding="utf-8")
            sources, headers = boundary.load_sources(root)
            self.assertEqual(set(sources), {"src/core/example.c", "src/internal/runtime_state.h",
                                            "src/internal/managed_domain.h"})
            self.assertIn("→", sources["src/core/example.c"])
            self.assertTrue(all("→" in source for source in headers.values()))

    def test_region_owner_validation_cannot_be_removed(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c",
                                    "worker->preempt_region_depth) { errno = ENOTSUP; return NULL; }",
                                    "0U) { errno = ENOTSUP; return NULL; }"))

    def test_region_must_publish_the_native_guard(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c",
                                    "llam_native_domain_enter_worker(worker);\n    if (worker->preempt_region_depth",
                                    "if (worker->preempt_region_depth"))

    def test_region_leave_preserves_pending_preemption(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c",
                                    "if (outermost) llam_task_safepoint();", "(void)outermost;"))

    def test_removed_entry_gate(self) -> None:
        self.assertTrue(self.mutated("src/core/task/yield_join_sleep.c", "LLAM_NATIVE_API_GUARD();", ""))

    def test_tls_before_entry_gate(self) -> None:
        self.assertTrue(self.mutated("src/core/task/yield_join_sleep.c", "LLAM_NATIVE_API_GUARD();",
                                    "llam_task_t *early = g_llam_tls_task; LLAM_NATIVE_API_GUARD();"))

    def test_creation_gate_must_not_admit_false_branch(self) -> None:
        self.assertTrue(self.mutated("src/core/registry/handle.c",
                                    "LLAM_UNLIKELY(llam_native_domain_blocked())",
                                    "LLAM_UNLIKELY(llam_native_domain_blocked()) && 0"))

    def test_creation_output_not_reset_on_successful_error_return(self) -> None:
        self.assertTrue(self.mutated("src/core/registry/handle.c", "*out = NULL;", "*out = runtime;"))

    def test_guard_macro_cannot_be_disabled(self) -> None:
        self.assertTrue(self.mutated("src/internal/runtime_state.h", "return failure;", "return failure + 0;"))

    def test_local_guard_override(self) -> None:
        self.assertTrue(self.mutated("src/core/task/spawn.c", '#include "runtime_internal.h"',
                                    '#include "runtime_internal.h"\n#undef LLAM_NATIVE_API_GUARD\n'
                                    '#define LLAM_NATIVE_API_GUARD(x) ((void)0)'))

    def test_changed_depth_and_probe_budgets(self) -> None:
        for before, after in (("DEPTH_MAX 65535U", "DEPTH_MAX 4294967295U"),
                              ("PROBES 64U", "PROBES 4096U")):
            with self.subTest(before=before):
                self.assertTrue(self.mutated("src/core/preempt/native_domain.c", before, after))

    def test_guard_cannot_acquire_a_lock(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c",
                                    "if (worker == NULL) return;", "pthread_mutex_lock(&some_lock); if (worker == NULL) return;"))

    def test_binding_must_probe_conditional_lock_free_atomics(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c",
                                    "!atomic_is_lock_free(&g_native_domain_slots[0].native_id)", "0"))

    def test_syscall_assembly_is_part_of_the_reviewed_contract(self) -> None:
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c", '"svc #0x80"', '"svc #0x81"'))
        self.assertTrue(self.mutated("src/core/preempt/native_domain.c", '"r8", "r9", "r10"', '"r8", "r9"'))

    def test_pure_api_cannot_start_using_tls(self) -> None:
        self.assertTrue(self.mutated("src/core/base/abi.c", "return LLAM_ABI_VERSION;",
                                    "if (g_llam_tls_task) return 0; return LLAM_ABI_VERSION;"))

    def test_clock_cache_publication_requires_review(self) -> None:
        self.assertTrue(self.mutated("src/core/time/time.c",
                                    "atomic_load_explicit(&timebase_ratio, memory_order_acquire)",
                                    "atomic_load_explicit(&timebase_ratio, memory_order_relaxed)"))

    def test_domain_predicate_cannot_inherit_original_g(self) -> None:
        self.assertTrue(self.mutated("src/internal/managed_domain.h",
                                    "llam_native_domain_guarded_worker() != NULL",
                                    "llam_native_domain_guarded_worker() == NULL"))

    def test_new_api_requires_a_gate(self) -> None:
        headers, sources = dict(self.headers), dict(self.sources)
        headers["runtime.h"] += "\nLLAM_API int llam_new_stateful_api(void);\n"
        sources["src/core/api/new.c"] = "int llam_new_stateful_api(void) { return 0; }\n"
        self.assertTrue(boundary.audit(sources, headers))
        sources["src/core/api/new.c"] = "int llam_new_stateful_api(void) { LLAM_NATIVE_API_GUARD(-1); return 0; }\n"
        self.assertEqual(boundary.audit(sources, headers), [])

    def test_comments_and_formatting_do_not_change_a_review(self) -> None:
        self.assertEqual(self.mutated("src/core/base/abi.c", "return LLAM_ABI_VERSION;",
                                     "/* Same constant. */ return\n LLAM_ABI_VERSION ;"), [])

    def test_finite_native_misuse_exits(self) -> None:
        if NATIVE_BINARY is None:
            self.skipTest("pass --binary to run finite native misuse cases")
        for mode in ("--bad-leave", "--depth-limit"):
            with self.subTest(mode=mode):
                result = subprocess.run([str(NATIVE_BINARY), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 125, result.stdout + result.stderr)


def main() -> int:
    global NATIVE_BINARY
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    options, remaining = parser.parse_known_args()
    NATIVE_BINARY = options.binary.resolve() if options.binary is not None else None
    return not unittest.main(argv=[sys.argv[0], *remaining], exit=False).result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(main())
