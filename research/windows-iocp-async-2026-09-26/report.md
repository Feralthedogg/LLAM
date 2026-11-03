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

# Windows IOCP attribution and experimental async-preemption handoff

## Outcome

The Windows IOCP experiment separated raw IOCP (A), LLAM's I/O engine without
task wake (B), managed wake (C), and the validated full echo workload (D).
On one shared Windows host, the final six measured 256-record rounds per layer
had median throughput of 31,137, 30,468, 23,483 and 21,430 records/s,
respectively. The workload used one credit and exactly 512 IOCP reads per
round. These non-additive layers point toward completion-to-managed-task
wake/dispatch as the next measurement target, but do not prove a specific
instruction, lock, or stable platform speedup. A metrics-lock removal
candidate showed no paired improvement and was reverted.

The Windows context oracles first qualified a live native M return and fresh
restore gate with full-context capture/restore. This continuation moved those
transitions into a private opt-in Win64/MinGW runtime path, retaining two
preallocated CONTEXT/XSTATE banks per G, bounded capsule memory,
acquire-before-suspend task/bank/binding references, deferred DEAD-G and
claimed-finalizer retirement, and pre-unbind drain. The ordinary Windows
context switch and public C ABI were not changed.

The existing single watchdog thread now makes bounded automatic probes when
the private backend has been explicitly enabled before any G is live. A
capture requires the actual suspended RIP in a certified PE image-code range.
The runtime holds loader references so those image mappings cannot be
unloaded during its lifetime; executable private/JIT mappings are rejected.
A later read-only suspension can witness the restored G even outside a
capture-safe range, retire the old bank, and permit another timeslice. A
no-safepoint G completed four automatic capture/restore cycles with peer
progress and no manual capture call. Unsafe-PC, empty-registry and NO_PREEMPT
controls made no context mutations. This is repeatable automatic preemption
for a narrow, default-off, one-M certified-code subset—not general Windows
async preemption, arbitrary C interruption, or asynchronous cancellation.

This continuation qualified public host-side `llam_runtime_destroy()` overlap
with that one-M backend. Five test-only, lock-free pauses cover destroy before
first dispatch, at captured READY, at the native restore gate, immediately
after remote restore but before its witness, and on a second run driven by a
different native caller M. The controller retains its task/bank/binding
operation through READY; it is not idle there. The post-restore case keeps G
busy until the direct witness and final binding release, then lets the
bounded leaf exit. All five cases drained without an abandoned restore or
premature runtime teardown. Those five cases do not by themselves qualify
registration/enablement races, controller startup failure, public activation
or concurrent multi-M.

A subsequent private configuration gate now pins the runtime before safe-range
registration or enablement touches it, serializes those operations, excludes
run start with a temporary execution claim, and reserves host/foreign spawns
until complete task publication. An enabled task must have its async capsule
before first dispatch. Six native cases exercise both spawn/enable orderings,
public destroy, active run, foreign managed-spawn trylock refusal, and injected
controller-start failure. This closes those pre-run races within the existing
private one-M contract; it does not create a public activation API.

A further startup characterization holds the already-created controller at
its first action, lets one certified G finish and its borrowed M unbind, then
starts the controller. Several real idle ticks must not capture from that
retired run. A second run on another caller M must still complete a real
automatic capture, restore and direct witness. This tests delayed scheduling
and stale-binding refusal without treating controller-thread creation as
proof that its loop has run. Only the watchdog loop originates automatic
capture in production.

The backend now has one expert-only public, all-or-nothing opt-in call. It
requires the caller to certify every possible interrupted PC in copied,
nonoverlapping PE-image ranges and explicitly acknowledge that uncertain
committed context mutation can fail-fast the **entire process**. LLAM checks
numeric/image/machine/lifetime conditions; it does not prove instruction or
unwind semantics. The public call is first-worker-run-only; even a completed
older run is rejected because its handles may still be reclaimed. This
additive ABI-2.1 call remains default-OFF, one-M,
Win64/MinGW experimental, and one-way until shutdown. Unsupported builds
export the same symbol with `ENOTSUP`; no public disable or multi-M promise
was added.

Uncertain remote context or suspension ownership in this opt-in path now uses
Windows `RaiseFailFastException`. It terminates the **hosting process**, not
only one LLAM runtime. This is distinct from supported pre-mutation refusal,
ordinary stop/cancel, or a legitimately pending restore. The child-process
test requires exact `0xC0000602` terminal status after actual capture and
restore context mutations, and status 0 for a refusal/valid-restore control.
This policy follows the [Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception).

