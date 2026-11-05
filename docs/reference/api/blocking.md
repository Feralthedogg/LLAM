# Blocking and diagnostics

Call foreign blocking code without pinning a scheduler worker, and inspect a
running runtime with stats and dumps.

## Blocking integration

| API | Purpose |
| --- | --- |
| `llam_call_blocking_result` | Run blocking work and return through an explicit out parameter. |
| `llam_call_blocking` | Convenience blocking callback API. |
| `llam_call_native_affine_result` | Run a callback on a native-affine task's own OS thread. See [Managed C](../../guides/managed-c.md#calls-native-tls-and-blocking). |
| `llam_enter_blocking` | Mark the current task as entering an opaque blocking region. |
| `llam_leave_blocking` | Mark the current task as leaving an opaque blocking region. |

## Diagnostics and introspection

| API | Purpose |
| --- | --- |
| `llam_runtime_collect_stats_ex` | Size-aware stats snapshot for the process-default runtime. |
| `llam_runtime_collect_stats_ex_handle` | Size-aware stats snapshot for an explicit runtime handle. |
| `llam_runtime_collect_stats` | Convenience stats snapshot for the process-default runtime. |
| `llam_runtime_write_stats_json` / `llam_runtime_write_stats_json_handle` | Write JSON statistics for the process-default or an explicit runtime. |
| `llam_dump_runtime_state` / `llam_dump_runtime_state_handle` | Best-effort dump for the process-default or an explicit runtime. |
| `LLAM_RUNTIME_STATS_CURRENT_SIZE` | Size token for `llam_runtime_collect_stats_ex` and handle-scoped stats. |
| `llam_task_flags` | Return task spawn/runtime flags. |
| `llam_task_id` | Return the runtime-assigned task id. |
| `llam_task_state_name` | Return a stable task state string. |
| `llam_task_class` | Return the scheduler class for a task. |
| `llam_current_task` | Return the current managed task, or `NULL` outside LLAM. |
