---
hide:
  - toc
---

<section class="llam-hero llam-hero-download">
  <div class="llam-hero-copy">
    <p class="llam-eyebrow">Stackful concurrency for C</p>
    <h1>Write blocking C. Run it concurrently.</h1>
    <p>
      LLAM gives every task its own stack and parks waits on native I/O
      backends, so straightforward C code can scale across Linux, macOS,
      and Windows.
    </p>
  </div>

  <div class="llam-download-panel" aria-label="Install LLAM">
    <div class="llam-download-head">
      <span>Install LLAM</span>
      <span>latest release</span>
    </div>
    <div class="llam-install-section">
      <div class="llam-install-label">
        <span>Linux, macOS</span>
        <span>POSIX shell</span>
      </div>
      <div class="llam-command-box">
        <button class="llam-copy-button" type="button" data-copy-command='curl -fsSL https://github.com/Feralthedogg/LLAM/releases/latest/download/install.sh | sh -s -- --prefix "$HOME/.local"' aria-label="Copy POSIX install command">Copy</button>
        <code><span><span class="llam-prompt">$</span>curl -fsSL</span><span>https://github.com/Feralthedogg/LLAM/releases/latest/download/install.sh</span><span>| sh -s -- --prefix "$HOME/.local"</span></code>
      </div>
      <div class="llam-platform-row">
        <span>Linux</span>
        <span>macOS</span>
        <span>Windows</span>
      </div>
    </div>
    <div class="llam-install-section llam-install-section-border">
      <div class="llam-install-label">
        <span>Windows</span>
        <span>PowerShell</span>
      </div>
      <div class="llam-command-box">
        <button class="llam-copy-button" type="button" data-copy-command='Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/latest/download/install.ps1" -OutFile install.ps1&#10;.\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"' aria-label="Copy Windows install command">Copy</button>
        <code><span>Invoke-WebRequest "https://github.com/Feralthedogg/LLAM/releases/latest/download/install.ps1"</span><span>-OutFile install.ps1</span><span>.\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"</span></code>
      </div>
      <div class="llam-platform-row">
        <span>Windows 10</span>
        <span>Windows 11</span>
        <span>x86_64</span>
      </div>
    </div>
  </div>
</section>

## Start Here

<div class="llam-step-grid">
  <a class="llam-step" href="getting-started/#install-release">
    <span>1</span>
    <strong>Install LLAM</strong>
    <p>Use the release installer for your platform.</p>
  </a>
  <a class="llam-step" href="getting-started/#use-from-cmake">
    <span>2</span>
    <strong>Link your app</strong>
    <p>Consume the installed CMake or pkg-config metadata.</p>
  </a>
  <a class="llam-step" href="getting-started/#minimal-program">
    <span>3</span>
    <strong>Run a task</strong>
    <p>Spawn stackful work and let the runtime schedule it.</p>
  </a>
</div>

## Run The First Task

```c
#include <llam/runtime.h>
#include <errno.h>

static void child(void *arg) {
    (void)arg;
    int error = llam_sleep_ns(1000000) == 0 ? 0 : errno;
    (void)llam_task_set_result(NULL, error);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    if (llam_runtime_create(NULL, 0, &runtime) != 0) return 1;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, child, NULL, NULL, 0);
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

## Choose What You Need

<div class="llam-card-grid llam-card-grid-wide">
  <a class="llam-card" href="getting-started/">
    <h3>Start a new app</h3>
    <p>Install LLAM, link through CMake, and run the smallest working program.</p>
  </a>
  <a class="llam-card" href="examples/">
    <h3>Copy an example</h3>
    <p>Pick a focused task, channel, I/O, embedding, or blocking-work snippet.</p>
  </a>
  <a class="llam-card" href="guides/embedding/">
    <h3>Embed LLAM</h3>
    <p>Create explicit runtimes and keep host-owned lifecycle boundaries clear.</p>
  </a>
  <a class="llam-card" href="concepts/runtime-handles/">
    <h3>Runtime Handles</h3>
    <p>Understand owner checks, cross-runtime use, and handle lifetime rules.</p>
  </a>
  <a class="llam-card" href="concepts/io-model/">
    <h3>Runtime I/O</h3>
    <p>Use blocking-looking calls while tasks park on io_uring, kqueue, or IOCP.</p>
  </a>
  <a class="llam-card" href="abi/">
    <h3>Bind from another runtime</h3>
    <p>Use size-aware structs, fixed-width fields, and ABI metadata safely.</p>
  </a>
  <a class="llam-card" href="operations/verification/">
    <h3>Verify a release</h3>
    <p>Run platform-local tests, stress, soak, sanitizer, and package gates.</p>
  </a>
  <a class="llam-card" href="guides/diagnostics/">
    <h3>Debug a hang</h3>
    <p>Capture stats, runtime dumps, wait ownership, and timeout artifacts.</p>
  </a>
  <a class="llam-card" href="security/">
    <h3>Check the boundary</h3>
    <p>Separate in-process handle hardening from broker-mode isolation.</p>
  </a>
</div>

## Supported Backends

<div class="llam-status-grid">
  <div class="llam-status">
    <strong>Linux x86_64 / aarch64</strong>
    <span>Primary</span>
    <p>io_uring/liburing backend with platform-local verification and benchmark gates.</p>
  </div>
  <div class="llam-status">
    <strong>macOS arm64 / x86_64</strong>
    <span>Supported</span>
    <p>kqueue readiness, Darwin-specific wake tuning, and native verification paths.</p>
  </div>
  <div class="llam-status">
    <strong>Windows 10 / 11</strong>
    <span>Supported</span>
    <p>Native IOCP socket and HANDLE I/O with Windows generation-specific policy tests.</p>
  </div>
</div>

## Keep Going

<div class="llam-link-row">
  <a href="concepts/execution-model/">Execution model</a>
  <a href="examples/">Examples</a>
  <a href="reference/api/">Public API</a>
  <a href="reference/options/">Options</a>
  <a href="reference/cli-tools/">CLI and tools</a>
  <a href="reference/environment/">Environment variables</a>
  <a href="operations/benchmarks/">Benchmarks</a>
  <a href="internals/architecture/">Architecture</a>
  <a href="project/documentation-guide/">Docs guide</a>
</div>
