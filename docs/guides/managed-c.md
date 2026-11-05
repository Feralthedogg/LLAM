<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSE.
-->

# Managed C

`llam_runtime_create()` and `llam_runtime_init()` select required asynchronous
preemption by default. On a qualified native profile, compiler-qualified CPU
loops are interrupted without an application poll or yield. Unsupported profiles
and unqualified task entries fail with `ENOTSUP`; the default does not fall back
to cooperative execution. `llam_runtime_create_managed()` remains the advanced
interface for an explicit policy and capsule/native-affine limits.

Use `llam_runtime_create_compat()` or `llam_runtime_init_compat()` for ordinary
native C callbacks that need the former boundary/safepoint contract. The
caller-sized singleton form is `llam_runtime_init_compat_ex()`. These explicit
compatibility runtimes retain the `AUTO`/`STRICT` cooperative request policies.

Standard release archives provide the runtime SDK. They do not bundle this managed
producer or LLVM.

The managed compiler emits LLPM2 records after instruction selection and
validates the final linked image. The runtime discovers these records, checks
the loaded image and retains an independent loader reference. Passing an
ordinary function pointer or copying metadata into heap memory does not qualify
that function for asynchronous preemption.

## Build and link

The managed producer requires LLVM **23.1.2**. Build the runtime and its matching
compiler plugin together; see the [build guide](../start/build.md) for platform
dependencies. A local SDK prefix can be prepared with:

```sh
cmake -S . -B build-managed \
  -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON \
  -DLLAM_MANAGED_TOOLING=required \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build-managed -j4
cmake --install build-managed --prefix "$PWD/sdk"
```

Compile task translation units with the installed driver and use the driver
again for the final link. Compile host setup code in native mode. For a POSIX
shared-library SDK:

```sh
export LLAM_LLVM_BIN=/path/to/llvm/bin
sdk/bin/llam-clang --llam-managed=off -Isdk/include -O2 -c host.c -o host.o
sdk/bin/llam-clang -Isdk/include -c tasks.c -o tasks.o
sdk/bin/llam-clang host.o tasks.o \
  -Lsdk/lib -lllam_runtime -Wl,-rpath,"$PWD/sdk/lib" -o managed-demo
./managed-demo
```

| `--llam-managed=` | Accepted by | Effect |
| --- | --- | --- |
| `required` (default) | `llam-clang`, `llam-managed-clang` | Rejects unsupported effects. |
| `auto` | `llam-clang`, `llam-managed-clang` | Leaves functions with unknown effects in the legacy class. |
| `off` | `llam-clang` only | Runs the native compiler. `llam-managed-clang` exits with status 2. |

Both managed modes require the matching plugin and final-image verifier; a missing
tool is an error. LTO, sanitizer instrumentation, section rewriting and link folding
are outside this compiler profile and are rejected.

## Required compiler and SDK CI

The ordinary Linux push/PR workflow includes `linux-managed-compiler-sdk` as a
required prerequisite of its native build matrix. It downloads the exact LLVM
23.1.2 release archive with a pinned SHA-256, configures
`LLAM_MANAGED_TOOLING=required`, and checks the compiler, public runtime import
contracts, a real admitted image and the relocated installed SDK. Missing,
duplicated, skipped or failed stages fail the job; automatic fallback cannot
turn an absent compiler into a green check. Its result is explicitly propagated
to the existing native check names, so a failed prerequisite is not reported as
successful skipped matrix checks. The LLVM 14 legacy metadata lane
remains separate.

The SDK check installs the complete package, relocates it into a path containing
spaces, and compiles/links/runs a cross-translation-unit consumer using only its
installed headers, shared runtime, driver, plugin and verifier. It also exercises
blocking callback integration, unknown-effect rejection and missing-plugin errors.

To run this same chain against an existing required-mode build:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify_managed_ci.py \
  --build-dir build-managed --report-dir .artifacts/managed-ci --parallel 4
```

The build must have been configured with `LLAM_MANAGED_TOOLING=required` and the
matching LLVM development package. Reports preserve the registered CTest
inventory, build log and JUnit results.

The plugin follows the selected LLVM package's linkage model. Shared LLVM
packages link its shared target; official Linux bundles with static tools use
LLVM's loadable-module rules and the tools' exported plugin API. The plugin
does not embed a separate LLVM registry. Both forms must pass the same compiler
and installed-SDK checks.

## Create and run tasks

An ordinary task can use calls, local arrays, atomics and supported arithmetic.
For example, `tasks.c` can contain:

```c
#include <stdint.h>

