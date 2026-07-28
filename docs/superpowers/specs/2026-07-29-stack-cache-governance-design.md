# Stack Cache Governance Design

**Date:** 2026-07-29

**Status:** Approved as part of the runtime-backend productization program.

## Goal

Task stacks may be cached for throughput, but a runtime must bound retained
virtual/committed memory in bytes, return cold pages to the platform while
live, and never route a stack through another runtime's cache.

## Ownership correction

Stack allocation first resolves the task's owner runtime and may use the TLS
shard cache only when `g_llam_tls_shard->runtime` is that exact runtime.
Otherwise it uses the owner runtime's external/runtime cache path. A
cross-runtime spawn test must fail before this correction and prove that no
cache counter or mapping changes on the foreign runtime afterward.

Every cached mapping has one owner runtime, size class, state, and last-return
timestamp. Pop, push, trim, shutdown, and allocation-failure paths account the
mapping exactly once.

## Runtime byte policy

Add size-prefixed runtime options for:

- cache byte budget;
- high and low watermarks;
- idle age;
- secure scrub and discard-on-return flags.

Zero values resolve documented defaults. The resolver requires
`low <= high <= budget` and checked, page-aligned values. A disabled budget
causes direct release rather than caching.

An overflow-safe atomic reservation guarantees total cached bytes never exceed
the runtime budget. Per-class lists remain local for reuse, but byte authority
is runtime-wide. Push reserves bytes before publication; failure releases the
mapping. Pop removes list ownership and byte accounting as one logical
operation.

## Platform VM boundary

Stack operations use focused helpers:

- map and release;
- discard and reactivate;
- secure zero;
- optional resident-byte sampling.

Linux uses `madvise(MADV_DONTNEED)` for discard. Windows decommits and
recommits usable pages while preserving the reservation and guards. Darwin
scrubs before any discard whose zeroing guarantee is insufficient for the
selected security policy. Unsupported residency sampling is explicit rather
than reported as zero.

No platform call occurs while a shard cache lock is held. Live trim detaches a
bounded batch under the lock, updates authority, performs VM work outside the
lock, and then releases or returns only mappings whose state transition
succeeded.

## Trim policy

The runtime exposes:

- manual `trim_ex` with a target;
- memory-pressure notification;
- opportunistic idle trim.

Crossing the high watermark trims toward low. Memory pressure may trim to
zero. Idle trim selects only entries older than the configured threshold.
Concurrent trim requests serialize at runtime authority but do not globally
block task execution.

Secure-return mode scrubs or discards before a mapping becomes reusable.
Failure to establish the required security state releases the mapping instead
of publishing it.

## Diagnostics

Append exact counters for cached bytes/mappings, committed bytes, trim
requests, discarded/released bytes, budget rejections, and secure-return
failures. Resident bytes include a validity/capability field and sampling
timestamp; they are never presented as exact authority.

## Verification

RED/GREEN tests cover:

- cross-runtime allocation while another runtime's shard is in TLS;
- exact budget enforcement across mixed classes and concurrent shards;
- high/low hysteresis, manual target, memory pressure, and idle age;
- detach-under-lock/VM-outside-lock behavior;
- secure reuse and injected discard/reactivation/scrub failure;
- Windows reserve/decommit/recommit transitions;
- concurrent pop/push/trim/stats and shutdown;
- option-prefix, JSON, and text-diagnostic compatibility.

Fresh ASan, TSan, platform, and long-burst RSS tests are required before this
stage is complete.
