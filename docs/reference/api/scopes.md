# Owned scopes and typed outcomes

!!! note "Development API after 3.0.0"
    These APIs require ABI **2.5** headers and runtime from the development tree.
    The published 3.0.0 archives provide ABI 2.4 and do not contain these symbols.
    Dynamic bindings must check the ABI minor and resolve the new symbols first.

Include `<llam/scope.h>`. A scope owns execution, cancellation and cleanup; a
future owns interest in one outcome. Destroying a drained scope does not destroy
its ready futures or values. Dropping a future never detaches work from its scope.
The existing task/group/result APIs retain their contracts.

## Initialize and describe values

| API | Purpose |
| --- | --- |
| `llam_result_contract_init` | Initialize success and application-error descriptors to unit. |
| `llam_value_spec_init` | Initialize an owned-value specification to unit with default `free` cleanup. |
| `llam_scope_opts_init` | Initialize reserved scope options to zero. |
| `llam_typed_task_spec_init` | Initialize a task specification with unit results and no argument. |
| `llam_outcome_init` | Initialize an empty outcome record. |
| `llam_value_create_ex` | Adopt initialized, aligned data and its cleanup context on success only. |
| `llam_value_copy_ex` | Copy a trivially byte-copyable value into separately allocated aligned storage. |
| `llam_value_data_ex` | Validate the expected semantic type and borrow the owned storage. |
| `llam_value_release` | Consume the value handle and run its cleanup exactly once. |

`llam_type_descriptor_t` has a fixed 32-byte layout: `id_high`, `id_low`, and
`size` are 64-bit fields; `alignment` and `reserved` are 32-bit fields. IDs are
stable semantic identities, never RTTI or process addresses. Both IDs zero mean
unit, which requires size zero and alignment one. Alignment must be a nonzero
power of two; reserved fields must be zero. Matching size alone is insufficient.

`LLAM_TYPE_UNIT_INIT`, `LLAM_TYPE_U64_INIT`, `LLAM_TYPE_I64_INIT`,
`LLAM_TYPE_U32_INIT`, and `LLAM_TYPE_I32_INIT` provide interoperable native scalar
descriptors. Application IDs must avoid the reserved `LLAM_TYPE_SCALAR_ID_HIGH`
namespace. These are native representations, not portable serialized formats.
The 64-byte `llam_result_contract_t` contains success and error descriptors.

`llam_value_spec_t` contains the descriptor, data, a drop callback and its context.
A null callback selects `free(data)`. The callback runs outside runtime locks,
under a native guard. It must be nonblocking, thread-neutral, non-throwing and must
not reenter LLAM. Keep its provider module loaded and context valid until **every**
owned value using it is released, including values retained after runtime teardown.
A byte copy does not clone referenced resources or invoke C++ constructors.

A value handle is move-only and runtime-neutral. A successful adoption transfers
ownership; failure leaves the original allocation and output untouched. A data
borrow is valid only while the caller owns the value: do not consume it concurrently.
Owned arguments may contain pointers only when their pointees independently remain
valid through full task retirement. This API does not make borrowed stack storage
safe after an early return.

## Spawn and collect

| API | Purpose |
| --- | --- |
| `llam_runtime_scope_create_ex` | Create an open scope on an explicit runtime; null options select defaults. |
| `llam_scope_spawn_ex` | Admit owned work, consuming its argument only on success and returning an independent future. |
| `llam_task_set_outcome_ex` | Set the current scoped task's candidate outcome once; consume a matching value only on success. |
| `llam_future_wait_until` | Wait without consuming; normal caller cancellation and request deadlines apply. |
| `llam_future_take_ex` | Nonblocking typed take; consume a ready future only after full type/output validation. |
| `llam_future_release` | Consume result interest, preserving scope ownership of the execution. |

A `llam_typed_task_spec_t` supplies an entry, an owned argument handle (or null),
optional borrowed spawn options plus their size, and both result descriptors.
The default runtime still requires a compiler-qualified entry. An explicit child
token is ORed with the private scope token; scope cancellation never cancels that
external token. Spawn racing with close either reserves ownership before close,
or returns `EPIPE` with the argument and output unchanged. Admission/allocation
failure also preserves the original argument handle.

The task receives the argument's data pointer and must not free it. It may call
`llam_task_set_outcome_ex` once. A second call returns `EBUSY`; invalid type/kind
leaves the candidate value owned by the caller. The setter permits `OK`,
`APP_ERROR`, or `CANCELLED`; only the runtime produces `RUNTIME_ERROR`.
A normally returning unit task implicitly succeeds. Missing non-unit output
becomes `RUNTIME_ERROR` with `EINVAL`. A task returning without an explicit outcome
while cancellation is observed becomes `CANCELLED`. An explicitly set outcome
is retained on normal return. An interrupted execution that never returns cannot
publish its candidate as a successful result.

| Terminal kind | Payload | Meaning |
| --- | --- | --- |
| `LLAM_OUTCOME_OK` | Success type, or null for unit | Task produced its success result. |
| `LLAM_OUTCOME_APP_ERROR` | Error type, or null for unit | Application-level failure; not an `errno` failure of take. |
| `LLAM_OUTCOME_CANCELLED` | Null | Task observed cancellation before a result. |
| `LLAM_OUTCOME_RUNTIME_ERROR` | Null; `runtime_error` populated | Execution could not produce a valid application outcome. |

