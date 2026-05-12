param(
    [switch]$Native
)

$ErrorActionPreference = "Stop"

$windowsGeneration = "unknown"
$windowsBuild = 0
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $version = [version]$os.Version
    if ($version.Major -lt 10) {
        Write-Error "Windows 10 or Windows 11 is required; detected $($os.Caption) $($os.Version)."
        exit 1
    }
    $windowsBuild = $version.Build
    $windowsGeneration = if ($version.Major -eq 10 -and $version.Minor -eq 0 -and $version.Build -ge 22000) {
        "windows11"
    } elseif ($version.Major -eq 10 -and $version.Minor -eq 0) {
        "windows10"
    } else {
        "unsupported"
    }
    Write-Host "host platform: $($os.Caption) $($os.Version)"
    Write-Host "windows generation: $windowsGeneration build=$windowsBuild"
} catch {
    Write-Host "host platform: windows-compatible shell"
}

if ($Native) {
    if ($windowsGeneration -eq "unsupported") {
        Write-Error "Native LLAM requires Windows 10 or Windows 11."
        exit 1
    }
    $processorCount = [Environment]::ProcessorCount
    if ($processorCount -lt 1) {
        $processorCount = 1
    }
    Write-Host "native backend policy: IOCP + x86_64 asm context"
    Write-Host "IOCP API: CreateIoCompletionPort/GetQueuedCompletionStatusEx on Windows 10 and Windows 11"
    if ($windowsGeneration -eq "windows11") {
        $iocpConcurrency = $processorCount
        $completionBatch = if ($processorCount -ge 16) { 128 } else { 64 }
        $acceptPrepost = if ($processorCount -ge 8) { 4 } else { 2 }
        $recvPrepost = if ($processorCount -ge 8) { 32 } else { 16 }
        Write-Host "selected strategy: win11-batched"
        Write-Host "selected tuning: processor_count=$processorCount iocp_concurrency=$iocpConcurrency completion_batch=$completionBatch control_batch=32 accept_prepost=$acceptPrepost recv_prepost=$recvPrepost poll_timeout_ms=5 timer_granularity_ms=1 skip_completion_on_success=1"
    } else {
        $iocpConcurrency = if ($processorCount -gt 1) { $processorCount - 1 } else { 1 }
        $acceptPrepost = if ($processorCount -ge 8) { 2 } else { 1 }
        $recvPrepost = if ($processorCount -ge 8) { 16 } else { 8 }
        Write-Host "selected strategy: win10-conservative"
        Write-Host "selected tuning: processor_count=$processorCount iocp_concurrency=$iocpConcurrency completion_batch=64 control_batch=16 accept_prepost=$acceptPrepost recv_prepost=$recvPrepost poll_timeout_ms=10 timer_granularity_ms=10 skip_completion_on_success=0"
    }
    Write-Host "native I/O backend: IOCP request path for WSARecv/WSASend/AcceptEx/ConnectEx, TCP POLLOUT readiness, UDP POLLIN readiness, and opt-in TCP POLLIN readiness; AF_UNIX poll remains fallback."

    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    $buildDir = Join-Path $repoRoot.Path "build-windows-native"
    $cmakeArgs = @("-S", $repoRoot.Path, "-B", $buildDir, "-DCMAKE_BUILD_TYPE=Release")
    if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
        $cmakeArgs += @("-G", "Ninja")
    }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & cmake --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & ctest --test-dir $buildDir --output-on-failure -C Release
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    foreach ($forcedGeneration in @("10", "11")) {
        Write-Host "forced policy verification: LLAM_WINDOWS_FORCE_GENERATION=$forcedGeneration"
        $env:LLAM_WINDOWS_FORCE_GENERATION = $forcedGeneration
        & ctest --test-dir $buildDir --output-on-failure -C Release -R "test_abi_compat|test_runtime_stress|test_runtime_fuzz|test_windows_policy|test_windows_runtime_smoke|test_windows_iocp_io|test_windows_handle_io"
        if ($LASTEXITCODE -ne 0) {
            Remove-Item Env:\LLAM_WINDOWS_FORCE_GENERATION -ErrorAction SilentlyContinue
            exit $LASTEXITCODE
        }
    }
    Remove-Item Env:\LLAM_WINDOWS_FORCE_GENERATION -ErrorAction SilentlyContinue
    exit 0
}

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Error "WSL is not available. Install WSL to verify the Linux backend, or run the native Windows CMake build and tests with -Native."
    exit 2
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$wslPath = (& wsl.exe wslpath -a $repoRoot.Path).Trim()
if ($LASTEXITCODE -ne 0 -or -not $wslPath) {
    Write-Error "Failed to translate repository path for WSL."
    exit 2
}

$escaped = $wslPath.Replace("'", "'\''")
& wsl.exe bash -lc "cd '$escaped' && ./scripts/verify_linux.sh"
exit $LASTEXITCODE