void cpu_task(void *argument) {
    uint64_t *values = argument;
    uint64_t value = values[0];
    for (uint64_t i = 0; i < 2000000; ++i)
        value = value * UINT64_C(6364136223846793005) + i;
    values[1] = value;
}
```

The host keeps task arguments alive until completion:

```c
#include <llam/managed.h>
#include <stdint.h>
#include <stdio.h>

void cpu_task(void *);

int main(void) {
    llam_runtime_opts_t runtime_options;
    llam_runtime_t *runtime = NULL;
    uint64_t first[2] = {7, 0}, second[2] = {11, 0};
    if (llam_runtime_opts_init(&runtime_options, sizeof runtime_options))
        return 1;
    runtime_options.deterministic = 1;
    if (llam_runtime_create(&runtime_options, sizeof runtime_options, &runtime)) {
        perror("managed create");
        return 1;
    }
    llam_task_t *a = llam_runtime_spawn_ex(runtime, cpu_task, first, NULL, 0);
    llam_task_t *b = llam_runtime_spawn_ex(runtime, cpu_task, second, NULL, 0);
    if (!a || !b || llam_runtime_run_handle(runtime) || llam_join(a) || llam_join(b)) {
        perror("managed execution");
        llam_runtime_destroy(runtime);
        return 1;
    }
    printf("%llu %llu\n", (unsigned long long)first[1],
           (unsigned long long)second[1]);
    llam_runtime_destroy(runtime);
    return 0;
}
```

The default `ASYNC_REQUIRED` policy rejects legacy entries, incompatible whole-task `NO_PREEMPT`
and unavailable native profiles before user entry. The refusals are `ENOTSUP`, except
that creation with a `preempt_mode` of `OFF` or `COOPERATIVE` and spawn with
`LLAM_SPAWN_F_NO_PREEMPT` fail with `EINVAL`. With `ASYNC_AUTO`, qualified
tasks use asynchronous preemption when available. Accepted legacy tasks run on
an isolated compatibility lane and the effective policy reports `MIXED`.
An unqualified legacy busy loop has no asynchronous scheduling guarantee.

## Calls, native TLS, and blocking

The compiler checks exact runtime API signatures and calling conventions.
Portable code preserves its M's native signal mask. Signal-set creation and
destruction carry native-affine effects because their subscription mask and
thread cookie belong to an OS thread. Foreign-fast contracts must preserve
native thread state; use a native-affine callback for persistent changes.
The Darwin native profile owns asynchronous signal masks on managed Ms. It
blocks asynchronous delivery while G continuations can be saved, reapplies
that policy after native callbacks, and restores a returning host M's original
mask after native roots drain. Signal-set APIs consume blocked signals;
synchronous fault diagnostics remain enabled. Native-affine TLS destructors
retain the owned mask until their dedicated OS thread exits.
Calls between translation units in the same final image can use
`LLAM_MANAGED_TARGET(function_name)(arguments)`. The target must have a matching
managed definition in that image. Unknown dynamic function pointers, escaping
markers and unresolved cross-image managed targets are rejected in `required`
mode. Statically bounded local target sets are checked before lowering to direct
calls.

| Execution class | Placement and native effects |
| --- | --- |
| `PORTABLE` | Shared managed workers; complete asynchronous snapshots restore on their original M with a fresh P lease. |
| `NATIVE_AFFINE` | A dedicated OS thread preserves native TLS and thread identity through native thread exit. |
| `LEGACY` | Compatibility execution; rejected by `ASYNC_REQUIRED`. |

`LLAM_SPAWN_F_NATIVE_AFFINE` can reserve a dedicated M for an already-qualified
entry. It cannot qualify arbitrary native code. `PINNED` retains its existing
placement meaning and does not promise native thread identity.

Cooperative portable continuations can move between Ms. LLAM restores logical
task `errno` through a fresh native accessor after each switch. Native TLS
addresses retained by application C frames require the native-affine class.

Use `llam_call_blocking_result()` to offload foreign work. A native-affine task
that must call foreign code on its own OS thread can use
`llam_call_native_affine_result()`. This API drains earlier native operations,
returns P before the callback and reacquires a fresh P on the same M before
returning its result. The callback has no managed G/P authority; LLAM task APIs
are rejected from it. Callback completion preserves its result, `errno` and
Windows `LastError`. Cancellation before callback entry returns `ECANCELED`;
after callback entry the runtime first completes the native return handoff.

## What each context can call

`required` mode checks every call a qualified function makes. A call with no
contract stops the compile with `LLAM managed effect/ABI is unqualified`; `auto` mode
instead leaves that function in the legacy class.

| API group | Host thread | Compatibility task | Qualified function |
| --- | --- | --- | --- |
| Spawn and join, sync, channels, timers, cancellation, byte I/O, owned buffers, DNS, object constructors, stop requests, `llam_runtime_get_resources_ex`, `llam_call_blocking_result`, `llam_now_ns`, `llam_wait_opts_init`, `llam_io_buffer_opts_init` | Per API; see [Threading](../reference/abi/threading.md) | Yes | Yes |
| Signal sets, `llam_task_local_set_owned` | Signal sets only | Yes | Yes; the function becomes `NATIVE_AFFINE` |
| `llam_call_native_affine_result` | `ENOTSUP` | `ENOTSUP` | Yes; the function becomes `NATIVE_AFFINE` |
| `llam_poll_fd`, `llam_poll_handle` | Yes | Yes | Rejected: narrow integer ABI |
| `llam_enter_blocking`, `llam_leave_blocking` | No-op | Yes | Rejected |
| Stats, JSON stats, dumps, `llam_runtime_preempt_info_ex` | Yes | Yes | Rejected |
| Runtime create and init, `llam_runtime_default`, run, drain, shutdown, destroy, module registration, Windows opt-in | Yes | Per API: constructors and run drivers get `EBUSY`, except the `*_compat` constructors; shutdown and destroy only request a stop | Rejected |
| `llam_abi_version`, `llam_abi_get_info`, `llam_version_string`, `llam_runtime_opts_init`, `llam_spawn_opts_init`, `llam_preempt_opts_init` | Yes | Yes | Rejected |

`llam_managed_native_enter` and `llam_managed_native_leave` are for the code the
compiler generates, in every context.

Callbacks have their own rules:

| Callback | What it can call |
| --- | --- |
| `llam_call_blocking_result` | It runs on a helper OS thread with no task, so the host-thread rules apply. |
| `llam_call_native_affine_result` | Only `llam_now_ns`, `llam_abi_version`, `llam_abi_get_info`, `llam_version_string`, `llam_runtime_opts_init`, `llam_spawn_opts_init`, `llam_wait_opts_init` and `llam_io_buffer_opts_init`. Every other public function fails with `ENOTSUP`. |

Keep setup and reporting in host code compiled with `--llam-managed=off`. When task
code needs something the first table rejects, use the replacement:

| Instead of | Use | Why |
| --- | --- | --- |
| `llam_poll_fd` | `llam_poll_fd_until` or `llam_poll_fd_ex` | These take an `int` mask. |
| `llam_poll_handle` | `llam_poll_fd_until` on POSIX, where a handle is a descriptor; a blocking callback on Windows | |
| `llam_enter_blocking` | `llam_call_blocking_result` | Keeps the worker free. |
| Foreign work tied to the task's OS thread | `llam_call_native_affine_result` | Blocks only this task's own thread. |
| `printf`, `strlen`, other unlisted calls | A callback compiled with `--llam-managed=off` | A callback in a managed file is checked too. |

Use a `short` local for `revents` when the task needs to distinguish readable input
from hangup or error events. Pass `NULL` when only the poll's return value is needed.

For example, this task waits for input and then logs a line. Compile `callbacks.c` with
`--llam-managed=off` and `tasks.c` with the managed driver:

```c title="callbacks.c"
#include <stdio.h>

