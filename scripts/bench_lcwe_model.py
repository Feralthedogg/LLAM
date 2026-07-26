#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run and classify the standalone LCWE Phase 1 cost model."""

from __future__ import annotations

import math
import re
from dataclasses import dataclass


RESULT_PREFIX = "[lcwe-model] "
WORKLOADS = {
    "exec_io_pipeline",
    "exec_rpc_state",
    "exec_event_fanout",
}
MODES = {
    "scalar",
    "cohort",
    "wave_pointers",
    "wave_capsule",
    "wave_aosoa",
}
LANE_WIDTHS = {1, 2, 4, 8, 16, 32}
INTEGER_FIELDS = {
    "instances",
    "sites",
    "lanes",
    "rounds",
    "warmup",
    "ops",
    "wall_ns",
    "cpu_ns",
}
FLOAT_FIELDS = {
    "wall_ns_per_op",
    "cpu_ns_per_op",
    "p50_ns_per_op",
    "p99_ns_per_op",
}
EXPECTED_FIELDS = {
    "workload",
    "mode",
    *INTEGER_FIELDS,
    *FLOAT_FIELDS,
    "checksum",
}
DECIMAL_RE = re.compile(r"[0-9]+")
FIXED_FLOAT_RE = re.compile(r"[0-9]+\.[0-9]{2}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")


@dataclass(frozen=True)
class ModelRow:
    workload: str
    mode: str
    instances: int
    sites: int
    lanes: int
    rounds: int
    warmup: int
    ops: int
    wall_ns: int
    cpu_ns: int
    wall_ns_per_op: float
    cpu_ns_per_op: float
    p50_ns_per_op: float
    p99_ns_per_op: float
    checksum: str


def _split_result(output: str) -> dict[str, str]:
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LCWE model result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LCWE model result row")

    fields: dict[str, str] = {}
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LCWE model field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(f"duplicate or empty LCWE model field: {key}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(f"LCWE model field mismatch: missing={missing} extra={extra}")
    return fields


def _parse_integer(name: str, value: str) -> int:
    if DECIMAL_RE.fullmatch(value) is None:
        raise ValueError(f"invalid integer field {name}")
    return int(value, 10)


def _parse_float(name: str, value: str) -> float:
    if FIXED_FLOAT_RE.fullmatch(value) is None:
        raise ValueError(f"invalid fixed-point field {name}")
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0.0:
        raise ValueError(f"non-positive or non-finite field {name}")
    return parsed


def parse_output(output: str) -> ModelRow:
    fields = _split_result(output)
    integers = {
        name: _parse_integer(name, fields[name]) for name in INTEGER_FIELDS
    }
    floats = {
        name: _parse_float(name, fields[name]) for name in FLOAT_FIELDS
    }

    if fields["workload"] not in WORKLOADS:
        raise ValueError("unknown workload")
    if fields["mode"] not in MODES:
        raise ValueError("unknown mode")
    if not (1 <= integers["instances"] <= 16_777_216):
        raise ValueError("instances out of range")
    if not (1 <= integers["sites"] <= 32):
        raise ValueError("sites out of range")
    if integers["lanes"] not in LANE_WIDTHS:
        raise ValueError("unsupported lane width")
    if not (1 <= integers["rounds"] <= 1_000_000):
        raise ValueError("rounds out of range")
    if not (0 <= integers["warmup"] <= 100_000):
        raise ValueError("warmup out of range")
    if integers["warmup"] >= integers["rounds"]:
        raise ValueError("warmup must be smaller than rounds")
    if fields["mode"] == "wave_aosoa" and integers["sites"] != 1:
        raise ValueError("wave_aosoa requires one site")

    measured_rounds = integers["rounds"] - integers["warmup"]
    expected_ops = integers["instances"] * measured_rounds
    if integers["ops"] != expected_ops:
        raise ValueError("operation count mismatch")
    if integers["wall_ns"] <= 0 or integers["cpu_ns"] <= 0:
        raise ValueError("non-positive aggregate time")
    if abs(
        floats["wall_ns_per_op"]
        - integers["wall_ns"] / integers["ops"]
    ) > 0.011:
        raise ValueError("wall ns/op mismatch")
    if abs(
        floats["cpu_ns_per_op"]
        - integers["cpu_ns"] / integers["ops"]
    ) > 0.011:
        raise ValueError("CPU ns/op mismatch")
    if floats["p99_ns_per_op"] < floats["p50_ns_per_op"]:
        raise ValueError("p99 smaller than p50")
    if CHECKSUM_RE.fullmatch(fields["checksum"]) is None:
        raise ValueError("invalid checksum")

    return ModelRow(
        workload=fields["workload"],
        mode=fields["mode"],
        instances=integers["instances"],
        sites=integers["sites"],
        lanes=integers["lanes"],
        rounds=integers["rounds"],
        warmup=integers["warmup"],
        ops=integers["ops"],
        wall_ns=integers["wall_ns"],
        cpu_ns=integers["cpu_ns"],
        wall_ns_per_op=floats["wall_ns_per_op"],
        cpu_ns_per_op=floats["cpu_ns_per_op"],
        p50_ns_per_op=floats["p50_ns_per_op"],
        p99_ns_per_op=floats["p99_ns_per_op"],
        checksum=fields["checksum"],
    )
