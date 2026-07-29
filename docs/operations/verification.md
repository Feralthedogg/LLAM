# Verification

Verification is platform-local. A green Linux run does not prove the kqueue or
Windows IOCP backend.

For script-level parameters and tool usage, see [CLI And Tools](../reference/cli-tools.md).

## Quick Local Gate

```sh
make test
```

This runs focused API, ABI, runtime, broker, and shared-load smoke coverage.

## Platform Gates

Linux:

```sh
make verify-linux CC=gcc
./scripts/docker_verify_linux.sh
```

macOS:

```sh
CC=clang make verify-darwin
```

Windows:

```powershell
.\scripts\verify_windows.ps1 -Native
```

The native mode also runs the Windows evidence-bundle integration gate for
protected SYSTEM-plus-current-token DACLs, compatible directory sharing,
handle-bound no-replace publication, identity/cleanup, reparse and hardlink
rejection, and concurrent finalization. The same test is a required step in
the pull-request `windows-runtime-stress` job. This security boundary requires
native Windows.
Wine currently neither preserves `SE_DACL_PROTECTED` on these filesystem
objects nor implements the required handle-relative directory rename, so Wine
is useful for diagnostics but is not an acceptance environment for the gate.

Windows evidence identity uses the volume serial number plus the 16-byte
`FILE_ID_INFO` identifier; failure to obtain that identifier is fatal. The
native gate prints `NATIVE_WINDOWS_EVIDENCE_FILESYSTEM=<name>` from the actual
test volume and accepts only an observed `NTFS` or `ReFS` name, so an NTFS run
is not reported as ReFS coverage.

For the complete writer transaction, retained root-to-output-parent handles
deny delete sharing. Each pathname component is protected from rename or
replacement while its retained handle remains open, through final publication
classification; the handles are then released during cleanup. This is not a
persistent lock or an ACL authorization check: after the handles close, an
actor that holds `DELETE` on an ancestor or `DELETE_CHILD` on its parent can
rename or replace that component. Such post-transaction authority is outside
the supported threat model. Put evidence beneath a trusted output parent whose
ACL withholds that authority from untrusted principals. Before consuming
evidence, audit the exact final path with `audit_bundle()` or
`--audit-existing` and consume the validated result rather than reopening
artifacts later under an assumed-immutable pathname.

On a Windows writer error, automatic abort is deliberately close-only. The
library never enumerates or deletes through the mutable staging pathname and
may therefore leave its private, high-entropy `.staging-<pid>-<random>`
directory behind. This conservative fallback prevents a substituted foreign
directory from being deleted, at the cost of possible disk residue under
repeated invalid or interrupted writes. Such residue is not proof of ownership
by pathname alone and is never removed automatically.

BSD is covered by the VM workflow in `.github/workflows/bsd.yml`.

## Stress And Soak

```sh
make test-quick
make test-full
make test-soak
make test-runtime-soak
```

`test-runtime-soak` exercises LLAM core runtime behavior without depending on
the example chat server policy.

## Hardening Gate

Run before release candidates or security-sensitive changes:

```sh
make analyze-cppcheck
make audit-deps
make test-fuzz-heavy
make test-process-utils
make test-runtime-soak
make test-hardening
```

## Package Smoke

```sh
make clean all test
./scripts/package_release.sh
```

Or:

```sh
make package
```

Release archives should include public headers, static and shared libraries,
CMake config, pkg-config metadata, examples, install scripts, and operations
docs.
