<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSE.
-->

# Counterexample-guided wake/handoff experiment

This private experiment synthesizes the existing
`LLAM_YIELD_DIRECT_HANDOFF_BURST` setting. It does not modify production
defaults, public ABI, wait ownership, atomic ordering, or native asynchronous
preemption. It uses finite explicit-state exploration without extra dependencies.
It does **not** implement OpenAI's ETH/#SAT algorithm or establish a speedup from
that result.

## Run

```sh
make -j4 CC=clang test-handoff
python3 -B scripts/bench_handoff.py \
  --samples 7 --training-samples 3 --warmup 1 --rounds 65536 \
  --service-bound 16 --output research/handoff-comparison.json
```

Choose a new output filename for each run. The runner refuses to overwrite an
existing result. CMake builds the same native targets:

```sh
cmake --build build --target bench_handoff test_handoff_protocol
ctest --test-dir build --output-on-failure -R '^test_handoff_(protocol|model)$'
```

`verify_handoff.c` requires POSIX threads and returns skip code 77 elsewhere.
Native Windows must use CMake; the Make experiment target is for POSIX hosts.

## What the counterexamples mean

`handoff_model.py` explores two separate, deliberately small contracts:

1. **Wake handshake:** one waiter arms while completion and cancellation compete
   on the same wait node. The model follows `sync.c`'s 0→1, 0→2, and 1→3 atomic
   transitions and separately schedules publication of the queued/completed
   flags. It checks that only one producer queues a wake, a running unarmed task
   is not enqueued, and the terminal park decision agrees with the queued wake.
   Mutations that permit duplicate enqueue or enqueue before arm must produce
   counterexamples. This checks the checker as well as the modeled protocol.
2. **Scheduler service:** a direct-handoff chain must return to scheduler service
   after at most a configured number of direct transitions. The default
   experimental bound is 16, chosen for exploration; it is **not** an existing
   LLAM guarantee or evidence of a runtime defect. A shortest violating path
   refines the finite burst template, rejecting every candidate that admits
   that path. In LLAM, burst 0 is unbounded, not handoff disabled.

The default template is `[0, 256, 64, 32, 16, 8, 4, 1]`. The 17-transition
counterexample rejects `[0, 256, 64, 32]` for the experimental service bound.
The survivors undergo separate training measurements. The runner minimizes the
geometric mean of paired message-p99 ratios among candidates whose per-workload
median throughput loss stays within the configured training budget (10% by
default). If none meets that budget it explicitly reports this and compares the
best latency candidate without calling it acceptable. The chosen burst is frozen
before fresh confirmation measurements; confirmation cannot reselect it.

Safety guards are fixed throughout this search. The burst knob also affects
yield handoffs because the runtime shares that budget; this is not an isolated
wake-only actuator. The model's service bound is about opportunities to return
to the scheduler, not wall-clock deadlines, starvation freedom for arbitrary
tasks, or interruption of arbitrary C busy loops.

## Native validation and its limits

`verify_handoff.c` calls the actual `llam_wait_node_should_park()` and
`llam_wait_node_prepare_wake()` functions. It replays all six coarse arm /
complete / cancel call orders and executes 3,000 overlapping trials with three
persistent OS threads. Threads finish each epoch before the node is reused.
The synthetic node is retained for the entire test; this does not test object
reclamation, primitive removal ownership, or the entire cancellation path.

The model is a sequentially consistent abstraction of a single atomic protocol,
not a C11 weak-memory proof, LLVM model checker, or proof of all runtime behavior.
Its internal publication steps are not all controllable in native replay.
The native benchmark also records the largest actually observed handoff streak
to reproduce the service-bound witness. This is not exact replay of an OS
schedule. Run the existing select/cancel, multi-runtime, ownership-publication,
shutdown and invariant tests alongside it.

## Measurement contract

All policies use the same compiled runtime, RELEASE_FAST profile, one processor,
lock-free normal queue, default AUTO preemption, disabled autotune and enabled
handoff counters. Other inherited `LLAM_*` overrides are removed. The baseline
uses the actual profile default and verifies it is burst 64. Candidate runs
override only the existing burst setting. Compiler/build signatures and source
and executable hashes are retained in the result.

Three matched workloads run in fresh processes:

| Case | Work |
| --- | --- |
| `pair` | One request/ack channel pair |
| `contended` | Four pairs with an additional runnable yielding task |
| `timers` | Four pairs with an additional task sleeping for 50 microseconds |

Each exchange validates its sequence, pointer and acknowledgement. Throughput
counts completed request/ack exchanges from the release barrier to the last
sender's acknowledgement. Allocation, spawn, join and teardown are excluded.
Reported message latency is **send entry to receive return**, including channel
and scheduling overhead; it is not pure wake-to-dispatch latency. The existing
runtime wake-latency flag is deliberately unused because it disables direct
handoff and would change the measured path.

One observation per 16-message block is selected with a varying hashed phase to
avoid aliasing power-of-two burst boundaries. p99 is nearest-rank on these
individual event observations, not p99 of seven process times. Every observed
nanosecond value and multiplicity is retained in exact histograms without
bucket rounding. The bounded recorder fails on overflow instead of overwriting.
The observer measures yield return time or timer lateness. Fewer than 100 observer
samples makes its p99 especially coarse; report the sample count and treat it
as diagnostic. This is a closed-loop channel benchmark, not an open-loop load
generator or an end-to-end network benchmark.

Training order rotates. Confirmation alternates baseline/candidate order and
discards explicit warmup pairs. Results include per-process samples, paired
median ratios and observed ratio ranges. They are local measurements, not
release-quality cross-platform claims. A passed model and native test do not
substitute for performance evidence, and a throughput gain cannot waive safety.
