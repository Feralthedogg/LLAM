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

For compiler-qualified C, use the [managed runtime API](../../guides/managed-c.md):
`llam_preempt_opts_init`, `llam_runtime_create_managed`,
`llam_runtime_preempt_info_ex`, `llam_runtime_module_register_ex`,
`llam_runtime_module_unregister`, and `llam_call_native_affine_result`.
`<llam/managed.h>` defines caller-sized policy, resource and module structures.
The ordinary and advanced constructors default to `ASYNC_REQUIRED`, refusing
unqualified task entries and unsupported native profiles. `ASYNC_AUTO` and
`COOPERATIVE` are explicit advanced choices. The `*_compat` constructors
and the expert [Windows interface](platform.md#experimental-windows-preemption) retain their compatibility contracts.

## Explicit ownership and I/O budgets

Host-side constructors are `llam_runtime_channel_create`,
`llam_runtime_mutex_create`, `llam_runtime_cond_create`,
`llam_runtime_cancel_token_create`, `llam_runtime_task_group_create[_ex]`,
`llam_runtime_timer_create_ex` and `llam_runtime_signal_set_create_ex`. They
retain the runtime through publication and preserve existing object lifetimes.
Old constructors retain current-task/default-runtime selection.

`llam_wait_opts_init` initializes the caller-sized common wait policy.
`llam_read_ex`, `llam_write_ex`, `llam_connect_ex`, `llam_poll_fd_ex`,
`llam_read_handle_ex` and `llam_write_handle_ex` accept it. Stream fd operations
also have absolute `llam_read_until`, `llam_write_until`, `llam_connect_until`
and `llam_poll_fd_until` forms. New poll forms take an `int` mask in the short-mask range; legacy poll ABI
is unchanged. Poll timeout returns 0; byte/connect timeout returns -1/`ETIMEDOUT`.

`llam_read_exact`, `llam_read_exact_ex`, `llam_write_all` and
`llam_write_all_ex` report confirmed progress through
a required `size_t *`, including on failure. Early EOF is `EPIPE`; premature
zero write is `EIO`. `llam_task_local_set_owned` adds nonblocking same-M exit
cleanup and `llam_task_local_take_result` transfers the value without cleanup.
See [Embed LLAM](../../guides/embedding.md) for cancellation composition,
endpoint restrictions, drain timeout recovery and complete failure cleanup.
