#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import math
import os
import pathlib
import re
import subprocess
import sys
from typing import Any

from cli_numbers import nonnegative_int_at_most, positive_int_at_most
from process_utils import ProcessTimeoutError, run_capture
from safe_output import write_text_safely


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
MAX_BENCH_MATRIX_ROUNDS = 10_000
MAX_BENCH_MATRIX_TIMEOUT_SEC = 3600
MAX_BENCH_MATRIX_SPIN_NS = 1_000_000_000
MAX_BENCH_MATRIX_SPIN_ITERS = 10_000_000


def parse_value(raw: str) -> Any:
    if re.fullmatch(r"-?\d+", raw):
        return int(raw)
    if re.fullmatch(r"-?\d+\.\d+", raw):
        return float(raw)
    return raw


def parse_fields(line: str) -> dict[str, Any]:
    fields: dict[str, Any] = {}

    for key, value in FIELD_RE.findall(line):
        if key in fields:
            raise ValueError(f"duplicate benchmark field {key}: {line!r}")
        fields[key] = parse_value(value)
    return fields


def require_finite_metric(fields: dict[str, Any], line: str, name: str, *, positive: bool) -> float:
    if name not in fields:
        raise ValueError(f"benchmark row missing {name}: {line!r}")
    try:
        value = float(fields[name])
    except (TypeError, ValueError) as exc:
        raise ValueError(f"benchmark row has invalid {name}: {line!r}") from exc
    if not math.isfinite(value):
        raise ValueError(f"benchmark row has non-finite {name}: {line!r}")
    if positive and value <= 0.0:
        raise ValueError(f"benchmark row has non-positive {name}: {line!r}")
    if not positive and value < 0.0:
        raise ValueError(f"benchmark row has negative {name}: {line!r}")
    fields[name] = value
    return value


def validate_workload_fields(fields: dict[str, Any], line: str) -> None:
    require_finite_metric(fields, line, "ops_per_sec", positive=True)
    require_finite_metric(fields, line, "p50_us", positive=False)
    require_finite_metric(fields, line, "p99_us", positive=False)
    require_finite_metric(fields, line, "io_submit_syscalls", positive=False)


def parse_bench_output(output: str) -> dict[str, Any]:
    config: dict[str, Any] | None = None
    workloads: dict[str, dict[str, Any]] = {}

    for line in output.splitlines():
        if not line.startswith("[bench] "):
            continue
        fields = parse_fields(line)
        if "name" in fields:
            name = str(fields.pop("name"))
            if name in workloads:
                raise ValueError(f"duplicate benchmark row for {name}: {line!r}")
            validate_workload_fields(fields, line)
            workloads[name] = fields
        elif "config" in line:
            if config is not None:
                raise ValueError(f"duplicate benchmark config row: {line!r}")
            config = fields

    if config is None or not workloads:
        raise ValueError("failed to parse bench output")
    return {"config": config, "workloads": workloads}


def format_delta(current: float, baseline: float, suffix: str = "%") -> str:
    if baseline == 0:
        return "n/a"
    delta = (current / baseline - 1.0) * 100.0
    return f"{delta:+.1f}{suffix}"


def format_float(value: float) -> str:
    return f"{value:.2f}"


def geom_mean(values: list[float]) -> float:
    positive = [value for value in values if value > 0.0]
    if not positive:
        return 0.0
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def field_value(values: dict[str, Any], name: str, default: Any = 0) -> Any:
    legacy = {
        "dynamic_workers": "dynamic_shards",
        "worker_rings": "shard_rings",
        "worker_rings_multishot": "shard_rings_multishot",
        "online_workers": "online_shards",
        "online_workers_floor": "online_shards_floor",
        "online_workers_min": "online_shards_min",
        "online_workers_max": "online_shards_max",
    }
    if name in values:
        return values[name]
    return values.get(legacy.get(name, ""), default)


def print_markdown_table(headers: list[str], rows: list[list[str]]) -> None:
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for row in rows:
        print("| " + " | ".join(row) + " |")


