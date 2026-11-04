# Tasks

Spawn, join, and detach tasks, and manage their groups and task-local values. A
task handle is consumed by one successful join or one detach.

## Tasks

| API | Purpose |
| --- | --- |
| `llam_spawn_opts_init` | Fill spawn options with ABI-safe defaults. |
| `llam_runtime_spawn_ex` | Spawn into an explicit runtime handle. |
| `llam_spawn_ex` | Spawn into the selected current/default runtime with explicit option size. |
| `llam_spawn` | Convenience spawn wrapper. |
| `llam_join` | Wait indefinitely and consume a task handle on success. |
| `llam_join_until` | Wait until an absolute deadline. A timeout leaves the handle joinable. |
| `llam_detach` | Consume a task handle without joining. |
| `llam_yield` | Cooperatively yield the current managed task. |
| `llam_task_safepoint` | Mark progress in CPU-bound loops. |
| `LLAM_PREEMPT_POLL` | Public hot-loop safepoint macro. |
| `LLAM_PREEMPT_POLL_EVERY` | Safepoint macro for counted loops. Arguments are evaluated once. |
| `llam_task_set_class` | Change the current task scheduler class. |

`llam_task_set_result` publishes an application value/error at completion.
`llam_join_result` and `llam_join_result_until` copy it before consuming a normal
handle; `llam_task_get_result` peeks after completion, including a borrowed group
child. An unsuccessful join leaves output untouched.

## Task groups

| API | Purpose |
| --- | --- |
| `llam_task_group_create` | Legacy group: explicit child tokens override group cancellation. |
| `llam_task_group_create_ex` / `llam_runtime_task_group_create_ex` | Opt in to `LLAM_TASK_GROUP_F_LINK_CANCEL`: group OR explicit token. |
| `llam_task_group_spawn_ex` | Spawn a grouped child with explicit option size. |
| `llam_task_group_spawn` | Convenience grouped spawn wrapper. |
| `llam_task_group_cancel` | Cancel the group source; external tokens themselves are never cancelled. |
| `llam_task_group_join` | Join all grouped children. |
| `llam_task_group_join_until` | Join grouped children until an absolute deadline. |
| `llam_task_group_destroy` | Destroy a group after child work is drained or cancelled. |

## Task-local storage

| API | Purpose |
| --- | --- |
| `llam_task_local_key_create` | Allocate a task-local storage key. |
| `llam_task_local_key_delete` | Delete a task-local storage key. |
| `llam_task_local_get` | Read the current task's value for a key. |
| `llam_task_local_set` | Set or clear the current task's value for a key. |
| `LLAM_TASK_LOCAL_INVALID_KEY` | Invalid key sentinel. |
