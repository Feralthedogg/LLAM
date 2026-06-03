param(
    [string]$Target = "windows-x86_64",
    [string]$BuildDir = "build\release-windows",
    [string]$Configuration = "Release",
    [string]$Version = $env:LLAM_RELEASE_VERSION,
    [string]$LibraryVersion = $env:LLAM_VERSION,
    [string]$AbiMajor = $env:LLAM_ABI_MAJOR
)

$ErrorActionPreference = "Stop"

if (-not ("LlamWin32FileInfo" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class LlamWin32FileInfo {
    public const UInt32 FILE_READ_ATTRIBUTES = 0x80;
    public const UInt32 FILE_SHARE_READ = 0x1;
    public const UInt32 FILE_SHARE_WRITE = 0x2;
    public const UInt32 FILE_SHARE_DELETE = 0x4;
    public const UInt32 OPEN_EXISTING = 3;
    public const UInt32 FILE_ATTRIBUTE_NORMAL = 0x80;
    public static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    public struct FILETIME {
        public UInt32 LowDateTime;
        public UInt32 HighDateTime;
    }

    [StructLayout(LayoutKind.Sequential)]
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

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFileW(
        string fileName,
        UInt32 desiredAccess,
        UInt32 shareMode,
        IntPtr securityAttributes,
        UInt32 creationDisposition,
        UInt32 flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetFileInformationByHandle(
        IntPtr fileHandle,
        out BY_HANDLE_FILE_INFORMATION fileInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);
}
"@
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = if ($env:GITHUB_REF_NAME) { $env:GITHUB_REF_NAME } else { "v2.0.0" }
}
$Version = $Version -replace '^v', ''
if ([string]::IsNullOrWhiteSpace($LibraryVersion)) {
    $LibraryVersion = "2.0.0"
}
if ([string]::IsNullOrWhiteSpace($AbiMajor)) {
    $AbiMajor = "2"
}

function Assert-ReleaseComponent([string]$Name, [string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value) -or ($Value -notmatch '^[A-Za-z0-9._+-]+$')) {
        throw "invalid ${Name}: ${Value}; allowed characters are ASCII letters, digits, '.', '_', '+', and '-'"
    }
}

function Get-FileLinkCount([string]$Path) {
    $Item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($Item.PSIsContainer) {
        return 1
    }
    $shareMode = [LlamWin32FileInfo]::FILE_SHARE_READ -bor
        [LlamWin32FileInfo]::FILE_SHARE_WRITE -bor
        [LlamWin32FileInfo]::FILE_SHARE_DELETE
    $handle = [LlamWin32FileInfo]::CreateFileW(
        $Item.FullName,
        [LlamWin32FileInfo]::FILE_READ_ATTRIBUTES,
        $shareMode,
        [IntPtr]::Zero,
        [LlamWin32FileInfo]::OPEN_EXISTING,
        [LlamWin32FileInfo]::FILE_ATTRIBUTE_NORMAL,
        [IntPtr]::Zero)
    if ($handle -eq [LlamWin32FileInfo]::INVALID_HANDLE_VALUE) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "cannot inspect file link count for ${Path}: Win32 error ${errorCode}"
    }
    try {
        $info = New-Object LlamWin32FileInfo+BY_HANDLE_FILE_INFORMATION
        if (-not [LlamWin32FileInfo]::GetFileInformationByHandle($handle, [ref]$info)) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "cannot inspect file link count for ${Path}: Win32 error ${errorCode}"
        }
        return [uint64]$info.NumberOfLinks
    } finally {
        [void][LlamWin32FileInfo]::CloseHandle($handle)
    }
}

function Assert-SafeOutputPath([string]$Path, [bool]$AllowLeafFile = $true) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "release output path must not be empty"
    }

    # The final archive/hash may be an existing file, but every parent
    # component must be a real directory owned by this workspace path.
    $Probe = $Path
    while (-not [string]::IsNullOrWhiteSpace($Probe)) {
        $Item = Get-Item -LiteralPath $Probe -Force -ErrorAction SilentlyContinue
        if ($null -ne $Item) {
            if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "refusing reparse point release output path: $($Item.FullName)"
            }
            if (-not $Item.PSIsContainer) {
                $linkCount = Get-FileLinkCount $Item.FullName
                if ($linkCount -gt 1) {
                    throw "refusing hard-linked release output path: $($Item.FullName)"
                }
                if (-not $AllowLeafFile -or $Probe -ne $Path) {
                    throw "refusing non-directory release output path component: $($Item.FullName)"
                }
            }
        }

        $Parent = Split-Path -Parent $Probe
        if ([string]::IsNullOrWhiteSpace($Parent) -or $Parent -eq $Probe) {
            break
        }
        $Probe = $Parent
    }
}