def run_profile(root: pathlib.Path, profile_name: str, env_overrides: dict[str, str], rounds: int, timeout_sec: int) -> dict[str, Any]:
    env = os.environ.copy()
    env["LLAM_BENCH_ROUNDS"] = str(rounds)
    env.update(env_overrides)

    command = [os.environ.get("SHELL", "/bin/sh"), "-lc", "./bench"]
    try:
        proc = run_capture(command, cwd=root, env=env, timeout=timeout_sec)
    except ProcessTimeoutError as exc:
        return {
            "profile": profile_name,
            "env": env_overrides,
            "stdout": exc.stdout,
            "stderr": exc.stderr,
            "returncode": 124,
            "status": "failed",
            "error": f"timeout after {timeout_sec}s",
        }
    result: dict[str, Any] = {
        "profile": profile_name,
        "env": env_overrides,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "returncode": proc.returncode,
    }
    if proc.returncode != 0:
        result["status"] = "failed"
        result["error"] = f"exit code {proc.returncode}"
        return result

    try:
        parsed = parse_bench_output(proc.stdout)
    except ValueError as exc:
        result["status"] = "parse_failed"
        result["error"] = str(exc)
        return result

    result.update(parsed)
    result["status"] = "ok"
    return result


def default_sqpoll_cpu() -> str | None:
    if not hasattr(os, "sched_getaffinity"):
        return None
    try:
        cpus = sorted(os.sched_getaffinity(0))
    except OSError:
        return None
    if len(cpus) <= 1:
        return None
    return str(cpus[-1])


def default_profiles(args: argparse.Namespace) -> dict[str, dict[str, str]]:
    spin_env = {
        "LLAM_IDLE_SPIN_NS": str(args.spin_ns),
        "LLAM_IDLE_SPIN_ITERS": str(args.spin_iters),
    }
    profiles = {
        "baseline": {"LLAM_EXPERIMENTAL_DYNAMIC_WORKERS": "1"},
        "dynamic": {"LLAM_EXPERIMENTAL_DYNAMIC_WORKERS": "1"},
        "legacy_dynamic": {"LLAM_EXPERIMENTAL_DYNAMIC_WORKERS": "0"},
        "legacy_normq": {"LLAM_EXPERIMENTAL_LOCKFREE_NORMQ": "0"},
        "spin": spin_env,
        "sqpoll": {"LLAM_EXPERIMENTAL_SQPOLL": "1"},
        "lockfree": {"LLAM_EXPERIMENTAL_LOCKFREE_NORMQ": "1"},
        "lockfree+sqpoll": {
            "LLAM_EXPERIMENTAL_LOCKFREE_NORMQ": "1",
            "LLAM_EXPERIMENTAL_SQPOLL": "1",
        },
        "worker_rings": {"LLAM_EXPERIMENTAL_WORKER_RINGS": "1"},
        "worker_rings+multishot": {
            "LLAM_EXPERIMENTAL_WORKER_RINGS": "1",
            "LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT": "1",
        },
        "huge": {"LLAM_EXPERIMENTAL_HUGE_ALLOC": "1"},
        "worker_rings+lockfree": {
            "LLAM_EXPERIMENTAL_WORKER_RINGS": "1",
            "LLAM_EXPERIMENTAL_LOCKFREE_NORMQ": "1",
        },
    }
    reserved_sqpoll_cpu = default_sqpoll_cpu()
    if reserved_sqpoll_cpu is not None:
        profiles["sqpoll_reserved"] = {
            "LLAM_EXPERIMENTAL_SQPOLL": "1",
            "LLAM_SQPOLL_CPU": reserved_sqpoll_cpu,
        }
    return profiles


def parse_profile_names(raw: str) -> list[str]:
    profiles = [name.strip() for name in raw.split(",") if name.strip()]
    seen: set[str] = set()
    duplicates: list[str] = []

    for name in profiles:
        if name in seen and name not in duplicates:
            duplicates.append(name)
        seen.add(name)

    if not profiles:
        raise argparse.ArgumentTypeError("must contain at least one profile")
    if duplicates:
        raise argparse.ArgumentTypeError(f"duplicate profile(s): {', '.join(duplicates)}")
    return profiles


