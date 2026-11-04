# Soak, broker, and release tools

Run the long soak, the broker smoke test, and the verification and release
scripts.

## Runtime soak

`runtime_soak.py` repeatedly runs focused runtime tests for a bounded duration.

```sh
python3 scripts/runtime_soak.py --duration 300 --timeout 60
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--duration <sec>` | `300` | Total soak duration. |
| `--timeout <sec>` | `180` | Per-command timeout. |
| `--seed <n>` | fixed LLAM seed | Runtime fuzz seed. |
| `--fuzz-scenarios <n>` | `128` | Single-runtime fuzz scenarios. |
| `--multi-fuzz-scenarios <n>` | `64` | Multi-runtime fuzz scenarios. |
| `--runtime-fuzz <path>` | `./test_runtime_fuzz` | Runtime fuzz test binary. |
| `--multi-runtime-core <path>` | `./test_multi_runtime_core` | Multi-runtime core test binary. |
| `--runtime-stress <path>` | `./test_runtime_stress` | Runtime stress test binary. |
| `--runtime-shutdown <path>` | `./test_runtime_shutdown_internal` | Shutdown test binary. |
| `--io-buffers <path>` | `./test_io_buffers` | I/O buffer test binary. |

## Broker

`llam_broker` is a broker/capability control-plane smoke hook. It is useful for
testing local transport setup and capability validation, not as a production
broker daemon.

| Command | Meaning |
| --- | --- |
| `./llam_broker --self-test` | Run in-process broker capability smoke. |
| `./llam_broker --serve <name>` | Serve local transport forever. |
| `./llam_broker --serve-once <name>` | Serve one local transport connection. |
| `./llam_broker --serve-n <name> <connections>` | Serve a bounded number of connections. |
| `./llam_broker --client-self-test <name>` | Run a client self-test against a local transport name. |
| `./llam_broker --help` | Print usage. |

## Verification scripts

| Script | Platform | Use |
| --- | --- | --- |
| `scripts/verify_linux.sh` | Linux | Linux local verification gate. |
| `scripts/docker_verify_linux.sh` | Linux host with Docker | Linux verification inside the configured image. |
| `scripts/verify_darwin.sh` | macOS | Darwin local verification gate. |
| `scripts/verify_windows.ps1` | Windows / WSL | Default WSL-oriented verification. |
| `scripts/verify_windows.ps1 -Native` | Windows | Native Windows CMake and CTest gate. |

## Release scripts

Release packaging is intentionally strict: scripts reject unsafe paths,
unexpected links, special files, and archive layout mismatches before producing
or installing artifacts.

| Command | Use |
| --- | --- |
| `./scripts/package_release.sh [target]` | Package the current POSIX host target. Target defaults from `uname`. |
| `.\scripts\package_release_windows.ps1` | Package the native Windows target. |
| `./scripts/install.sh --prefix "$HOME/.local"` | Install a release archive on POSIX. |
| `.\scripts\install.ps1 -Prefix "$env:LOCALAPPDATA\LLAM"` | Install a release archive on Windows. |

See [Options](../options/build.md#install-script) for install parameters.
