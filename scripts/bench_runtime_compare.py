#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

import argparse
import csv
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass

from process_utils import ProcessTimeoutError, print_captured_output, run_capture
from safe_output import open_binary_for_write, open_text_for_write

try:
    import matplotlib.pyplot as plt
except ModuleNotFoundError:
    plt = None


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
DEFAULT_CASES = [
    "spawn_join",
    "channel_pingpong",
    "select_recv_ready",
    "select_park_wake",
    "select_timeout",
    "io_echo",
    "poll_wake",
    "sleep_fanout",
    "opaque_block",
]
NAME_ALIASES = {
    "poll_wake_approx": "poll_wake",
    "opaque_syscall_sleep_approx": "opaque_block",
    "opaque_block_in_place_approx": "opaque_block",
}


@dataclass
class BenchRow:
    runtime: str
    case: str
    ops_per_sec: float
    p50_us: float
    p99_us: float


def parse_fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def normalize_case(name: str) -> str:
    return NAME_ALIASES.get(name, name)


def parse_cases(value: str) -> list[str]:
    cases = [item.strip() for item in re.split(r"[,\s]+", value) if item.strip()]
    unknown = [case for case in cases if case not in DEFAULT_CASES]

    if unknown:
        raise argparse.ArgumentTypeError(f"unknown benchmark case(s): {', '.join(unknown)}")
    return cases


def parse_output(runtime: str, output: str) -> list[BenchRow]:
    rows: list[BenchRow] = []
    prefixes = {
        "LLAM": "[bench] ",
        "Goroutine": "[go-bench] ",
        "Tokio": "[tokio-bench] ",
    }
    prefix = prefixes[runtime]

    for line in output.splitlines():
        if not line.startswith(prefix):
            continue
        fields = parse_fields(line)
        if "name" not in fields or "ops_per_sec" not in fields:
            continue
        rows.append(
            BenchRow(
                runtime=runtime,
                case=normalize_case(fields["name"]),
                ops_per_sec=float(fields["ops_per_sec"]),
                p50_us=float(fields["p50_us"]),
                p99_us=float(fields["p99_us"]),
            )
        )
    return rows


def run_command(
    root: pathlib.Path,
    runtime: str,
    command: list[str],
    env: dict[str, str],
    timeout: int,
    sample: int,
    samples: int,
    case: str | None = None,
) -> list[BenchRow]:
    label = f"{runtime} {case}" if case is not None else runtime

    if samples > 1:
        print(f"[bench-runtime-compare] running {label} sample {sample}/{samples}", file=sys.stderr)
    else:
        print(f"[bench-runtime-compare] running {label}", file=sys.stderr)
    try:
        proc = run_capture(command, cwd=root, env=env, timeout=timeout)
    except ProcessTimeoutError as exc:
        print_captured_output(exc.stdout, exc.stderr)
        print(
            f"[bench-runtime-compare] {label} timed out after {timeout}s",
            file=sys.stderr,
        )
        raise SystemExit(124) from None
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)
    return parse_output(runtime, proc.stdout)


