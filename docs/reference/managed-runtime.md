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
See LICENSES/Apache-2.0.txt.
-->

# Managed C and asynchronous preemption

`llam_runtime_create_managed()` admits compiler-qualified C tasks through
`<llam/managed.h>`. On a qualified native profile, CPU loops can be preempted
without an application poll or yield. Existing runtime constructors retain
their compatibility behavior.

The managed compiler emits LLPM2 records after instruction selection and
validates the final linked image. The runtime discovers these records, checks
the loaded image and retains an independent loader reference. Passing an
ordinary function pointer or copying metadata into heap memory does not qualify
that function for asynchronous preemption.

## Build and link

The managed producer requires LLVM **23.1.2**. Build the runtime and its matching
compiler plugin together; see the [build guide](../build.md) for platform
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
sdk/bin/llam-clang --llam-managed=required -Isdk/include -c tasks.c -o tasks.o
sdk/bin/llam-clang --llam-managed=required host.o tasks.o \
  -Lsdk/lib -lllam_runtime -Wl,-rpath,"$PWD/sdk/lib" -o managed-demo
./managed-demo
```

`required` rejects unsupported effects. `auto` leaves functions with unknown
effects in the legacy class. Both managed modes require the matching plugin
and final-image verifier; a missing tool is an error. `off` uses the native
compiler. LTO, sanitizer instrumentation, section rewriting and link folding
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
    llam_preempt_opts_t preempt_options;
    llam_runtime_t *runtime = NULL;
    uint64_t first[2] = {7, 0}, second[2] = {11, 0};
    if (llam_runtime_opts_init(&runtime_options, sizeof runtime_options) ||
        llam_preempt_opts_init(&preempt_options, sizeof preempt_options))
        return 1;
    runtime_options.deterministic = 1;
    runtime_options.preempt_mode = LLAM_PREEMPT_STRICT;
    preempt_options.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    if (llam_runtime_create_managed(&runtime_options, sizeof runtime_options,
            &preempt_options, sizeof preempt_options, &runtime)) {
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

`ASYNC_REQUIRED` rejects legacy entries, incompatible whole-task `NO_PREEMPT`
and unavailable native profiles before user entry. With `ASYNC_AUTO`, qualified
tasks use asynchronous preemption when available. Accepted legacy tasks run on
an isolated compatibility lane and the effective policy reports `MIXED`.
An unqualified legacy busy loop has no asynchronous scheduling guarantee.

## Calls, native TLS and blocking work

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

## Policy, resources and module lifetime

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

Creation and module mutation are host operations. Keep the caller's loader
reference alive during `llam_runtime_module_register_ex()`; LLAM obtains its own
reference. Mutation requires stopped dispatch and no retained G/native/catalog
references. `EBUSY` preserves the module handle; success consumes it. Another
runtime's handle returns `EXDEV`. A loader may retain an image for its own TLS
lifetime even after LLAM has released its final independent reference.

The public API is an ABI 2.x minor extension. New structures retain fixed-width
fields, compatible-prefix reads and writes, and defaults for omitted fields.
See the [ABI contract](../abi.md) and
[platform support](platform-support.md) for the applicable contracts and gates.
