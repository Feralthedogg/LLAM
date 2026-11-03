# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.

$ErrorActionPreference = "Stop"

$SourceRoot = Split-Path -Parent $PSScriptRoot
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("llam-license-layout-" + [guid]::NewGuid().ToString("N"))
$Repo = Join-Path $TempRoot "repo"
$Build = Join-Path $Repo "build\Release"
$Notice = "NOTICE"
$ApacheLicense = "LICENSES\Apache-2.0.txt"
$ThirdPartyNotices = "THIRD_PARTY_NOTICES.md"
$ThirdPartyMagic2License = "LICENSES\THIRD_PARTY\magic2\LICENSE"
$NoticeText = "LLAM attribution`n"
$ApacheText = "Apache license`n"
$ThirdPartyNoticesText = "third-party attributions`n"
$ThirdPartyMagic2Text = "MIT license`n"

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

    Write-Utf8File (Join-Path $Repo "LICENSE") $ApacheText
    Write-Utf8File (Join-Path $Repo $Notice) $NoticeText
    Write-Utf8File (Join-Path $Repo $ApacheLicense) $ApacheText
    Write-Utf8File (Join-Path $Repo $ThirdPartyNotices) $ThirdPartyNoticesText
    Write-Utf8File (Join-Path $Repo "third_party\magic2\LICENSE") $ThirdPartyMagic2Text
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
    if ($RootText -cne $ApacheText) {
        throw "root LICENSE content changed in Windows archive"
    }
    $NoticePath = Join-Path $PackageRoot $Notice
    if (-not (Test-Path -LiteralPath $NoticePath -PathType Leaf)) {
        throw "NOTICE attribution is missing from Windows archive"
    }
    if ([System.IO.File]::ReadAllText($NoticePath) -cne $NoticeText) {
        throw "NOTICE attribution changed in Windows archive"
    }
    $ApachePath = Join-Path $PackageRoot $ApacheLicense
    if (-not (Test-Path -LiteralPath $ApachePath -PathType Leaf)) {
        throw "Apache 2.0 text is missing from Windows archive"
    }
    if ([System.IO.File]::ReadAllText($ApachePath) -cne $ApacheText) {
        throw "Apache 2.0 text changed in Windows archive"
    }
    $AttributionsPath = Join-Path $PackageRoot $ThirdPartyNotices
    if (-not (Test-Path -LiteralPath $AttributionsPath -PathType Leaf)) {
        throw "third-party attributions are missing from Windows archive"
    }
    if ([System.IO.File]::ReadAllText($AttributionsPath) -cne $ThirdPartyNoticesText) {
        throw "third-party attributions changed in Windows archive"
    }
    $ThirdPartyMagic2Path = Join-Path $PackageRoot $ThirdPartyMagic2License
    if (-not (Test-Path -LiteralPath $ThirdPartyMagic2Path -PathType Leaf)) {
        throw "magic2 MIT license is missing from Windows archive"
    }
    if ([System.IO.File]::ReadAllText($ThirdPartyMagic2Path) -cne $ThirdPartyMagic2Text) {
        throw "magic2 MIT license changed in Windows archive"
    }
    if (Test-Path -LiteralPath (Join-Path $PackageRoot "OLD-LICENSES")) {
        throw "obsolete top-level OLD-LICENSES directory was packaged"
    }
    if ((Test-Path -LiteralPath (Join-Path $PackageRoot "LICENSES\OLD-LICENSE")) -or
        (Get-ChildItem -LiteralPath (Join-Path $PackageRoot "LICENSES") -Recurse -Filter "LicenseRef-LLAM*")) {
        throw "retired LLAM license metadata was packaged"
    }

    $VersionCheck = @'
set(PACKAGE_FIND_VERSION "3.0.0")
set(PACKAGE_FIND_VERSION_MAJOR 3)
include("__VERSION_FILE__")
if(NOT PACKAGE_VERSION_COMPATIBLE OR NOT PACKAGE_VERSION_EXACT)
    message(FATAL_ERROR "SDK 3.0.0 must accept exact package version 3.0.0 independently of ABI 2")
endif()
unset(PACKAGE_VERSION_COMPATIBLE)
unset(PACKAGE_VERSION_EXACT)
set(PACKAGE_FIND_VERSION "2")
set(PACKAGE_FIND_VERSION_MAJOR 2)
include("__VERSION_FILE__")
if(PACKAGE_VERSION_COMPATIBLE OR PACKAGE_VERSION_EXACT)
    message(FATAL_ERROR "SDK 3.0.0 must reject package major 2")
endif()
'@
    $VersionFile = (Join-Path $PackageRoot "share\llam\cmake\llam-config-version.cmake").Replace('\', '/')
    $VersionScript = Join-Path $TempRoot "version-check.cmake"
    Write-Utf8File $VersionScript ($VersionCheck.Replace("__VERSION_FILE__", $VersionFile))
    & cmake -P $VersionScript
    if ($LASTEXITCODE -ne 0) {
        throw "Windows SDK package version check failed"
    }

    Write-Host "Windows package license layout and SDK version checks ok"
} finally {
    Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
}