def select_sample_rows(samples: list[list[BenchRow]], policy: str) -> list[BenchRow]:
    grouped: dict[tuple[str, str], list[BenchRow]] = {}

    for rows in samples:
        for row in rows:
            grouped.setdefault((row.runtime, row.case), []).append(row)

    selected: list[BenchRow] = []
    for key in sorted(grouped):
        rows = sorted(grouped[key], key=lambda row: row.ops_per_sec)
        if policy == "best":
            selected.append(rows[-1])
        else:
            selected.append(rows[len(rows) // 2])
    return selected


def run_command_samples(
    root: pathlib.Path,
    runtime: str,
    command: list[str],
    env: dict[str, str],
    timeout: int,
    samples: int,
    policy: str,
    case: str | None = None,
) -> list[BenchRow]:
    sample_rows = [
        run_command(root, runtime, command, env, timeout, sample + 1, samples, case)
        for sample in range(samples)
    ]
    return select_sample_rows(sample_rows, policy)


def run_isolated_cases(
    root: pathlib.Path,
    runtime: str,
    command: list[str],
    base_env: dict[str, str],
    timeout: int,
    samples: int,
    policy: str,
    cases: list[str],
) -> list[BenchRow]:
    rows: list[BenchRow] = []

    for case in cases:
        env = base_env.copy()
        env["LLAM_BENCH_ONLY"] = case
        rows.extend(run_command_samples(root, runtime, command, env, timeout, samples, policy, case))
    return rows


def llam_bench_command(root: pathlib.Path, no_build: bool) -> list[str]:
    if os.name == "nt":
        build_dir = root / "build" / "bench-runtime-compare"
        if not no_build:
            subprocess.run(
                [
                    "cmake",
                    "-S",
                    ".",
                    "-B",
                    str(build_dir),
                    "-G",
                    "Visual Studio 17 2022",
                    "-A",
                    "x64",
                    "-DCMAKE_BUILD_TYPE=Release",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run(
                ["cmake", "--build", str(build_dir), "--config", "Release", "--target", "bench"],
                cwd=root,
                check=True,
            )
        return [str(build_dir / "Release" / "bench.exe")]

    if not no_build:
        subprocess.run(["make", "-j4", "bench"], cwd=root, check=True)
    return ["./bench"]


def write_csv(path: pathlib.Path, rows: list[BenchRow]) -> None:
    with open_text_for_write(path, newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["runtime", "case", "ops_per_sec", "p50_us", "p99_us"])
        for row in rows:
            writer.writerow([row.runtime, row.case, f"{row.ops_per_sec:.2f}", f"{row.p50_us:.2f}", f"{row.p99_us:.2f}"])


def print_summary(rows: list[BenchRow], cases: list[str]) -> None:
    by_key = {(row.runtime, row.case): row for row in rows}
    runtimes = [runtime for runtime in ["LLAM", "Goroutine", "Tokio"] if any(row.runtime == runtime for row in rows)]

    if runtimes != ["LLAM", "Goroutine", "Tokio"]:
        out_rows = []
        out_rows.append(["runtime", "case", "ops/s", "p50 us", "p99 us"])
        for case in cases:
            for runtime in runtimes:
                row = by_key.get((runtime, case))
                if row is None:
                    continue
                out_rows.append([runtime, case, f"{row.ops_per_sec:.2f}", f"{row.p50_us:.2f}", f"{row.p99_us:.2f}"])
        if len(out_rows) <= 1:
            return
        widths = [max(len(str(item)) for item in col) for col in zip(*out_rows)]
        w = [max(3, width) for width in widths]
        print(f"| {out_rows[0][0]:<{w[0]}} | {out_rows[0][1]:<{w[1]}} | {out_rows[0][2]:>{w[2]}} | {out_rows[0][3]:>{w[3]}} | {out_rows[0][4]:>{w[4]}} |")
        print(f"| {'-' * w[0]} | {'-' * w[1]} | {'-' * (w[2]-1)}: | {'-' * (w[3]-1)}: | {'-' * (w[4]-1)}: |")
        for row in out_rows[1:]:
            print(f"| {row[0]:<{w[0]}} | {row[1]:<{w[1]}} | {row[2]:>{w[2]}} | {row[3]:>{w[3]}} | {row[4]:>{w[4]}} |")
        return

    out_rows = []
    out_rows.append(["case", "LLAM ops/s", "Goroutine ops/s", "Tokio ops/s", "LLAM/Go", "LLAM/Tokio"])
    for case in cases:
        values = [by_key.get((runtime, case)) for runtime in runtimes]
        if any(value is None for value in values):
            continue
        llam, go, tokio = values
        assert llam is not None and go is not None and tokio is not None
        out_rows.append([
            case,
            f"{llam.ops_per_sec:.2f}",
            f"{go.ops_per_sec:.2f}",
            f"{tokio.ops_per_sec:.2f}",
            f"{llam.ops_per_sec / go.ops_per_sec:.2f}x",
            f"{llam.ops_per_sec / tokio.ops_per_sec:.2f}x"
        ])

    if len(out_rows) <= 1:
        return

    widths = [max(len(str(item)) for item in col) for col in zip(*out_rows)]
    w = [max(3, width) for width in widths]

    print(f"| {out_rows[0][0]:<{w[0]}} | {out_rows[0][1]:>{w[1]}} | {out_rows[0][2]:>{w[2]}} | {out_rows[0][3]:>{w[3]}} | {out_rows[0][4]:>{w[4]}} | {out_rows[0][5]:>{w[5]}} |")
    print(f"| {'-' * w[0]} | {'-' * (w[1]-1)}: | {'-' * (w[2]-1)}: | {'-' * (w[3]-1)}: | {'-' * (w[4]-1)}: | {'-' * (w[5]-1)}: |")
    for row in out_rows[1:]:
        print(f"| {row[0]:<{w[0]}} | {row[1]:>{w[1]}} | {row[2]:>{w[2]}} | {row[3]:>{w[3]}} | {row[4]:>{w[4]}} | {row[5]:>{w[5]}} |")


def grouped_bars(ax, cases: list[str], runtimes: list[str], values: dict[tuple[str, str], float], ylabel: str, title: str, log: bool = False) -> None:
    width = 0.25
    xs = list(range(len(cases)))
    offsets = {"LLAM": -width, "Goroutine": 0.0, "Tokio": width}
    colors = {"LLAM": "#0f766e", "Goroutine": "#2563eb", "Tokio": "#d97706"}

    for runtime in runtimes:
        ys = [values[(runtime, case)] for case in cases]
        ax.bar([x + offsets[runtime] for x in xs], ys, width=width, label=runtime, color=colors[runtime])
    ax.set_xticks(xs)
    ax.set_xticklabels(cases, rotation=25, ha="right")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.25)
    if log:
        ax.set_yscale("log")


def plot_graph(path: pathlib.Path, rows: list[BenchRow], cases: list[str]) -> None:
    if plt is None:
        print("[bench-runtime-compare] matplotlib not available; skipping graph", file=sys.stderr)
        return

    runtimes = ["LLAM", "Goroutine", "Tokio"]
    by_key = {(row.runtime, row.case): row for row in rows}
    complete_cases = [case for case in cases if all((runtime, case) in by_key for runtime in runtimes)]

    ops = {(runtime, case): by_key[(runtime, case)].ops_per_sec for runtime in runtimes for case in complete_cases}
    p50 = {(runtime, case): by_key[(runtime, case)].p50_us for runtime in runtimes for case in complete_cases}
    p99 = {(runtime, case): by_key[(runtime, case)].p99_us for runtime in runtimes for case in complete_cases}
    speedup_go = {("LLAM", case): by_key[("LLAM", case)].ops_per_sec / by_key[("Goroutine", case)].ops_per_sec for case in complete_cases}
    speedup_go.update({("Tokio", case): by_key[("Tokio", case)].ops_per_sec / by_key[("Goroutine", case)].ops_per_sec for case in complete_cases})

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, axes = plt.subplots(2, 2, figsize=(18, 11), constrained_layout=True)
    fig.suptitle("LLAM vs Goroutine vs Tokio Runtime Benchmarks", fontsize=18, fontweight="bold")

    grouped_bars(axes[0][0], complete_cases, runtimes, ops, "ops/sec (log)", "Throughput", log=True)
    grouped_bars(axes[0][1], complete_cases, runtimes, p50, "p50 us (log)", "Median Latency", log=True)
    grouped_bars(axes[1][0], complete_cases, runtimes, p99, "p99 us (log)", "Tail Latency", log=True)
    grouped_bars(axes[1][1], complete_cases, ["LLAM", "Tokio"], speedup_go, "x vs Goroutine", "Throughput Speedup vs Goroutine", log=False)
    axes[0][0].legend(loc="upper right")
    axes[1][1].axhline(1.0, color="#111827", linewidth=1, linestyle="--", alpha=0.6)

    graph_format = path.suffix.lower().lstrip(".") or "png"
    with open_binary_for_write(path) as handle:
        fig.savefig(handle, format=graph_format, dpi=180)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare LLAM, Goroutine, and Tokio benchmarks and render graphs.")
    parser.add_argument("--rounds", type=int, default=31)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--out-dir", default="object/bench_compare")
    parser.add_argument("--runtime", choices=["all", "llam", "go", "tokio"], default="all")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument(
        "--cases",
        nargs="+",
        default=None,
        help="comma separated, quoted whitespace separated, or repeated benchmark cases to run",
    )
    parser.add_argument(
        "--isolate-cases",
        action="store_true",
        help="run each benchmark case in a fresh process to avoid cross-case scheduler/CPU-state noise",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=int(os.environ.get("LLAM_BENCH_COMPARE_SAMPLES", "3")),
        help="number of process-level samples per runtime; median is reported by default",
    )
    parser.add_argument(
        "--sample-policy",
        choices=["median", "best"],
        default=os.environ.get("LLAM_BENCH_COMPARE_SAMPLE_POLICY", "median"),
        help="how to select one row per runtime/case when --samples is greater than 1",
    )
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be at least 1")
    cases = DEFAULT_CASES if args.cases is None else parse_cases(" ".join(args.cases))
    if not cases:
        parser.error("--cases must contain at least one case")

    root = pathlib.Path(__file__).resolve().parent.parent
    out_dir = root / args.out_dir
    selected_runtimes = {
        "all": ["LLAM", "Goroutine", "Tokio"],
        "llam": ["LLAM"],
        "go": ["Goroutine"],
        "tokio": ["Tokio"],
    }[args.runtime]
    env = os.environ.copy()
    env.update(
        {
            "LLAM_BENCH_ROUNDS": str(args.rounds),
            "LLAM_BENCH_WARMUP_ROUNDS": str(args.warmup),
            "LLAM_BENCH_SPAWN_TASKS": "512",
            "LLAM_BENCH_CHANNEL_MESSAGES": "4096",
            "LLAM_BENCH_SELECT_OPS": "4096",
            "LLAM_BENCH_IO_MESSAGES": "512",
            "LLAM_BENCH_POLL_EVENTS": "512",
            "LLAM_BENCH_SLEEP_TASKS": "1024",
            "LLAM_BENCH_OPAQUE_SCOPES": "64",
        }
    )

    llam_command: list[str] | None = None
    if "LLAM" in selected_runtimes:
        llam_command = llam_bench_command(root, args.no_build)
    if not args.no_build and "Tokio" in selected_runtimes:
        subprocess.run(
            ["cargo", "build", "--release", "--manifest-path", "scripts/bench_tokio_compare/Cargo.toml"],
            cwd=root,
            check=True,
        )

    rows: list[BenchRow] = []
    if "LLAM" in selected_runtimes:
        assert llam_command is not None
        if args.isolate_cases:
            rows.extend(
                run_isolated_cases(
                    root,
                    "LLAM",
                    llam_command,
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                    cases,
                )
            )
        else:
            rows.extend(
                run_command_samples(
                    root,
                    "LLAM",
                    llam_command,
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                )
            )
    if "Goroutine" in selected_runtimes:
        go_script = "scripts/bench_go_windows_compare.go" if os.name == "nt" else "scripts/bench_go_compare.go"
        if args.isolate_cases:
            rows.extend(
                run_isolated_cases(
                    root,
                    "Goroutine",
                    ["go", "run", go_script],
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                    cases,
                )
            )
        else:
            rows.extend(
                run_command_samples(
                    root,
                    "Goroutine",
                    ["go", "run", go_script],
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                )
            )
    if "Tokio" in selected_runtimes:
        tokio_command = [
            "cargo",
            "run",
            "--release",
            "--quiet",
            "--manifest-path",
            "scripts/bench_tokio_compare/Cargo.toml",
            "--bin",
            "bench_tokio_compare",
        ]
        if args.isolate_cases:
            rows.extend(
                run_isolated_cases(
                    root,
                    "Tokio",
                    tokio_command,
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                    cases,
                )
            )
        else:
            rows.extend(
                run_command_samples(
                    root,
                    "Tokio",
                    tokio_command,
                    env,
                    args.timeout,
                    args.samples,
                    args.sample_policy,
                )
            )

    csv_path = out_dir / "runtime_compare.csv"
    graph_path = out_dir / "runtime_compare.png"
    graph_written = False
    write_csv(csv_path, rows)
    if selected_runtimes == ["LLAM", "Goroutine", "Tokio"]:
        plot_graph(graph_path, rows, cases)
        graph_written = plt is not None
    else:
        print("[bench-runtime-compare] graph requires --runtime all; skipping graph", file=sys.stderr)
    print_summary(rows, cases)
    print(f"\nCSV: {csv_path}")
    if graph_written:
        print(f"Graph: {graph_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