def profile_verdict(result: dict[str, Any], baseline: dict[str, Any]) -> tuple[str, str]:
    if result["profile"] == "baseline":
        return ("baseline", "reference")
    if result.get("status") != "ok":
        return ("hold", str(result.get("error", "failed")))

    baseline_workloads = baseline["workloads"]
    bad_reasons: list[str] = []
    good_reasons: list[str] = []
    baseline_io_syscalls = 0.0
    profile_io_syscalls = 0.0

    for workload_name, metrics in result["workloads"].items():
        base_metrics = baseline_workloads[workload_name]
        ops_ratio = float(metrics["ops_per_sec"]) / float(base_metrics["ops_per_sec"])
        p99_ratio = float(base_metrics["p99_us"]) / float(metrics["p99_us"])
        baseline_io_syscalls += float(base_metrics["io_submit_syscalls"])
        profile_io_syscalls += float(metrics["io_submit_syscalls"])
        if ops_ratio < 0.95:
            bad_reasons.append(f"{workload_name} ops")
        if p99_ratio < 0.95:
            bad_reasons.append(f"{workload_name} p99")
        if ops_ratio > 1.05:
            good_reasons.append(f"{workload_name} ops")
        if p99_ratio > 1.05:
            good_reasons.append(f"{workload_name} p99")

    if bad_reasons:
        return ("hold", ", ".join(bad_reasons[:3]))
    if profile_io_syscalls + 1.0 < baseline_io_syscalls or good_reasons:
        return ("candidate", ", ".join(good_reasons[:3]) or "lower submit syscalls")
    return ("neutral", "no clear gain")


def summarize_profiles(results: list[dict[str, Any]]) -> None:
    baseline = next(result for result in results if result["profile"] == "baseline")
    if baseline.get("status") != "ok":
        raise RuntimeError(f"baseline profile failed: {baseline.get('error', 'unknown error')}")
    baseline_workloads = baseline["workloads"]

    summary_rows: list[list[str]] = []
    for result in results:
        verdict, detail = profile_verdict(result, baseline)
        if result.get("status") != "ok":
            summary_rows.append(
                [
                    str(result["profile"]),
                    str(result.get("status")),
                    verdict,
                    "-",
                    "-",
                    "-",
                    str(field_value(result.get("config", {}), "dynamic_workers", 0)),
                    str(result.get("config", {}).get("lockfree_normq", 0)),
                    str(result.get("config", {}).get("sqpoll", 0)),
                    str(result.get("config", {}).get("sqpoll_cpu", -1)),
                    str(field_value(result.get("config", {}), "worker_rings", 0)),
                    str(field_value(result.get("config", {}), "worker_rings_multishot", 0)),
                    detail,
                ]
            )
            continue
        ops_ratios: list[float] = []
        p99_ratios: list[float] = []
        io_syscalls = 0.0

        for workload_name, metrics in result["workloads"].items():
            base_metrics = baseline_workloads[workload_name]
            ops_ratios.append(float(metrics["ops_per_sec"]) / float(base_metrics["ops_per_sec"]))
            p99_ratios.append(float(base_metrics["p99_us"]) / float(metrics["p99_us"]))
            io_syscalls += float(metrics["io_submit_syscalls"])

        summary_rows.append(
            [
                str(result["profile"]),
                str(result["status"]),
                verdict,
                format_float(geom_mean(ops_ratios)),
                format_float(geom_mean(p99_ratios)),
                format_float(io_syscalls),
                str(field_value(result["config"], "dynamic_workers", 0)),
                str(result["config"].get("lockfree_normq", 0)),
                str(result["config"].get("sqpoll", 0)),
                str(result["config"].get("sqpoll_cpu", -1)),
                str(field_value(result["config"], "worker_rings", 0)),
                str(field_value(result["config"], "worker_rings_multishot", 0)),
                detail,
            ]
        )

    print("## Profile Summary")
    print_markdown_table(
        ["profile", "status", "verdict", "ops gm", "p99 gm", "io submit syscalls", "dynamic", "lockfree", "sqpoll", "sqpoll_cpu", "worker_rings", "worker_ms", "detail"],
        summary_rows,
    )
    print()

    for workload_name in baseline_workloads:
        rows: list[list[str]] = []
        base_metrics = baseline_workloads[workload_name]
        for result in results:
            if result.get("status") != "ok":
                rows.append(
                    [
                        str(result["profile"]),
                        "failed",
                        "-",
                        "-",
                        "-",
                        "-",
                        "-",
                        "-",
                        "-",
                        "-",
                        "-",
                    ]
                )
                continue
            metrics = result["workloads"][workload_name]
            rows.append(
                [
                    str(result["profile"]),
                    format_float(float(metrics["ops_per_sec"])),
                    format_delta(float(metrics["ops_per_sec"]), float(base_metrics["ops_per_sec"])),
                    format_float(float(metrics["p50_us"])),
                    format_delta(float(base_metrics["p50_us"]), float(metrics["p50_us"])),
                    format_float(float(metrics["p99_us"])),
                    format_delta(float(base_metrics["p99_us"]), float(metrics["p99_us"])),
                    str(int(metrics["io_submit_syscalls"])),
                    str(int(field_value(metrics, "online_workers_floor", field_value(metrics, "online_workers", 0)))),
                    str(int(field_value(metrics, "online_workers_min", field_value(metrics, "online_workers", 0)))),
                    str(int(field_value(metrics, "online_workers_max", field_value(metrics, "online_workers", 0)))),
                ]
            )

        print(f"## {workload_name}")
        print_markdown_table(
            ["profile", "ops/s", "ops delta", "p50 us", "p50 delta", "p99 us", "p99 delta", "submit syscalls", "online floor", "online min", "online max"],
            rows,
        )
        print()


