# License and Comment Hygiene Design

**Date:** 2026-07-30

**Status:** Approved for implementation

## Objective

Make LLAM's licensing machine-auditable across every tracked code-bearing
file, and make its most dangerous ownership state machines understandable
without adding comments that merely restate C syntax.

This work changes documentation and repository checks only. It must not change
runtime behavior, public ABI, build output, performance policy, version, tag,
or release state.

## Current State

The repository root contains the complete Apache License 2.0 text, the README
identifies the project as Apache-2.0, and nearly all stable runtime files carry
either the full Apache boilerplate or an SPDX identifier. Public API
documentation is strong: every current `LLAM_API` declaration has an adjacent
Doxygen contract.

The remaining problems are consistency and maintenance:

- the LEIR research files do not carry per-file license identifiers;
- a smaller set of tests, scripts, and recently split headers also lack
  identifiers;
- stable C files generally use the full boilerplate while scripts and newer
  research files use concise SPDX headers;
- no automated check prevents new unmarked code-bearing files;
- some of the largest platform ownership files describe local branches but do
  not summarize their complete state and retirement contracts near the code.

The absence of a per-file marker does not override the repository-level
license. It does, however, weaken source extraction, SPDX scanning, SDK
packaging, and third-party compliance review.

## Considered Approaches

### Add only the missing license headers

This closes the immediate scanner gap with little churn, but it does not
prevent recurrence and leaves the most difficult state machines dependent on
separate design documents.

### Add comments to every function

This creates a superficially high documentation count but duplicates names,
types, and control flow that are already visible in C. Such comments drift
quickly and hide the few ownership rules that maintainers actually need.

### Audit all license markers and document only semantic boundaries

This is selected. Every tracked code-bearing file receives a machine-readable
license marker or an accepted existing full notice. A repository check
prevents regression. Comment work is restricted to public contracts, file
responsibilities, state transitions, ownership transfer, cancellation,
generation validation, kernel retirement, and reuse boundaries.

## License Policy

### Canonical identifier

New or previously unmarked files use:

```text
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Feralthedogg
```

The lines use the native comment syntax for C, assembly, shell, Python,
PowerShell, CMake, Make, Go, Rust, and workflow files. Existing full Apache
boilerplate remains valid and is not mechanically replaced.

Generated data formats that do not support comments, including strict JSON,
are excluded. Vendored third-party files, if introduced later, must retain
their upstream notices and must not be relabeled as Apache-2.0.

### Coverage

The audit covers tracked, code-bearing files under:

- `include/`, `src/`, `tests/`, `experiments/`, `examples/`, `scripts/`;
- `cmake/`, `docker/`, and `.github/workflows/`;
- top-level build entry points such as `CMakeLists.txt` and `Makefile`.

The accepted forms are an SPDX identifier within the leading comment block or
the existing full Apache license notice. Shebangs remain the first line; their
SPDX marker follows immediately.

The audit reports every missing path and exits non-zero. It has focused tests
for accepted SPDX syntax, accepted full notices, shebang placement, unsupported
extensions, JSON exclusion, and multiple missing files.

### Third-party notices

LLAM does not currently vendor liburing. The Linux build links the system
library. A `THIRD_PARTY_NOTICES` file is therefore not introduced solely for
this change. Packaging work that later bundles or statically incorporates
third-party code must preserve its notices and add a distribution-level notice
inventory.

## Comment Policy

### Comments that are required

Comments must describe facts that cannot be recovered safely from a local
expression:

- who owns an object before and after a function;
- the synchronization or lock required for a transition;
- which generation makes a completion current or stale;
- the distinction between semantic completion and resource retirement;
- when kernel-visible tokens, descriptors, buffers, tasks, or tickets may be
  reused;
- why an apparently redundant ordering, fence, branch, or fallback is
  required;
- public blocking, cancellation, timeout, lifetime, and error behavior.

### Comments that are rejected

The change must not add comments that merely translate a function name,
assignment, loop, or condition into English. It must not describe speculative
future work, duplicate entire design documents, or claim platform guarantees
that are not enforced by code and tests.

### Targeted state-machine review

The review prioritizes code where mistakes can create use-after-free,
double-completion, stale-generation reuse, lost wakeups, or kernel ownership
violations:

- generic wait publication, cancellation, completion, and reinjection;
- Linux native-segment queue, cancellation, CQE reduction, target retirement,
  and reuse;
- Windows IOCP association generation, submission, and completion retirement;
- Darwin/kqueue watch submission, buffered readiness, and completion transfer;
- shard rehome and direct handoff;
- LEIR program, plan, binding, fixed-resource attachment, batch execution, and
  destruction.

Each selected file receives a concise file-level contract or state map. Only
cross-boundary functions receive additional Doxygen ownership contracts.

## Verification

Verification is layered:

1. run the focused license-audit unit tests;
2. run the audit against the complete tracked tree and require zero missing
   code-bearing files;
3. verify every `LLAM_API` declaration still has an adjacent Doxygen block;
4. compile default research-off and research-on builds to catch malformed
   comment placement in C, assembly, scripts, and build files;
5. run formatting and repository structure checks already required by the
   branch;
6. inspect the diff to confirm there are no executable-token or ABI changes.

The completion report must distinguish automated license coverage from the
necessarily qualitative state-machine comment review.

## Non-goals

- changing LLAM's Apache-2.0 license;
- adding a contributor license agreement;
- rewriting existing full license headers to SPDX;
- documenting every private helper;
- altering runtime algorithms or public ABI;
- promoting LEIR, changing its performance verdict, or releasing a new
  version.
