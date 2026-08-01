# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

$ErrorActionPreference = "Stop"

$SourceRoot = Split-Path -Parent $PSScriptRoot
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("llam-license-layout-" + [guid]::NewGuid().ToString("N"))
$Repo = Join-Path $TempRoot "repo"
$Build = Join-Path $Repo "build\Release"
$ActiveLicense = "LICENSES\LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
$CurrentText = "current license`n"

if (-not $IsWindows -and -not ("LlamWin32FileInfo" -as [type])) {
    Add-Type -TypeDefinition @"
using System;

public static class LlamWin32FileInfo {
    public const UInt32 FILE_READ_ATTRIBUTES = 0x80;
    public const UInt32 FILE_SHARE_READ = 0x1;
    public const UInt32 FILE_SHARE_WRITE = 0x2;
    public const UInt32 FILE_SHARE_DELETE = 0x4;
    public const UInt32 OPEN_EXISTING = 3;
    public const UInt32 FILE_ATTRIBUTE_NORMAL = 0x80;
    public static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

    public struct FILETIME {
        public UInt32 LowDateTime;
        public UInt32 HighDateTime;
    }

    public struct BY_HANDLE_FILE_INFORMATION {
        public UInt32 FileAttributes;
        public FILETIME CreationTime;
        public FILETIME LastAccessTime;
        public FILETIME LastWriteTime;
        public UInt32 VolumeSerialNumber;
        public UInt32 FileSizeHigh;
        public UInt32 FileSizeLow;
        public UInt32 NumberOfLinks;
        public UInt32 FileIndexHigh;
        public UInt32 FileIndexLow;
    }

    public static IntPtr CreateFileW(
        string fileName,
        UInt32 desiredAccess,
        UInt32 shareMode,
        IntPtr securityAttributes,
        UInt32 creationDisposition,
        UInt32 flagsAndAttributes,
        IntPtr templateFile) {
        return new IntPtr(1);
    }

    public static bool GetFileInformationByHandle(
        IntPtr fileHandle,
        out BY_HANDLE_FILE_INFORMATION fileInformation) {
        fileInformation = new BY_HANDLE_FILE_INFORMATION();
        fileInformation.NumberOfLinks = 1;
        return true;
    }

    public static bool CloseHandle(IntPtr handle) {
        return true;
    }
}
"@
}

function Write-Utf8File([string]$Path, [string]$Content) {
    $Parent = Split-Path -Parent $Path
    if ($Parent) {
        New-Item -ItemType Directory -Force -Path $Parent | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

try {
    foreach ($Directory in @(
        (Join-Path $Repo "scripts"),
        (Join-Path $Repo "docs"),
        (Join-Path $Repo "include\llam"),
        (Join-Path $Repo "examples"),
        $Build
    )) {
        New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    }

    foreach ($Script in @(
        "package_release_windows.ps1",
        "install.sh",
        "install.ps1",
        "verify_windows.ps1"
    )) {
        Copy-Item -LiteralPath (Join-Path $SourceRoot "scripts\$Script") -Destination (Join-Path $Repo "scripts\$Script")
    }

    Write-Utf8File (Join-Path $Repo "LICENSE") $CurrentText
    Write-Utf8File (Join-Path $Repo $ActiveLicense) $CurrentText
    Write-Utf8File (Join-Path $Repo "OLD-LICENSES\Apache-2.0.txt") "historical Apache license`n"
    Write-Utf8File (Join-Path $Repo "README.md") "fixture`n"
    Write-Utf8File (Join-Path $Repo "CHANGELOG.md") "fixture`n"
    Write-Utf8File (Join-Path $Repo "docs\fixture.md") "fixture`n"
    Write-Utf8File (Join-Path $Repo "include\llam\runtime.h") "/* fixture */`n"
    Write-Utf8File (Join-Path $Repo "examples\smoke.c") "/* fixture */`n"
    Write-Utf8File (Join-Path $Repo "examples\smoke.h") "/* fixture */`n"

    foreach ($Artifact in @(
        "llam_runtime.lib",
        "llam_runtime.dll",
        "llam_runtime_shared.lib",
        "bench.exe"
    )) {
        Write-Utf8File (Join-Path $Build $Artifact) "fixture`n"
    }

    & (Join-Path $Repo "scripts\package_release_windows.ps1") `
        -Target "windows-x86_64" `
        -BuildDir "build" `
        -Configuration "Release" `
        -Version "3.0.0" `
        -LibraryVersion "3.0.0" `
        -AbiMajor "2"

    $Archive = Join-Path $Repo "target\dist\llam-3.0.0-windows-x86_64.zip"
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
        throw "Windows package archive was not created"
    }

    $Expanded = Join-Path $TempRoot "expanded"
    Expand-Archive -LiteralPath $Archive -DestinationPath $Expanded -Force
    $PackageRoot = Join-Path $Expanded "llam-3.0.0-windows-x86_64"
    $RootText = [System.IO.File]::ReadAllText((Join-Path $PackageRoot "LICENSE"))
    if ($RootText -cne $CurrentText) {
        throw "root LICENSE content changed in Windows archive"
    }
    $ActivePath = Join-Path $PackageRoot $ActiveLicense
    if (-not (Test-Path -LiteralPath $ActivePath -PathType Leaf)) {
        throw "active LicenseRef text is missing from Windows archive"
    }
    if ([System.IO.File]::ReadAllText($ActivePath) -cne $CurrentText) {
        throw "active LicenseRef content changed in Windows archive"
    }
    if (Test-Path -LiteralPath (Join-Path $PackageRoot "OLD-LICENSES")) {
        throw "historical licenses were packaged as current Windows terms"
    }

    Write-Host "Windows package license layout ok"
} finally {
    Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
}