function Assert-NoReparsePoints([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $RootItem = Get-Item -LiteralPath $Path -Force
    if (($RootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "refusing reparse point in release path: $($RootItem.FullName)"
    }
    if (-not $RootItem.PSIsContainer) {
        $linkCount = Get-FileLinkCount $RootItem.FullName
        if ($linkCount -gt 1) {
            throw "refusing hard-linked file in release path: $($RootItem.FullName)"
        }
        return
    }
    foreach ($Item in Get-ChildItem -LiteralPath $Path -Recurse -Force) {
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "refusing reparse point in release path: $($Item.FullName)"
        }
        if (-not $Item.PSIsContainer) {
            $linkCount = Get-FileLinkCount $Item.FullName
            if ($linkCount -gt 1) {
                throw "refusing hard-linked file in release path: $($Item.FullName)"
            }
        }
    }
}

Assert-ReleaseComponent "version" $Version
Assert-ReleaseComponent "target" $Target
Assert-ReleaseComponent "library version" $LibraryVersion
Assert-ReleaseComponent "ABI major" $AbiMajor

$Root = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
$BuildRoot = Resolve-Path -LiteralPath (Join-Path $Root $BuildDir)
$ConfigDir = Join-Path $BuildRoot $Configuration
if (-not (Test-Path -LiteralPath $ConfigDir)) {
    $ConfigDir = $BuildRoot
}
Assert-NoReparsePoints $BuildRoot

$OutDir = Join-Path $Root "target\dist"
$PackageName = "llam-$Version-$Target"
$Stage = Join-Path $OutDir $PackageName
$Archive = Join-Path $OutDir "$PackageName.zip"

Assert-SafeOutputPath (Join-Path $Root "target") $false
Assert-SafeOutputPath $OutDir $false
Assert-SafeOutputPath $Stage $false
Assert-SafeOutputPath $Archive
Assert-SafeOutputPath "$Archive.sha256"
# Refuse stale stage trees containing junctions before Remove-Item can recurse.
Assert-NoReparsePoints $Stage
Remove-Item -Recurse -Force -LiteralPath $Stage, $Archive, "$Archive.sha256" -ErrorAction SilentlyContinue
foreach ($Dir in @(
    "bin",
    "docs",
    "examples",
    "include",
    "lib",
    "scripts",
    "share\llam\cmake",
    "lib\pkgconfig"
)) {
    # Hosted Windows PowerShell images do not consistently expose
    # New-Item -LiteralPath.  CreateDirectory keeps the path literal while
    # preserving the wildcard-safe staging invariant.
    [System.IO.Directory]::CreateDirectory((Join-Path $Stage $Dir)) | Out-Null
}

function Require-Input([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "missing release input: $Path"
    }
}

Assert-SafeOutputPath (Join-Path $Root "target") $false
Assert-SafeOutputPath $OutDir $false
Assert-SafeOutputPath $Stage $false
Assert-SafeOutputPath $Archive
Assert-SafeOutputPath "$Archive.sha256"
Assert-NoReparsePoints $Stage

function Find-BuildArtifact([string]$Name, [string]$Fallback) {
    if ($Fallback -and (Test-Path -LiteralPath $Fallback)) {
        return (Resolve-Path -LiteralPath $Fallback).Path
    }

    $Match = Get-ChildItem -LiteralPath $BuildRoot -Recurse -File -Filter $Name |
        Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } |
        Select-Object -First 1
    if (-not $Match) {
        throw "missing build artifact: $Name under $BuildRoot"
    }
    return $Match.FullName
}

$StaticLib = Find-BuildArtifact "llam_runtime.lib" (Join-Path $ConfigDir "llam_runtime.lib")
$SharedDll = Find-BuildArtifact "llam_runtime.dll" (Join-Path $ConfigDir "llam_runtime.dll")
$SharedImportLib = Find-BuildArtifact "llam_runtime_shared.lib" (Join-Path $ConfigDir "llam_runtime_shared.lib")
$BenchExe = Find-BuildArtifact "bench.exe" (Join-Path $ConfigDir "bench.exe")

Require-Input (Join-Path $Root "LICENSE")
Require-Input (Join-Path $Root "README.md")
Require-Input (Join-Path $Root "CHANGELOG.md")
Require-Input (Join-Path $Root "scripts\install.sh")
Require-Input (Join-Path $Root "scripts\install.ps1")
Require-Input (Join-Path $Root "scripts\verify_windows.ps1")
Require-Input (Join-Path $Root "docs")
Require-Input (Join-Path $Root "include\llam")
Require-Input (Join-Path $Root "examples")
Require-Input $StaticLib
Require-Input $SharedDll
Require-Input $SharedImportLib
Require-Input $BenchExe

