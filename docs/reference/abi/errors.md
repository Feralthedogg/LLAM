# Error contract

What each error value means across the public API.

## Return conventions

Unless explicitly documented otherwise:

- `0` means success for integer status APIs.
- Positive byte counts or descriptor values follow the corresponding syscall.
- `-1` means failure and `errno` contains the reason.
- `NULL` means allocation, cancellation, or submission failure and `errno`
  disambiguates where possible.
- Managed tasks have task-local `errno` at LLAM fiber context-switch
  boundaries. When a task yields, parks, sleeps, joins, waits on I/O, or exits,
  LLAM snapshots its current `errno`; when that task resumes, LLAM restores the
  snapshot before user code continues. This prevents another task or scheduler
  worker thread from leaking its TLS `errno` into the resumed task.
- External threads that are not running inside a LLAM task continue to use the
  platform thread-local `errno` directly.
- Callers should still inspect `errno` only after APIs that signal failure,
  except for APIs that explicitly forward callback/syscall errno state.
## Common errors

Common public failures are fixed as follows:

| errno | Meaning |
| --- | --- |
| `EINVAL` | Invalid argument, zero-sized inbound option struct, invalid enum/policy value, or invalid consumed handle use. A constructor that requires asynchronous preemption also reports it for `LLAM_PREEMPT_OFF` or `LLAM_PREEMPT_COOPERATIVE`, and spawn on that runtime for `LLAM_SPAWN_F_NO_PREEMPT`. |
| `EAGAIN` | Nonblocking operation would need to park or wait. |
| `EBUSY` | Runtime already initialized/running, an ordinary or managed constructor called from a managed task, concurrent second task join before handle consumption, detach while a join waiter owns completion, or destroy while observers remain. |
| `ETIMEDOUT` | Absolute deadline or relative timeout expired. |
| `ECANCELED` | Cancellation token or cooperative runtime stop was observed by a cancellable operation. |
| `EPIPE` | Channel is closed or an operation reached an equivalent peer-closed state. |
| `EXDEV` | Runtime-aware object was used from a different runtime owner. Explicit runtimes are isolated owner domains; cross-runtime managed use fails deterministically. A host thread also gets `EXDEV` from an object whose explicit owner runtime was destroyed; see [After the owner runtime is gone](sync.md#after-the-owner-runtime-is-gone). |
| `ENOMEM` | Allocation failure. |
| `ENOTSUP` | Platform/backend feature is unsupported, a constructor that requires asynchronous preemption found no qualified native profile, an unqualified task entry was spawned on that runtime, or a runtime-aware operation requiring a managed task was called outside one. |

## Owner checks

Default source and release builds keep runtime-owner checks enabled so
`EXDEV` remains observable. Projects that build LLAM from source can define
`LLAM_RUNTIME_DISABLE_OWNER_CHECKS=1` only for explicit singleton-only profiling
builds; those binaries intentionally give up cross-owner diagnostics and should
not be used as ABI-conformance references.

LLAM preserves syscall-style error values for I/O operations so language
runtimes can map them into their own error domains without backend-specific
special cases.