## Verification

- Native Windows 10 x64, MinGW GCC 15.1 Release, isolated build, at most two
  build jobs and serial short CTests: final focused matrix **38/38 passed in
  4.99 seconds**. It includes production-linked explicit transaction and
  safe-PC tests, four-cycle automatic preemption, stop during admitted
  restore, unsafe/empty/NO_PREEMPT negatives, PE image-loader pin/JIT
  rejection, five public-destroy overlap phases, prior lifetime/fail-fast/
  two-capsule oracles, runtime core, multi-runtime core, shutdown, Windows
  policy/smoke, IOCP I/O/dump and HANDLE I/O. The manual four-cycle,
  automatic four-cycle, automatic-stop and destroy phases A–D each passed
  20 short repeats; the image-pin, unsafe/empty and second-M destroy cases
  passed ten repeats. The earlier [17-case lifetime log](host-logs/windows-lifetime-final-17-ctest.log)
  remains available; final commands and results are recorded in
  [evidence.md](evidence.md).
- macOS `make -j4 test` passed, including ABI/export, runtime stress/fuzz,
  static lease-boundary and shared-load gates. Cross-MinGW syntax checks of
  changed C files passed with `-Wall -Wextra -Wpedantic -Werror`.
  A default-OFF Win64 DLL built with no async transition sources or private
  exports, and the opt-in native DLL exported no private async symbols.
  `make check-license-policy` and `git diff --check` passed. A separate
  `check_worker_identity_boundary.py` run still reports six direct `active_g`
  references in pre-existing `init.c`, `scheduler.c`, and `runtime_types.h`
  lines untouched by this Windows slice; it is not a passing check.
- After the destroy-oracle continuation, macOS focused
  `make -j4 test_runtime_core && ./test_runtime_core` passed; the default-OFF
  Win64 shared build, license policy, lease-boundary check and diff check
  passed. The opt-in DLL did not export the test-only phase-gate symbol.
- After the private configuration continuation, every native Windows async
  target was rebuilt and the async matrix passed **36/36 in 3.00 s**. The six
  new configuration cases each passed 20 short serial repeats. Native runtime
  core, multi-runtime core and Windows smoke passed separately. Local macOS
  runtime core/multi-runtime core and full `make test`, strict changed-file
  MinGW syntax, the default-OFF Win64 DLL build, license policy and diff
  checks passed. Extra High's one conditional review point—foreign managed
  spawn must fail before
  allocation if its config trylock fails—was verified in source and with a
  native deterministic case; no independent review execution was claimed.
- After the delayed-controller continuation, the new native case passed 20
  serial repeats and the fully rebuilt Windows async suite passed **37/37 in
  3.15 s**. Native runtime core, multi-runtime core and Windows smoke passed.
  Strict changed-file MinGW checks and both default-OFF cross and opt-in
  native DLL builds passed; the opt-in DLL exported no private test gate. The
  first local macOS `make test` attempt hit a legacy-lease helper deadline
  while another local cross build ran; the focused test passed alone, and the
  full `make test` passed after the cross build finished. This is recorded as
  an initial test failure, not silently counted as a first-pass pass.
- After the expert opt-in continuation, the production-linked public success
  and rejection cases plus four public-entry race cases each passed 20 short
  native repeats. The rebuilt Windows async matrix passed **43/43 in 3.50 s**.
  Native ABI contract/compat, preemption capability, runtime core,
  multi-runtime core and smoke passed. The formerly POSIX-only shared-load
  test now runs with the Windows loader and passed against both opt-in and
  default-OFF DLLs. Both DLLs export the new public symbol but no private
  Windows async helper; the default-OFF build returns `ENOTSUP`. Strict MinGW
  compilation, macOS ABI/capability/shared-load/export checks, Apache license
  policy and diff checks passed. The first macOS full `make test` attempt
  failed an unrelated worker-identity `sigaltstack EINVAL` expectation; that
  focused binary passed alone and the complete `make test` passed on retry.
- A final source audit verified the public managed-caller refusal reads TLS
  before pinning the runtime, and that the watchdog/operation/reclaim/
  diagnostic paths acquire the enabled publication flag before lock-free
  access to control/ranges. It exposed a prior-run handle-reclamation gap;
  public enable now rejects any runtime whose primary worker has previously
  run. A native negative test covers that `EBUSY` case. After this adjustment,
  the rebuilt Windows async matrix again passed **43/43**, public success and
  negative cases each passed 20 repeats, and native runtime core,
  multi-runtime core and shared DLL load passed.