def main() -> int:
    parser = argparse.ArgumentParser(description="Run LLAM bench across a matrix of runtime profiles.")
    parser.add_argument("--rounds", type=positive_int_at_most(MAX_BENCH_MATRIX_ROUNDS), default=7, help="Value for LLAM_BENCH_ROUNDS.")
    parser.add_argument("--timeout", type=positive_int_at_most(MAX_BENCH_MATRIX_TIMEOUT_SEC), default=60, help="Per-profile timeout in seconds.")
    parser.add_argument("--spin-ns", type=nonnegative_int_at_most(MAX_BENCH_MATRIX_SPIN_NS), default=50000, help="Spin budget for the 'spin' profile.")
    parser.add_argument("--spin-iters", type=nonnegative_int_at_most(MAX_BENCH_MATRIX_SPIN_ITERS), default=10000, help="Spin iteration cap for the 'spin' profile.")
    parser.add_argument(
        "--profiles",
        type=parse_profile_names,
        default=parse_profile_names("baseline,legacy_dynamic,legacy_normq,spin,sqpoll,worker_rings,worker_rings+multishot,huge"),
        help="Comma-separated profile list.",
    )
    parser.add_argument("--json-out", help="Optional path to write raw results as JSON.")
    parser.add_argument("--no-build", action="store_true", help="Skip 'make bench'.")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    profile_map = default_profiles(args)
    selected_profiles = args.profiles

    for name in selected_profiles:
        if name not in profile_map:
            raise SystemExit(f"unknown profile: {name}")

    if not args.no_build:
        subprocess.run(["make", "-j4", "bench"], cwd=root, check=True)

    results: list[dict[str, Any]] = []
    for name in selected_profiles:
        print(f"[bench-matrix] running profile={name}", file=sys.stderr)
        results.append(run_profile(root, name, profile_map[name], args.rounds, args.timeout))

    summarize_profiles(results)

    if args.json_out:
        out_path = pathlib.Path(args.json_out)
        write_text_safely(out_path, json.dumps(results, indent=2))
        print(f"[bench-matrix] wrote {out_path}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