void *log_line(void *arg) {
    return puts(arg) >= 0 ? arg : NULL;
}
```

```c title="tasks.c"
#include <llam/runtime.h>
#include <poll.h>

void *log_line(void *);

/* 0 once fd is readable and the line is logged, -1 otherwise. */
int wait_then_log(llam_fd_t fd, char *line) {
    uint64_t deadline = llam_now_ns() + UINT64_C(1000000000);
    void *logged = NULL;
    short revents = 0;

    if (llam_poll_fd_until(fd, POLLIN, deadline, &revents) <= 0 ||
        (revents & POLLIN) == 0) {
        return -1;
    }
    if (llam_call_blocking_result(log_line, line, &logged) != 0) {
        return -1;
    }
    return logged != NULL ? 0 : -1;
}
```

### Foreign calls and effects

Only `malloc`, `calloc`, `realloc`, `free`, `memcpy`, `memmove`, `memset` and `memcmp`
are recognized libc calls, and they need their standard signatures. Reading or
writing `errno`, or any thread-local variable, makes the function `NATIVE_AFFINE`.

A foreign declaration can state its effect with an annotation. The compiler cannot
check the function behind it, so the annotation is your promise:

```c
__attribute__((annotate("llam-foreign-fast")))
extern long fast_mix(long);
```

| Annotation | Applies to | Result |
| --- | --- | --- |
| `llam-foreign-fast` | Foreign declaration | Callable; the caller stays `PORTABLE`. The function must return promptly, must not block, and must preserve native thread state. |
| `llam-native-affine` | Declaration or definition | Callable; the caller becomes `NATIVE_AFFINE`. |
| `llam-foreign-blocking` | Foreign declaration | Rejected: an inline call cannot park. Use `llam_call_blocking_result`. |
| `llam-legacy` | Definition | Compiles in the legacy class even in `required`. `ASYNC_REQUIRED` then refuses it at spawn. |
| `llam-no-preempt` | Definition | Rejected in `required`. |

## Policy, resources, and module lifetime

The [field tables](../reference/options/runtime.md#managed-runtime-options) list each
option, counter, and flag.

`llam_runtime_preempt_info_ex()` uses the usual caller-size handshake. Inspect
requested and effective policies, the reason, `RUNNING`/`COMMITTED` flags,
capsule charges, entry counts and available native-affine capacity. A pending
or failed startup probe is not a committed asynchronous profile.

`prepared_capsules` and `available_capsules` describe the bounded prepared
pool. `reserved_capsule_bytes` includes both free and in-use slots;
`capsule_charge_bytes` counts slots retained by admitted tasks. These are
storage-accounting values, not RSS. Preparation scales with the selected M
count and budget. A native cold worker replenishes the pool without holding P;
managed spawn returns `EAGAIN` if a prepared slot is temporarily unavailable,
or `ENOMEM` at the declared quota, before publishing G.

An affine slot becomes available again after its previous native thread has
exited, TLS destructors and native readers have drained, and a fresh OS thread
has passed its native probe. A delayed join of the old G cannot release a newer
slot reservation. `EAGAIN` during that refill is retryable admission failure.

Dynamic workers soft-park idle portable Ms and offline only empty, unowned Ps.
Queues, timers, live allocation owners and in-flight I/O prevent P offlining;
wake resources remain open, and publication or pressure can reactivate P.
Portable native threads remain bound until run end because saved C frames may
retain native TLS addresses. Affine M replacement happens only after native
thread exit and complete G retirement.

An idle portable M can defer a P claim while other portable Ms own P or have made
cooperative progress within the existing 100µs retry interval. This avoids
contending on every P return. Origin-bound READY work and dedicated affine Ms
retain their wait positions; all successful claims still validate a fresh lease.

`effective_quantum_ns[3]` is indexed by `llam_task_class_t` and uses the same
calculation as native capture requests. Zero means no active qualified native
profile. This is a request budget: native delivery, PC eligibility and reader
drain can delay actual preemption. It is not a hard scheduling deadline.

Creation and module mutation are host operations. Pass at least the two metadata
fields of `llam_module_opts_t`, 16 bytes, and keep the caller's loader
reference alive during `llam_runtime_module_register_ex()`; LLAM obtains its own
reference. Mutation requires stopped dispatch and no retained G/native/catalog
references. `EBUSY` preserves the module handle; success consumes it. Another
runtime's handle returns `EXDEV`. A loader may retain an image for its own TLS
lifetime even after LLAM has released its final independent reference.

The public API is an ABI 2.x minor extension. New structures retain fixed-width
fields, compatible-prefix reads and writes, and defaults for omitted fields.
See the [ABI contract](../reference/abi/index.md) and
[platform support](../start/platforms.md) for the applicable contracts and gates.