A successful take returns zero for **all four** terminal kinds. `EAGAIN` means
not ready; `EINVAL` includes type/size mismatch; `EBUSY` includes conflicting
public consumption. These failures preserve future and output. Stale handles
return `EINVAL`. A full V1 `llam_outcome_t` output is mandatory because a partial
copy could lose an owned pointer. The expected contract validates both types.
Success transfers the payload and consumes the future. Release the payload with
`llam_value_release` after use. `task_id` is diagnostic, not a joinable handle.

Readiness follows entry return, task-local/owned-argument cleanup, backend/helper
retirement, native execution retirement, and task admission release. A ready
future can be taken by a host after both scope and runtime are destroyed. Managed
access from a peer runtime fails with `EXDEV`. Scoped children cannot be joined
or detached using ordinary task APIs (`EBUSY`).

## Cancel and drain

| API | Purpose |
| --- | --- |
| `llam_scope_cancel` | Seal admission and request private scope cancellation; idempotent. |
| `llam_scope_get_report_ex` | Copy the compatible report prefix without waiting or sealing. |
| `llam_scope_drain_until` | Seal admission and wait for complete quiescence using a cleanup deadline. |
| `llam_scope_cancel_and_drain_until` | Seal, cancel and drain under one public operation. |
| `llam_scope_destroy` | Consume a drained scope with no concurrent public operation. |

The state moves from `OPEN` to `CLOSING` on drain or cancel, then to `DRAINED`
when no pending tasks or active spawns remain.

A drain shields only the calling task's application cancellation and inherited
request budget for the duration of the cleanup wait. Both remain effective after
it returns. The explicit `cleanup_deadline_ns` stays effective; runtime failure
or shutdown can still interrupt an incomplete drain. Managed callers park,
allowing peer tasks to progress even with one worker.

Deadlines use absolute `llam_now_ns()` units: zero never waits; `UINT64_MAX` is
unbounded. Already quiescent scopes succeed immediately. `ETIMEDOUT` leaves all
outstanding ownership intact and the scope sealed; retry drain later. A timeout
never permits freeing memory still used by I/O, a native frame, or a blocking
callback. Cancellation requests do not forcibly unwind arbitrary foreign code.

Host drain never drives the scheduler. Pending work without an active runner
returns `EBUSY` and sets `LLAM_SCOPE_REPORT_F_NEEDS_RUNNER` in that call's report
(unless its deadline was already expired). Only one drainer is admitted. A child
cannot drain its own scope (`EDEADLK`, without sealing or cancellation side effects).

The 72-byte report includes state, flags, admitted/pending task counts, active
spawns, four terminal-kind counts and the first runtime error. `QUIESCENT` means
all admitted execution has retired, including cleanup; it does not release ready
future results retained by callers. `CANCEL_REQUESTED` records scope cancellation.
Reports can be requested on wait failure; validation failures do not transfer any
ownership. Destroy before quiescence or during another operation returns `EBUSY`.

## ABI sizes and typed host binding

Use `LLAM_TYPE_DESCRIPTOR_CURRENT_SIZE`, `LLAM_RESULT_CONTRACT_CURRENT_SIZE`,
`LLAM_VALUE_SPEC_CURRENT_SIZE`, `LLAM_SCOPE_OPTS_CURRENT_SIZE`,
`LLAM_TYPED_TASK_SPEC_CURRENT_SIZE`, `LLAM_OUTCOME_CURRENT_SIZE`, and
`LLAM_SCOPE_REPORT_CURRENT_SIZE` for the corresponding size argument. Initializers
and ownership-bearing inputs/outputs require their complete V1 prefix, preserve
unknown output tails, and ignore unknown input tails. Reports copy the available
prefix. Future versions may append tails without changing V1 ownership fields.

`<llam/future.hpp>` provides optional C++17 host types `llam::owned_value<T>`,
`llam::future<T,E>`, and `llam::outcome<T,E>` (default error type `llam::unit`).
They are move-only. Specialize `llam::type_id<T>` with stable `high`/`low` members
for custom types. `owned_value<T>::adopt` consumes a `std::unique_ptr<T>` only on
success and calls `delete` once; T's destructor must satisfy the callback rules.
`future::spawn` moves an owned argument only on successful admission; `take`
requires an empty outcome destination. Methods return C-style status and `errno`.

Use `reset()` for reportable release failures. Destruction or move-assignment
that must release an invalid/concurrently accessed/wrong-owner handle terminates
instead of silently leaking. Keep wrapper operations in an allowed owner or host
context and externally synchronize access. The wrapper does not grant C++ task
code managed qualification or translate C++ exceptions into outcomes.

The complete [C ownership example](https://github.com/Feralthedogg/LLAM/blob/main/examples/scoped_results.c)
checks creation, run, take, cancellation and teardown failures using an explicit
compatibility runtime. The installed SDK regression compiles
`tests/fixtures/managed/scoped_tasks.c` through `llam-clang`, links
`scoped_consumer.cpp` as a native C++ host, and checks the default asynchronous
runtime plus typed take after scope/runtime teardown.
