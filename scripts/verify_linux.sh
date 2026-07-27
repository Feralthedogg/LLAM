#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$(uname -s)" != "Linux" ]; then
    echo "verify_linux.sh is intended for Linux" >&2
    exit 1
fi

JOBS="${JOBS:-4}"
make clean
make -j"$JOBS" all test bench_leir_native_segment

python3 - <<'PY'
import sys

sys.path.insert(0, "scripts")

from process_utils import ProcessTimeoutError, print_captured_output, run_capture

command = [
    "./bench_leir_native_segment",
    "--candidate", "link_skip",
    "--ops", "4",
    "--concurrency", "4",
    "--payload", "64",
    "--activations", "8",
    "--min-mode-ms", "1",
    "--order", "ABBA",
]
try:
    probe = run_capture(
        command,
        timeout=30,
        max_output_bytes=64 * 1024,
    )
except ProcessTimeoutError as exc:
    print_captured_output(exc.stdout, exc.stderr)
    print(
        "verify_linux.sh: Linux io_uring native segment "
        "probe timed out after 30s",
        file=sys.stderr,
    )
    sys.exit(124)

if probe.stdout_truncated or probe.stderr_truncated:
    print(
        "verify_linux.sh: Linux io_uring native segment "
        "probe exceeded the output cap",
        file=sys.stderr,
    )
    sys.exit(1)
if probe.returncode == 0:
    print(
        "verify_linux.sh: Linux io_uring native segment "
        "available"
    )
    print_captured_output(probe.stdout, probe.stderr)
elif probe.returncode == 77:
    print(
        "verify_linux.sh: SKIP Linux io_uring native "
        "segment unavailable"
    )
    print_captured_output(probe.stdout, probe.stderr)
else:
    print_captured_output(probe.stdout, probe.stderr)
    sys.exit(probe.returncode)
PY

python3 - <<'PY'
import os
import sys

sys.path.insert(0, "scripts")

from process_utils import ProcessTimeoutError, print_captured_output, run_capture


def run(cmd, timeout, env=None):
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    try:
        proc = run_capture(
            cmd,
            env=merged_env,
            timeout=timeout,
            stderr_to_stdout=True,
            max_output_bytes=256 * 1024,
        )
    except ProcessTimeoutError as exc:
        print_captured_output(exc.stdout, exc.stderr)
        print(f"verify_linux.sh: command timed out after {timeout}s: {' '.join(cmd)}", file=sys.stderr)
        sys.exit(124)
    print_captured_output(proc.stdout, proc.stderr)
    if proc.stdout_truncated or proc.stderr_truncated:
        print(
            "verify_linux.sh: command exceeded output cap: "
            f"{' '.join(cmd)}",
            file=sys.stderr,
        )
        sys.exit(1)
    if proc.returncode != 0:
        sys.exit(proc.returncode)


run(["./demo"], timeout=30)
run(["./bench"], timeout=120, env={
    "LLAM_BENCH_ROUNDS": "1",
    "LLAM_BENCH_SELECT_OPS": "128",
    "LLAM_BENCH_IO_MESSAGES": "32",
    "LLAM_BENCH_POLL_EVENTS": "32",
    "LLAM_BENCH_SLEEP_TASKS": "128",
    "LLAM_BENCH_OPAQUE_SCOPES": "4",
})
run(["./stress"], timeout=90, env={
    "LLAM_STRESS_ROUNDS": "1",
    "LLAM_STRESS_DYNAMIC_ROUNDS": "1",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS": "32",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_ROUNDS": "256",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_US": "1000",
})

if os.environ.get("LLAM_VERIFY_LINUX_EXPERIMENTAL") == "1":
    run(["./stress"], timeout=90, env={
        "LLAM_STRESS_ROUNDS": "1",
        "LLAM_STRESS_DETERMINISTIC_PHASE": "0",
        "LLAM_STRESS_DYNAMIC_PHASE": "1",
        "LLAM_EXPERIMENTAL_WORKER_RINGS": "1",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS": "32",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_ROUNDS": "256",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_US": "1000",
    })
    run(["./bench"], timeout=120, env={
        "LLAM_BENCH_ROUNDS": "1",
        "LLAM_BENCH_IO_MESSAGES": "32",
        "LLAM_BENCH_POLL_EVENTS": "32",
        "LLAM_EXPERIMENTAL_SQPOLL": "1",
    })
PY
