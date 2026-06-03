#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for C test environment parsing helpers."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    raise SystemExit(f"[test_c_env_helpers] {message}")


def build_probe(tmp: Path) -> Path:
    source = tmp / "env_probe.c"
    binary = tmp / "env_probe"
    source.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                '#include "tests/test_env.h"',
                "int main(void) {",
                '    printf("%u\\n", llam_test_env_u32("LLAM_PROBE_U32", 48U, 99U));',
                '    printf("%llu\\n", (unsigned long long)llam_test_env_u64("LLAM_PROBE_U64", 123ULL));',
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(ROOT), source, "-o", binary],
        cwd=ROOT,
        check=True,
    )
    return binary


def build_server_support_probe(tmp: Path) -> Path:
    source = tmp / "server_support_probe.c"
    binary = tmp / "server_support_probe"
    source.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                '#include "examples/server_support.h"',
                "int main(void) {",
                '    printf("%d\\n", chat_env_enabled("0", true));',
                '    printf("%d\\n", chat_env_enabled("false", true));',
                '    printf("%d\\n", chat_env_enabled("False", true));',
                '    printf("%d\\n", chat_env_enabled("no", true));',
                '    printf("%d\\n", chat_env_enabled("OFF", true));',
                '    printf("%d\\n", chat_env_enabled("1", false));',
                '    printf("%d\\n", chat_env_enabled("true", false));',
                '    printf("%d\\n", chat_env_enabled("Yes", false));',
                '    printf("%d\\n", chat_env_enabled("on", false));',
                '    printf("%d\\n", chat_env_enabled("maybe", false));',
                '    printf("%d\\n", chat_env_enabled("maybe", true));',
                '    printf("%u\\n", (unsigned)chat_parse_port(NULL, 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("8080", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("+8080", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port(" +8080", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port(" -1", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("-1", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("0", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("65536", 1234U));',
                '    printf("%u\\n", (unsigned)chat_parse_port("80x", 1234U));',
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-D_GNU_SOURCE",
            "-D_XOPEN_SOURCE=700",
            "-D_DARWIN_C_SOURCE",
            "-I",
            str(ROOT),
            "-I",
            str(ROOT / "examples"),
            source,
            ROOT / "examples" / "server_support.c",
            ROOT / "examples" / "diagnostic_output.c",
            "-o",
            binary,
        ],
        cwd=ROOT,
        check=True,
    )
    return binary


def build_example_flag_probe(tmp: Path) -> Path:
    source = tmp / "example_flag_probe.c"
    binary = tmp / "example_flag_probe"
    source.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                '#include "examples/env_compat.h"',
                "static const char *g_probe_value;",
                "const char *llam_env_get(const char *name) {",
                '    (void)name;',
                "    return g_probe_value;",
                "}",
                "static void probe(const char *value, unsigned default_value) {",
                "    g_probe_value = value;",
                '    printf("%u\\n", llam_example_env_flag_default("LLAM_PROBE_FLAG", default_value));',
                "}",
                "int main(void) {",
                "    probe(NULL, 1U);",
                '    probe("", 0U);',
                '    probe("0", 1U);',
                '    probe("false", 1U);',
                '    probe("False", 1U);',
                '    probe("no", 1U);',
                '    probe("OFF", 1U);',
                '    probe("1", 0U);',
                '    probe("true", 0U);',
                '    probe("Yes", 0U);',
                '    probe("on", 0U);',
                '    probe("maybe", 0U);',
                '    probe("maybe", 1U);',
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", str(ROOT), source, "-o", binary],
        cwd=ROOT,
        check=True,
    )
    return binary


