# Shared library names

Find the shared library file name and SONAME to load on each platform.

CMake builds both a static target and a shared target:

```text
llam_runtime         static library target
llam_runtime_shared  shared library target
```

The Makefile builds the shared library through:

```sh
make shared
```

Expected dynamic artifacts:

```text
Linux:  libllam_runtime.so -> libllam_runtime.so.2 -> libllam_runtime.so.3.0.0
macOS:  libllam_runtime.dylib -> libllam_runtime.2.dylib
Windows: llam_runtime.dll plus llam_runtime.lib and llam_runtime_shared.lib in
         the native Windows x86_64 release archive.
```

Language runtimes should prefer the ABI-major soname/install-name when a
platform supports it.

## Native Windows

Native Windows 10/11 support covers Windows x86_64 context switching, Windows
event wake handles, IOCP policy primitives, and IOCP one-shot socket requests
for `read`, `write`, `accept`, and `connect`, plus generic HANDLE `ReadFile`
and `WriteFile` requests through `llam_read_handle()`, `llam_write_handle()`,
`llam_pread_handle()`, and `llam_pwrite_handle()`. TCP `POLLOUT` readiness is also
IOCP-backed; TCP and UDP `POLLIN` remain fallback by default unless
`LLAM_WINDOWS_IOCP_TCP_POLLIN=1` or `LLAM_WINDOWS_IOCP_UDP_POLLIN=1` is enabled for
controlled smoke or benchmark runs. The release workflow publishes a native Windows
x86_64 archive after its Windows CTest set passes: runtime stress, fuzz, invariants,
shutdown, policy, IOCP, HANDLE I/O, broker, and security tests, plus an opt-in TCP
`POLLIN` IOCP run and a `bench` smoke.