- Native Windows default/disabled behavior had one regression during the
  intermediate patch: `test_runtime_core` timed out because a capsule-less
  task acquired a scheduler scan ref before its synchronous M-side reclaim.
  Restricting the new handoff pin to capsule-bearing Gs restored the ordinary
  path; the final native runtime core test passed in 0.20 seconds.
- `mkdocs build --strict` was unavailable locally because `mkdocs` is not
  installed; the added documentation links and notices were checked directly.

The test-host usage was checked before the bounded runs (roughly 7–19% CPU
and 10–16 GiB free memory). No large stress or soak test was run there.

## Reproduction and scope boundary

On native Windows x64 with MinGW, configure the experimental qualification build
with `-DLLAM_ENABLE_WINDOWS_BACKEND=ON`,
`-DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=ON`, and
`-DLLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE=ON`. Build the Windows async CTest
targets with at most two jobs on a shared host and run them serially with
short timeouts. The experimental build flag alone does not activate the
backend. The public expert opt-in call must atomically supply caller-certified
PE-image ranges and explicit process-fail-fast acknowledgement before the
first worker run and before task admission. The existing public preemption
mode still governs cooperative
behavior when the backend is disabled or the stopped PC is not
capture-certified. See [docs/build.md](../../docs/build.md)
for the exact qualification and failure contract.

ChatGPT Pro turns through `T-PRO-WIN-13` accepted only R1–R4 lifetime scope.
Pro then became unavailable in the user-designated conversation; the user
explicitly authorized Extra High. `T-XHIGH-WIN-15` through `-25` reviewed
the controller, destroy and private configuration protocols based on our
reports, not independent source inspection or execution. A proposed
stop-after-witness blocker was
withdrawn after the actual scheduler cancellation path was checked. The
registered PE-image lifetime issue was closed by loader references and a
native DLL oracle. `T-XHIGH-WIN-19` accepted only the reported narrow
experimental automatic classification; `T-XHIGH-WIN-22` accepted only the
corrected A–E public-destroy overlap interpretation; `T-XHIGH-WIN-24`
conditionally accepted the private configuration gate, with its foreign
managed-spawn condition verified locally and natively. `T-XHIGH-WIN-25`
classified the delayed-controller case as useful characterization rather
than a new one-M correctness prerequisite. `T-XHIGH-WIN-26` recommended the
single expert-only public transaction with caller certification and explicit
process-wide fail-fast acknowledgement before PE metadata tooling or multi-M.
`T-XHIGH-WIN-27` required a source audit of pre-pin runtime access and the
enabled publication gate; that audit led to the first-run-only restriction and
additional acquire-gated readers described above.
`T-XHIGH-WIN-28` accepted the reported narrow public contract after those
checks, while emphasizing that acquire/release publication is not a substitute
for a runtime lifetime pin on any reader that can overlap destroy.
None is Pro approval,
general Windows support or a performance claim. All source inspection,
implementation, execution and regression reduction were performed in LLAM;
see [state.md](state.md) and
[evidence.md](evidence.md).

An experimental private collector now reads LLPM v1 records from an already
loaded `.llampm` PE section. It holds a temporary loader reference, validates
the loaded image and selected executable ranges, and can hand copied ranges
to the separate public expert call while the references overlap. Native
Windows collector cases passed **10/10**, and the rebuilt Windows async
matrix passed **53/53**. The public path remains default-off and requires
manual semantic certification plus explicit process-fail-fast acknowledgement.
Focused native ABI/runtime/default-OFF and shared-export checks passed. The
unrelated `test_legacy_lease_internal` Windows compile error still prevents
the all-target build; selected async targets and the runtime built.
Extra High reviewed the reported two-stage design in `T-XHIGH-WIN-29`; it did
not independently inspect or run the code, and Pro remained unavailable.
`T-XHIGH-WIN-30` found no reported handoff counterexample and asked for one
explicit PE-bounds check: source audit confirmed the full loaded metadata
span is validated as readable `MEM_IMAGE` under the pinned module before
parsing. This is our source audit, not independent model execution.

At the R7 point, the public opt-in was expert-only and manually certified;
the normal LLAM build still has no Windows compiler metadata emitter. There
is no simultaneous multi-M, wider XSTATE/mitigation or general Win64 compiler
qualification; no guarantee of bounded stop for an uncooperative G; and no
causal IOCP
completion-entry-to-READY latency breakdown. No commit, push, PR, release or
deployment was made.

