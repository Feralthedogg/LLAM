# Lifecycle

Create, run, stop, and destroy runtimes.

## Lifecycle

Embedding code should use the handle-scoped lifecycle:
`llam_runtime_create()`, `llam_runtime_spawn_ex()`,
`llam_runtime_run_handle()`, and `llam_runtime_destroy()`. The process-default
APIs select the same required asynchronous policy for single-runtime programs.
Use the explicitly named `*_compat` initializers for boundary/safepoint execution.

| API | Purpose |
| --- | --- |
| `llam_runtime_opts_init` | Fill runtime options with ABI-safe defaults. |
| `llam_runtime_init_ex` | Initialize the process-default asynchronous runtime with an explicit option size. |
| `llam_runtime_init` | Convenience default-runtime initializer. |
| `llam_runtime_init_compat_ex` | Initialize the process-default compatibility runtime with an explicit option size. |
| `llam_runtime_init_compat` | Convenience default-runtime compatibility initializer. |
| `llam_runtime_default` | Return the default runtime handle. |
| `llam_runtime_create` | Explicit runtime with required asynchronous admission. |
| `llam_runtime_create_compat` | Explicit native-C compatibility runtime. |
| `llam_runtime_run_handle` | Canonical embedding scheduler driver for one runtime handle. |
| `llam_runtime_request_stop_handle` | Stop one explicit runtime; peer managed tasks get `EXDEV`. |
| `llam_runtime_get_resources_ex` | Caller-sized snapshot of pool sizes, live/retained work and closing state. |
| `llam_runtime_drain_until` | Host bounded stop/drain; timeout preserves runtime and resources. |
| `llam_run` | Drive the process-default runtime. |
| `llam_runtime_request_stop` | Request cooperative stop and wake workers. |
| `llam_runtime_destroy` | Canonical embedding teardown. Passing `NULL` aliases default shutdown. |
| `llam_runtime_shutdown` | Stop and release default-runtime resources. |

## Managed runtimes

For compiler-qualified C, use the [managed runtime API](../../guides/managed-c.md).
`<llam/managed.h>` defines caller-sized policy, resource and module structures; see
[their fields](../options/runtime.md#managed-runtime-options).
The ordinary and advanced constructors default to `ASYNC_REQUIRED`, refusing
unqualified task entries and unsupported native profiles. `ASYNC_AUTO` and
`COOPERATIVE` are explicit advanced choices. The `*_compat` constructors
and the expert [Windows interface](platform.md#experimental-windows-preemption) retain their compatibility contracts.

| API | Purpose |
| --- | --- |
| `llam_preempt_opts_init` | Fill managed options with their defaults. |
| `llam_runtime_create_managed` | Explicit runtime with a chosen admission policy and capsule and native-affine limits. |
| `llam_runtime_preempt_info_ex` | Requested and effective policy, the reason, and capsule storage accounting. |
| `llam_runtime_module_register_ex` | Register one loaded image's managed metadata. |
| `llam_runtime_module_unregister` | Unregister a module; success consumes its handle. |

## Explicit ownership

These host-side constructors stamp an object with the runtime you name. They retain
the runtime through publication and preserve existing object lifetimes. Older
constructors keep their current-task/default-runtime selection. The group forms that
take options are under [Task groups](tasks.md#task-groups).

| API | Purpose |
| --- | --- |
| `llam_runtime_channel_create` | Create a channel owned by a runtime. |
| `llam_runtime_mutex_create` | Create a mutex owned by a runtime. |
| `llam_runtime_cond_create` | Create a condition variable owned by a runtime. |
| `llam_runtime_cancel_token_create` | Create a cancellation token owned by a runtime. |
| `llam_runtime_task_group_create` | Create a task group owned by a runtime. |
| `llam_runtime_timer_create_ex` | Create a timer owned by a runtime. |
| `llam_runtime_signal_set_create_ex` | Create a signal wait set owned by a runtime. Linux-only; other platforms return `ENOTSUP`. |

See [Embed LLAM](../../guides/embedding.md) for cancellation composition,
endpoint restrictions, drain timeout recovery and complete failure cleanup.

## Reserved compiler ABI

These symbols are exported for code the managed compiler generates. They are not
application API and are not task admission.

| API | Purpose |
| --- | --- |
| `llam_managed_native_enter` / `llam_managed_native_leave` | Native entry and exit guards the compiler emits in balanced pairs. Do not call them directly. |
