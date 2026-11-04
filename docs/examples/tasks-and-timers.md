# Tasks and timers

Start with this shape when one task needs to create work and wait for it.

## Spawn and join

**Complete program.** One task creates a child, joins it, and prints its result.

```c title="main.c"
#include <llam/runtime.h>

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
    (void)arg;

    job_t job = {.input = 12};
    llam_task_t *task = llam_spawn(child, &job, NULL);

    if (task != NULL && llam_join(task) == 0) {
        printf("result=%d\n", job.output);
    }
}

int main(void) {
    if (llam_runtime_init_compat(NULL) != 0) {
        return 1;
    }

    llam_task_t *task = llam_spawn(root, NULL, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return 1;
    }
    int rc = llam_run();
    if (rc == 0 && llam_join(task) != 0) rc = -1;
    llam_runtime_shutdown();
    return rc;
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
