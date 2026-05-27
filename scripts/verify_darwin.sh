#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$(uname -s)" != "Darwin" ]; then
    echo "verify_darwin.sh is intended for Darwin/macOS" >&2
    exit 1
fi

make clean
make -j4 all test
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
        proc = run_capture(cmd, env=merged_env, timeout=timeout, stderr_to_stdout=True)
    except ProcessTimeoutError as exc:
        print_captured_output(exc.stdout, exc.stderr)
        print(f"verify_darwin.sh: command timed out after {timeout}s: {' '.join(cmd)}", file=sys.stderr)
        sys.exit(124)
    print_captured_output(proc.stdout, proc.stderr)
    if proc.returncode != 0:
        sys.exit(proc.returncode)

run(["./demo"], timeout=30)
run(["./bench"], timeout=120, env={
    "LLAM_BENCH_ROUNDS": "1",
    "LLAM_BENCH_IO_MESSAGES": "32",
    "LLAM_BENCH_POLL_EVENTS": "32",
})
run(["./stress"], timeout=60, env={
    "LLAM_STRESS_ROUNDS": "1",
    "LLAM_STRESS_DYNAMIC_ROUNDS": "1",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS": "32",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_ROUNDS": "256",
    "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_US": "1000",
})
if os.environ.get("LLAM_VERIFY_DARWIN_EXPERIMENTAL") == "1":
    run(["./stress"], timeout=60, env={
        "LLAM_STRESS_ROUNDS": "1",
        "LLAM_STRESS_DETERMINISTIC_PHASE": "0",
        "LLAM_STRESS_DYNAMIC_PHASE": "1",
        "LLAM_EXPERIMENTAL_WORKER_RINGS": "1",
        "LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT": "1",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS": "128",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_ROUNDS": "256",
        "LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_US": "2000",
    })
PY
