#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for chat-server stress correctness accounting."""

from __future__ import annotations

import stress_server
import stress_server_composite


class FakeClient:
    def __init__(self, index: int, data: bytes) -> None:
        self.index = index
        self.buffer = bytearray(data)


def fail(message: str) -> None:
    raise SystemExit(f"[test_stress_server_logic] {message}")


def test_broadcast_check_rejects_aggregate_duplicate_false_positive() -> None:
    payload = b"unique-message\n"
    clients = [
        FakeClient(0, payload + payload),
        FakeClient(1, b""),
        FakeClient(2, b""),
    ]

    try:
        stress_server.wait_for_broadcasts(
            clients,
            [(0, payload)],
            timeout=0.01,
        )
    except AssertionError:
        return
    fail("broadcast check accepted duplicate delivery to one client as full fanout")


def test_broadcast_check_accepts_per_recipient_delivery() -> None:
    payload = b"unique-message\n"
    clients = [
        FakeClient(0, b""),
        FakeClient(1, payload),
        FakeClient(2, payload),
    ]

    stress_server.wait_for_broadcasts(
        clients,
        [(0, payload)],
        timeout=0.01,
    )


def expect_metric_parser_rejects(line: str) -> None:
    try:
        stress_server_composite.parse_metric_fields(line)
    except ValueError:
        return
    fail(f"composite metric parser accepted malformed line: {line!r}")


def test_composite_metric_parser_rejects_ambiguous_fields() -> None:
    expect_metric_parser_rejects("server flood ok: sent=1 sent=999 delivery_mps=1.0")
    expect_metric_parser_rejects("server flood ok: =1 sent=1 delivery_mps=1.0")
    expect_metric_parser_rejects("server flood ok: sent= delivery_mps=1.0")


def test_composite_diagnostic_classifies_ambiguous_fields() -> None:
    diagnostic = stress_server_composite.diagnose_flood_result(
        "flood malformed",
        0,
        "server flood ok: sent=1 sent=999 delivery_mps=1.0\n",
        "",
    )
    if "class=malformed_metrics" not in diagnostic:
        fail(f"composite diagnostic did not classify malformed metrics: {diagnostic!r}")


def main() -> int:
    test_broadcast_check_rejects_aggregate_duplicate_false_positive()
    test_broadcast_check_accepts_per_recipient_delivery()
    test_composite_metric_parser_rejects_ambiguous_fields()
    test_composite_diagnostic_classifies_ambiguous_fields()
    print("[test_stress_server_logic] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
