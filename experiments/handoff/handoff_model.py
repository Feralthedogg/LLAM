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

"""Finite wake-handshake exploration and counterexample-guided burst synthesis.

This is an SC abstraction of sync.c's atomic protocol, not a C11 model checker.
The producer's completion publication is split from its CAS so the explorer
also visits publication windows. Object lifetime/primitive ownership are assumed;
native tests exercise the production functions separately.
"""

from collections import deque
from dataclasses import dataclass, replace


@dataclass(frozen=True)
class WakeState:
    armed: int = 0  # 0 unarmed, 1 armed, 2 inline-completed, 3 queued-completed
    queued: bool = False
    completed: bool = False
    park: int = -1
    observed: int = -1
    arm_pc: int = 0
    arm_committed: bool = False
    producer_pc: tuple[int, int] = (0, 0)
    wakes: int = 0


def wake_successors(state: WakeState, mutant: str = "none"):
    """Yield one atomic protocol transition; complete and cancel compete equally."""
    if state.arm_pc == 0:
        if state.armed == 0:
            yield "arm:CAS(0,1)", replace(state, armed=1, park=1, arm_pc=2, arm_committed=True)
        else:
            yield "arm:CAS-failed", replace(state, observed=state.armed, arm_pc=1)
    elif state.arm_pc == 1:
        park = int(state.observed == 3 or state.queued)
        yield "arm:observe-completion", replace(state, park=park, arm_pc=2)

    for actor, name in enumerate(("complete", "cancel")):
        pc = state.producer_pc[actor]
        pcs = list(state.producer_pc)
        changes = {}
        if pc == 0:
            if state.armed < 2:
                queued = state.armed == 1 or mutant == "queue_inline"
                changes["armed"] = 3 if queued else 2
                pcs[actor] = 1 if queued else 2
            elif mutant == "duplicate" and state.armed == 3:
                pcs[actor] = 1
            else:
                pcs[actor] = 4
        elif pc == 1:
            changes["queued"] = True
            pcs[actor] = 2
        elif pc == 2:
            changes["completed"] = True
            pcs[actor] = 3 if state.armed == 3 else 4
        elif pc == 3:
            changes["wakes"] = state.wakes + 1
            pcs[actor] = 4
        else:
            continue
        yield f"{name}:{pc}", replace(state, producer_pc=tuple(pcs), **changes)


def check_wake(mutant: str = "none") -> dict:
    if mutant not in ("none", "duplicate", "queue_inline"):
        raise ValueError("unknown mutation")
    initial = WakeState()
    frontier = deque([initial])
    parents = {initial: None}
    terminal = 0
    while frontier:
        state = frontier.popleft()
        done = state.arm_pc == 2 and state.producer_pc == (4, 4)
        invalid = state.wakes > 1 or (state.wakes > 0 and not state.arm_committed) or (done and (
            not state.completed or state.park != int(state.wakes == 1)))
        if invalid:
            trace = []
            cursor = state
            while parents[cursor] is not None:
                previous, event = parents[cursor]
                trace.append(event)
                cursor = previous
            return {"passed": False, "states": len(parents),
                    "counterexample": list(reversed(trace)), "terminal_states": terminal}
        if done:
            terminal += 1
        for event, following in wake_successors(state, mutant):
            if following not in parents:
                parents[following] = (state, event)
                frontier.append(following)
    return {"passed": True, "states": len(parents), "terminal_states": terminal,
            "counterexample": None}


def budget_counterexample(burst: int, service_bound: int) -> list[str] | None:
    """Find a path bypassing scheduler service more than the experiment permits.

    Zero means UNBOUNDED in LLAM, not disabled. Every direct transition assumes
    all existing hard guards passed. This is a service-opportunity bound, not a
    wall-clock deadline or a bound on arbitrary C task execution.
    """
    if burst < 0 or service_bound < 1:
        raise ValueError("nonnegative burst and positive service bound required")
    frontier = deque([(0, ())])
    seen = {0}
    while frontier:
        streak, trace = frontier.popleft()
        if streak > service_bound:
            return list(trace)
        if burst == 0 or streak < burst:
            following = streak + 1
            if following not in seen:
                seen.add(following)
                frontier.append((following, trace + ("direct_handoff",)))
        # A fallback/reset returns to the already visited zero state.
    return None


def synthesize(bursts=(0, 256, 64, 32, 16, 8, 4, 1), service_bound=16) -> dict:
    """Refine a finite policy template with universally quantified path checks.

    Throughput is only a ranking hypothesis here: prefer long bursts, ask the
    verifier, then eliminate every candidate admitting the returned trace.
    The runner ranks the surviving policies using separate training samples.
    """
    if len(set(bursts)) != len(bursts) or any(b < 0 for b in bursts):
        raise ValueError("distinct nonnegative bursts required")
    remaining = list(bursts)
    rounds = []
    while remaining:
        candidate = remaining[0]
        trace = budget_counterexample(candidate, service_bound)
        if trace is None:
            break
        rejected = [b for b in remaining if b == 0 or b >= len(trace)]
        rounds.append({"candidate": candidate, "counterexample": trace,
                       "reject": rejected, "constraint": f"0 < burst < {len(trace)}"})
        remaining = [b for b in remaining if b not in rejected]
    accepted = [b for b in remaining if budget_counterexample(b, service_bound) is None]
    return {"service_bound": service_bound, "rounds": rounds, "accepted_bursts": accepted,
            "baseline_burst": 64,
            "baseline_counterexample": budget_counterexample(64, service_bound)}
