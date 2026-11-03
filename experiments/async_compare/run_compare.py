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
# See LICENSES/Apache-2.0.txt.

"""Run the existing comparator with prebuilt binaries and retain actual child output."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import bench_runtime_compare as compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llam", type=Path, required=True)
    parser.add_argument("--go", type=Path, required=True)
    parser.add_argument("--tokio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    raw = output / "raw"
    raw.mkdir()
    binaries = {"llam": args.llam.resolve(strict=True), "go": args.go.resolve(strict=True),
                "tokio": args.tokio.resolve(strict=True)}
    cases = [case for case in compare.DEFAULT_CASES if case not in ("opaque_block", "select_timeout")]
    # The qualification profile supports one primary, full SIMD preservation,
    # no dynamic workers and no lock-free normal queue. Match scheduler width.
    for key in list(os.environ):
        if key.startswith("LLAM_"):
            del os.environ[key]
    os.environ.update(LLAM_BENCH_ASYNC="1", LLAM_RUNTIME_PROFILE="release-fast",
                      LLAM_EXPERIMENTAL_DYNAMIC_WORKERS="0", LLAM_EXPERIMENTAL_LOCKFREE_NORMQ="0",
                      LLAM_PREEMPT_MODE="auto", LLAM_PREEMPT_QUANTUM_NS="1000000",
                      GO_BENCH_GOMAXPROCS="1", GOMAXPROCS="1", GODEBUG="asyncpreemptoff=0",
                      TOKIO_BENCH_WORKER_THREADS="1")
    metadata = {"time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "cases": cases, "rounds": 127, "warmup_rounds": 8, "samples": 5,
                "environment": {key: value for key, value in os.environ.items()
                                if key.startswith("LLAM_") or key in
                                ("GO_BENCH_GOMAXPROCS", "GOMAXPROCS", "GODEBUG", "TOKIO_BENCH_WORKER_THREADS")},
                "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                             for name, path in binaries.items()},
                "unsupported": {
                    "opaque_block": "native async profile rejects opaque enter; no LLAM score",
                    "select_timeout": "different API semantics; Tokio zero-duration timeout exceeded 180s at these sizes"},
                "coverage": "certified assembly leaf qualification before timing; ordinary C benchmark bodies outside certified ranges"}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    real_capture = compare.run_capture
    index = 0

    def record_capture(command, **kwargs):
        nonlocal index
        process = real_capture(command, **kwargs)
        index += 1
        record = {"command": command, "case": kwargs.get("env", {}).get("LLAM_BENCH_ONLY"),
                  "returncode": process.returncode, "stdout": process.stdout, "stderr": process.stderr}
        # Read-through recording only: execute the actual process and return its
        # result unchanged to the existing parser and median-selection policy.
        (raw / f"process-{index:03d}.json").write_text(json.dumps(record, indent=2) + "\n")
        return process

    old_argv = sys.argv
    compare.run_capture = record_capture
    sys.argv = ["bench_runtime_compare.py", "--runtime", "all", "--no-build", "--isolate-cases",
                "--llam-binary", str(binaries["llam"]), "--go-binary", str(binaries["go"]),
                "--tokio-binary", str(binaries["tokio"]), "--rounds", "127", "--warmup", "8",
                "--samples", "5", "--sample-policy", "median", "--out-dir", str(output),
                "--cases", *cases]
    try:
        result = compare.main()
    finally:
        compare.run_capture = real_capture
        sys.argv = old_argv
    if result == 0 and index != len(cases) * 3 * 5:
        raise RuntimeError("missing process records")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