foreach ($InputPath in @(
    (Join-Path $Root "LICENSE"),
    (Join-Path $Root "README.md"),
    (Join-Path $Root "CHANGELOG.md"),
    (Join-Path $Root "scripts\install.sh"),
    (Join-Path $Root "scripts\install.ps1"),
    (Join-Path $Root "scripts\verify_windows.ps1"),
    (Join-Path $Root "docs"),
    (Join-Path $Root "include\llam"),
    (Join-Path $Root "examples"),
    $StaticLib,
    $SharedDll,
    $SharedImportLib,
    $BenchExe
)) {
    Assert-NoReparsePoints $InputPath
}

Set-Content -LiteralPath (Join-Path $Stage "VERSION") -Value $Version -NoNewline
Set-Content -LiteralPath (Join-Path $Stage "ABI_MAJOR") -Value $AbiMajor -NoNewline
Set-Content -LiteralPath (Join-Path $Stage "LIBRARY_VERSION") -Value $LibraryVersion -NoNewline
Copy-Item -LiteralPath @((Join-Path $Root "LICENSE"), (Join-Path $Root "README.md"), (Join-Path $Root "CHANGELOG.md")) -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "scripts\install.sh") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "scripts\install.ps1") -Destination $Stage
foreach ($Item in Get-ChildItem -LiteralPath (Join-Path $Root "docs") -Force) {
    Copy-Item -Recurse -Force -LiteralPath $Item.FullName -Destination (Join-Path $Stage "docs")
}
Copy-Item -Recurse -LiteralPath (Join-Path $Root "include\llam") -Destination (Join-Path $Stage "include")
foreach ($Item in Get-ChildItem -LiteralPath (Join-Path $Root "examples") -File -Force) {
    if ($Item.Extension -eq ".c" -or $Item.Extension -eq ".h") {
        Copy-Item -LiteralPath $Item.FullName -Destination (Join-Path $Stage "examples")
    }
}
Copy-Item -LiteralPath (Join-Path $Root "scripts\verify_windows.ps1") -Destination (Join-Path $Stage "scripts")
Copy-Item -LiteralPath $BenchExe -Destination (Join-Path $Stage "bin")
Copy-Item -LiteralPath @($StaticLib, $SharedImportLib) -Destination (Join-Path $Stage "lib")
Copy-Item -LiteralPath $SharedDll -Destination (Join-Path $Stage "bin")

$Pc = @'
prefix=${pcfiledir}/../..
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include

Name: LLAM
Description: Stackful user-thread runtime for C applications
Version: __LLAM_VERSION__
Libs: -L${libdir} -lllam_runtime
Libs.private: -lws2_32 -lmswsock -lsynchronization
Cflags: -I${includedir}
'@.Replace("__LLAM_VERSION__", $LibraryVersion)
Set-Content -LiteralPath (Join-Path $Stage "lib\pkgconfig\llam.pc") -Value $Pc

$Config = @'
include(CMakeFindDependencyMacro)

find_dependency(Threads)

get_filename_component(_LLAM_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/llam-targets.cmake")

set(llam_FOUND TRUE)
'@
Set-Content -LiteralPath (Join-Path $Stage "share\llam\cmake\llam-config.cmake") -Value $Config

$Targets = @'
set(_LLAM_PLATFORM_LIBS Threads::Threads ws2_32 mswsock synchronization)

if(NOT TARGET llam::runtime)
    add_library(llam::runtime STATIC IMPORTED)
    set_target_properties(llam::runtime PROPERTIES
        IMPORTED_LOCATION "${_LLAM_PREFIX}/lib/llam_runtime.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

if(NOT TARGET llam::runtime_shared)
    add_library(llam::runtime_shared SHARED IMPORTED)
    set_target_properties(llam::runtime_shared PROPERTIES
        IMPORTED_LOCATION "${_LLAM_PREFIX}/bin/llam_runtime.dll"
        IMPORTED_IMPLIB "${_LLAM_PREFIX}/lib/llam_runtime_shared.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

unset(_LLAM_PLATFORM_LIBS)
'@
Set-Content -LiteralPath (Join-Path $Stage "share\llam\cmake\llam-targets.cmake") -Value $Targets

$VersionConfig = @"
set(PACKAGE_VERSION "$LibraryVersion")

if(PACKAGE_FIND_VERSION_MAJOR STREQUAL "" OR PACKAGE_FIND_VERSION_MAJOR EQUAL $AbiMajor)
    if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    endif()
    if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
"@
Set-Content -LiteralPath (Join-Path $Stage "share\llam\cmake\llam-config-version.cmake") -Value $VersionConfig

Assert-NoReparsePoints $Stage

Assert-SafeOutputPath $Archive
Compress-Archive -LiteralPath $Stage -DestinationPath $Archive -Force
$Hash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-SafeOutputPath "$Archive.sha256"
Set-Content -LiteralPath "$Archive.sha256" -Value "$Hash  $(Split-Path -Leaf $Archive)"
Write-Host $Archive
