# Tasks and timers

Start with this shape when one task needs to create work and wait for it.

## Spawn and join

**Complete program.** One task creates a child, joins it, and prints its result.

```c title="main.c"
#include <llam/runtime.h>

#include <errno.h>
#include <stdio.h>

typedef struct job {
    int input;
    int output;
} job_t;

static void child(void *arg) {
    job_t *job = arg;

    job->output = job->input * job->input;
}

static void root(void *arg) {
    job_t *job = arg;
    int error = 0;

    llam_task_t *task = llam_spawn(child, job, NULL);
    if (task == NULL || llam_join(task) != 0) {
        error = errno;
    } else {
        printf("result=%d\n", job->output);
    }
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    /* Declared here because a failed join can leave the child running; the
     * run call returns only after every task has finished. */
    job_t job = {.input = 12};
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create_compat(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, root, &job, NULL, 0);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return 1;
    }
    llam_task_result_t result;
    int failed = llam_join_result(task, &result, sizeof(result)) != 0;
    if (!failed) failed = result.flags != LLAM_TASK_RESULT_F_SET || result.error != 0;
    llam_runtime_destroy(runtime);
    return failed;
}
```

```text title="Output"
result=144
```

## Sleep until a deadline

**Focused snippet.** Use absolute deadlines from `llam_now_ns()`:

```c
uint64_t deadline = llam_now_ns() + 10ULL * 1000ULL * 1000ULL;

if (llam_sleep_until(deadline) != 0) {
    /* Check errno for cancellation or runtime stop. */
}
```

## Interval timer

**Focused snippet.** Use a waitable timer when a task needs repeated ticks:

```c
static void ticker(void *arg) {
    (void)arg;

    llam_timer_t *timer = NULL;
    if (llam_timer_create(100ULL * 1000ULL * 1000ULL, &timer) != 0) {
        return;
    }

    for (int i = 0; i < 3; ++i) {
        uint64_t ticks = 0;
        if (llam_timer_wait(timer, &ticks) != 0) {
            break;
        }
        printf("tick=%llu\n", (unsigned long long)ticks);
    }

    (void)llam_timer_destroy(timer);
}
```

## CPU-bound loop

**Focused snippet.** Long loops on a compatibility runtime should expose safepoints:

```c
size_t poll_counter = 0;

for (uint64_t i = 0; i < work_items; ++i) {
    process_item(i);
    LLAM_PREEMPT_POLL_EVERY(poll_counter++, 1024U);
}
```
