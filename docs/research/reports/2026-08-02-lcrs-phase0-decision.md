# LCRS Phase 0 Decision

Decision: **NARROW — retain the coded palette model and permit shadow-only prototyping; do not activate it as a scheduler policy yet.**

## Why

The isolated algorithm passed every correctness property and produced strong model-level separation:

- 74.52% aggregate probe reduction versus global deepest;
- steady-state p99 fan-in reductions of 75% to 81.25%;
- hotspot p99 reductions of 37.5% to 50%;
- coded-only p99 fan-in better than deterministic random-k in all six modeled workloads;
- exact full scan remains available and unchanged as the conservative fallback model.

Those results are sufficient to retain the idea and implement an observe-only shadow measurement path. They are not sufficient for active scheduling because the model does not execute the real queue, migration, cache, or CAS path, and Linux native evidence could not be collected.

## Authorized next step

Phase 1 may add an internal, disabled-by-default shadow path that:

- constructs and validates immutable palettes at runtime initialization;
- samples coded and current full-scan victims;
- always steals from the current full-scan victim;
- records probes, agreement, depth ratio, invalid candidates, and predicted fan-in;
- never counts observation as watchdog progress;
- immediately disables itself on allocation or validation failure.

## Still blocked

The following remain unauthorized:

- coded victim selection controlling a real steal;
- removal or semantic modification of full scan;
- stable ABI exposure;
- release or version changes;
- any 3.0.0 publication.

Before active mode, collect Linux x86_64 evidence at 32, 64, and preferably 128 shards; compare against a production-quality O(k) deterministic random sampler; run real pinned-task, merge/offline, dynamic-worker, shutdown, sanitizer, fuzz, and soak cases; and verify throughput and migration gates independently from platform-specific fan-in results.

Work on LSWG, LRPA, and LCCF-CFS can continue independently while Linux LCRS evidence is pending.