## R8 narrow compiler-generated certificate prototype

R8 adds an explicit, opt-in LLVM 23.1.2 X86 patch and an LLAM WinGNU IR
diagnostic tag, without changing the public ABI, default runtime policy or R7
collector. For an annotated scalar-integer leaf to receive a 48-byte LLPM v1
record, the compiler pipeline requires LLAM's restrictive IR acceptance,
final register-only machine-body and emission-profile acceptance, an actual
function endpoint in the same one-only COFF code contribution, and an
associative metadata follower. Developer transport and compiler-certification
modes are exclusive; selected late refusals fail the build. The qualified
linker is LLD 23.1.2 with `/opt:ref /opt:noicf`. GNU ld GC, ICF, LTO,
different toolchain versions and broad source shapes remain unqualified.
The [patch and build recipe](../../tools/preempt/patches/README.md) remain
experimental; the default LLAM tooling does not emit PE certificates.

The first end-to-end oracle used the compiler-certified 0x45-byte finite
unsigned recurrence in a DLL alongside a separate scalar certificate and an
uncertified ordinary function. The unchanged native R7 collector returned
exact ranges. The public expert call then enabled a fresh one-M runtime while
holding the module; the caller and collector references were released before
execution, leaving only the runtime's intended pin. One compute G called the
recurrence once; an ordinary peer G stayed runnable and yielded. A bounded
test-only trace verified two sequences on the same task and distinct bank
generations: capture in the recurrence, peer progress, restore, direct
witness, then a second such sequence before the call returned. The validator
checks bank identity plus exact capture/restore task and worker dispatch
generations. The returned value for the fixed 120,000,000-step input matched
the separate unregistered reference:
`10386373181075607123`. The first 30,000,000-step trial returned correctly
but had only one capture, so it was a coverage non-pass, not a corruption
finding. The fixed 120M input is the installed positive; it is not increased
until a test passes. Disabled, short ordinary structural-exclusion and
prepublication invalid-range controls also passed, with no residual runtime
debt or module pin after destroy. The short ordinary control did not prove a
dynamic unsafe-PC refusal. The invalid-range control proved no published
configuration or residual pin, not that a pin had necessarily been acquired
and rolled back.

The packaged patch was applied to a fresh declared Homebrew-patched official
LLVM 23.1.2 source base and a new X86-only `llc` was built. A newly rebuilt
LLAM pass plus that compiler and pinned LLD recreated the DLL; native Windows
collection and the 120M repeated-chain runtime oracle passed again. The
[offline certifier regression](../../scripts/test_windows_async_certifier.sh)
covered one-only/associative COFF fields, GC one/two-record links, N1
incoming stack traffic, N2 generated helper, N3 callee-save stack traffic,
selected ordering, SLS and MC-padding exclusions, mode separation,
stale-output removal and audit-only non-emission. On the final native Windows
tree configured with this clean-reconstructed DLL, all **57/57** async CTests
passed serially in 4.28 seconds. ABI, capability and shared-load checks passed
**4/4** in each of default-OFF and experimental opt-in builds. The shared host
was load-checked; no soak or large stress run was performed. The earlier
unrelated Windows `test_legacy_lease_internal` all-target compile error was
not resolved or hidden by these focused builds.

ChatGPT 6 Pro accepted E3-CERT-1 for the reported compiler-to-collector
profile in `T-PRO-WIN-38` and accepted R8-RUNTIME-1 for the reported fixed
120M one-M oracle in `T-PRO-WIN-39`. Those were design reviews of our evidence,
not independent source execution. After the user unlocked the Mac, the
previously pending actual Linux regression ran in a CPU/memory-capped Ubuntu
24.04 amd64 Docker container with the workspace mounted read-only. Its
matching LLVM 18 pass, linked ELF metadata, parser and registration CTests
passed **3/3** with zero skips. The shared 1.9-GiB Linux server was not
modified. Clean compiler reconstruction, final native Windows CTest and the
affected Linux regression have therefore each passed locally.
`T-PRO-WIN-40` accepted the narrow classification **experimentally
integrated** for the pinned LLVM 23.1.2 extension, qualified LLD link and
WinGNU `scalar-leaf-v1` one-primary-M profile. Pro relied on our reported
evidence, not independent execution or a release audit. The Linux regression
ran under amd64 emulation and does not qualify native Linux hardware or all
LLVM versions. No general Win64 preemption, arbitrary-caller safety,
multi-M support, broader XSTATE/mitigation, other compiler/linker profiles
or performance gain is claimed.