def build_server_flood_stats_probe(tmp: Path) -> Path:
    source = tmp / "server_flood_stats_probe.c"
    binary = tmp / "server_flood_stats_probe"
    source.write_text(
        "\n".join(
            [
                "#include <stdio.h>",
                '#include "examples/server_flood_stats.h"',
                "int main(int argc, char **argv) {",
                "    for (int i = 1; i < argc; ++i) {",
                "        flood_server_stats_t stats;",
                '        printf("%d\\n", flood_read_server_stats(argv[i], &stats) ? 1 : 0);',
                "    }",
                "    return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    cc = os.environ.get("CC", "cc")
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-D_GNU_SOURCE",
            "-D_XOPEN_SOURCE=700",
            "-D_DARWIN_C_SOURCE",
            "-I",
            str(ROOT),
            "-I",
            str(ROOT / "examples"),
            source,
            ROOT / "examples" / "server_flood_stats.c",
            "-o",
            binary,
        ],
        cwd=ROOT,
        check=True,
    )
    return binary


def run_probe(binary: Path, u32: str | None, u64: str | None) -> tuple[str, str]:
    env = os.environ.copy()
    env.pop("LLAM_PROBE_U32", None)
    env.pop("LLAM_PROBE_U64", None)
    if u32 is not None:
        env["LLAM_PROBE_U32"] = u32
    if u64 is not None:
        env["LLAM_PROBE_U64"] = u64
    proc = subprocess.run([binary], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    lines = proc.stdout.splitlines()
    if len(lines) != 2:
        fail(f"unexpected probe output: stdout={proc.stdout!r} stderr={proc.stderr!r}")
    return lines[0], lines[1]


def expect(binary: Path, u32: str | None, u64: str | None, want_u32: str, want_u64: str) -> None:
    got_u32, got_u64 = run_probe(binary, u32, u64)
    if got_u32 != want_u32 or got_u64 != want_u64:
        fail(
            "env parse mismatch "
            f"u32={u32!r} u64={u64!r} got=({got_u32}, {got_u64}) want=({want_u32}, {want_u64})"
        )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="llam-c-env-test-") as raw_tmp:
        tmp = Path(raw_tmp)
        binary = build_probe(tmp)
        expect(binary, None, None, "48", "123")
        expect(binary, "42", "777", "42", "777")
        expect(binary, "0x11", "0x1122334455667788", "17", "1234605616436508552")
        expect(binary, "0", "0", "48", "123")
        expect(binary, "", "", "48", "123")
        expect(binary, "bad", "bad", "48", "123")
        expect(binary, "-1", "-1", "48", "123")
        expect(binary, "+7", "+7", "48", "123")
        expect(binary, " +7", " +7", "48", "123")
        expect(binary, " -1", " -1", "48", "123")
        expect(binary, "100000", "184467440737095516150", "99", "123")
        server_probe = build_server_support_probe(tmp)
        proc = subprocess.run([server_probe], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        lines = proc.stdout.splitlines()
        expected = [
            "0",
            "0",
            "0",
            "0",
            "0",
            "1",
            "1",
            "1",
            "1",
            "0",
            "1",
            "1234",
            "1234",
            "8080",
            "0",
            "0",
            "0",
            "0",
            "0",
            "0",
            "0",
        ]
        if lines != expected:
            fail(f"server support parsing mismatch: got={lines!r} want={expected!r}")
        example_probe = build_example_flag_probe(tmp)
        proc = subprocess.run([example_probe], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        lines = proc.stdout.splitlines()
        expected = ["1", "0", "0", "0", "0", "0", "0", "1", "1", "1", "1", "0", "1"]
        if lines != expected:
            fail(f"example env flag boolean parsing mismatch: got={lines!r} want={expected!r}")
        stats_probe = build_server_flood_stats_probe(tmp)
        valid_stats = tmp / "valid-stats.txt"
        huge_drop_stats = tmp / "huge-drop-stats.txt"
        max_representable_stats = tmp / "max-representable-stats.txt"
        signed_overflow_stats = tmp / "signed-overflow-stats.txt"
        consistent_huge_stats = tmp / "consistent-huge-stats.txt"
        overflow_drop_stats = tmp / "overflow-drop-stats.txt"
        impossible_enqueue_stats = tmp / "impossible-enqueue-stats.txt"
        prefixed_key_stats = tmp / "prefixed-key-stats.txt"
        duplicate_key_stats = tmp / "duplicate-key-stats.txt"
        valid_stats.write_text(
            "server stopped; outbox_full_drops=2 outbox_closed_drops=1 "
            "broadcast_messages_created=4 broadcast_deliveries_attempted=10 "
            "broadcast_deliveries_enqueued=7\n",
            encoding="utf-8",
        )
        huge_drop_stats.write_text(
            "server stopped; outbox_full_drops=18446744073709551615 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1\n",
            encoding="utf-8",
        )
        max_representable_stats.write_text(
            "server stopped; outbox_full_drops=0 outbox_closed_drops=0 "
            "broadcast_messages_created=9223372036854775807 "
            "broadcast_deliveries_attempted=9223372036854775807 "
            "broadcast_deliveries_enqueued=9223372036854775807\n",
            encoding="utf-8",
        )
        signed_overflow_stats.write_text(
            "server stopped; outbox_full_drops=0 outbox_closed_drops=0 "
            "broadcast_messages_created=9223372036854775808 "
            "broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1\n",
            encoding="utf-8",
        )
        consistent_huge_stats.write_text(
            "server stopped; outbox_full_drops=18446744073709551615 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=18446744073709551615 "
            "broadcast_deliveries_enqueued=0\n",
            encoding="utf-8",
        )
        overflow_drop_stats.write_text(
            "server stopped; outbox_full_drops=18446744073709551615 outbox_closed_drops=1 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=0 "
            "broadcast_deliveries_enqueued=0\n",
            encoding="utf-8",
        )
        impossible_enqueue_stats.write_text(
            "server stopped; outbox_full_drops=0 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=2\n",
            encoding="utf-8",
        )
        prefixed_key_stats.write_text(
            "server stopped; xoutbox_full_drops=0 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1\n",
            encoding="utf-8",
        )
        duplicate_key_stats.write_text(
            "server stopped; outbox_full_drops=0 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1 outbox_full_drops=99\n",
            encoding="utf-8",
        )
        proc = subprocess.run(
            [
                stats_probe,
                valid_stats,
                huge_drop_stats,
                max_representable_stats,
                signed_overflow_stats,
                consistent_huge_stats,
                overflow_drop_stats,
                impossible_enqueue_stats,
                prefixed_key_stats,
                duplicate_key_stats,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        lines = proc.stdout.splitlines()
        expected = ["1", "0", "1", "0", "0", "0", "0", "0", "0"]
        if lines != expected:
            fail(f"server flood stats consistency mismatch: got={lines!r} want={expected!r}")
    print("[test_c_env_helpers] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
