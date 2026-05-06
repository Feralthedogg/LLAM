param(
    [switch]$Native
)

$ErrorActionPreference = "Stop"

try {
    $os = Get-CimInstance Win32_OperatingSystem
    $version = [version]$os.Version
    if ($version.Major -lt 10) {
        Write-Error "Windows 10 or Windows 11 is required; detected $($os.Caption) $($os.Version)."
        exit 1
    }
    Write-Host "host platform: $($os.Caption) $($os.Version)"
} catch {
    Write-Host "host platform: windows-compatible shell"
}

if ($Native) {
    Write-Error "Native Windows 10/11 backend is planned but not complete. The next backend milestone is IOCP wake/I/O plus Fiber context switching."
    exit 2
}

$wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wsl) {
    Write-Error "WSL is not available. Install WSL to verify the Linux backend on Windows 10/11 until the native IOCP/Fiber backend lands."
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
