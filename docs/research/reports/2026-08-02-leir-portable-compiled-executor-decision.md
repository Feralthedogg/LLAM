# LEIR Portable Compiled Executor Decision

**Date:** 2026-08-02
**Architecture decision:** `CONTINUE`
**Performance promotion:** `INCONCLUSIVE`
**3.0.0 release gate:** `BLOCKED`
**Release authorized:** no

## Decision

Continue with LEIR as the compiler/planner semantic contract and compiled
effect segments as the runtime execution boundary. Keep the portable compiled
B adapter as the general baseline and treat Linux/io_uring linked segments as
a separately measured specialization.

The experiment establishes the architectural part of the hypothesis:

- portable compiled execution preserves the interpreter oracle's outputs and
  peer-visible behavior;
- the completion consumer performs one normalization and one site lookup per
  activation with no interpreter dispatch or hot allocation;
- the Linux linked segment preserves the same contract while reducing two
  prepared SQEs to one visible terminal CQE per activation.

It does not yet establish a publishable general performance advantage. The
portable and Linux median ratios were favorable, but the predeclared spread
guard made the aggregate performance verdict `INCONCLUSIVE`. That guard stays
in place.

## Architectural boundary

The next LLAM layer should look like a runtime backend, not an Erlang-style VM:

1. compilers or planners emit a versioned LEIR module and typed continuation
   metadata;
2. LLAM binds values, owns cancellation and completion normalization, and
   invokes a compiled portable segment when no specialization is selected;
3. a platform backend may lower the same segment to io_uring, IOCP, kqueue, or
   another native mechanism;
4. actors, supervision, mailboxes, and language object models remain optional
   libraries above this boundary rather than becoming LLAM's core semantics.

This keeps the intended “LLVM for code generation, LLAM for concurrency
runtime lowering” position open to C callers and multiple language runtimes.

## Next experiment

Proceed in this order:

1. freeze longer per-cell windows before collection so every candidate exceeds
   a stable duration, then repeat the same classifier without relaxing its
   20% spread limit;
2. reproduce the portable axis on macOS and Windows using the same common row
   schema, with platform counters kept out of the portable verdict;
3. reproduce Linux `submit_all` and `coop_taskrun` on independent physical
   ARM64 and x86_64 machines;
4. add compiled `READ -> WRITE`, timers, file I/O, cancellation, peer-close,
   partial-write, and backpressure segments while preserving exact ownership
   receipts;
5. expose a small stable embedding ABI and standalone C fixture so an external
   compiler can create, bind, run, cancel, and destroy generated modules
   without private LLAM headers;
6. investigate worker-owned ring construction separately before reconsidering
   `defer_taskrun`.

The full evidence and provenance are recorded in
[LEIR Portable Compiled Executor Results](2026-08-02-leir-portable-compiled-executor-results.md).
No item above permits a 3.0.0 release; release work remains explicitly blocked
until the research features, cross-platform validation, and independent
reproduction gates are complete.
