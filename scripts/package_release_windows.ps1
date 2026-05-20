param(
    [string]$Target = "windows-x86_64",
    [string]$BuildDir = "build\release-windows",
    [string]$Configuration = "Release",
    [string]$Version = $env:LLAM_RELEASE_VERSION,
    [string]$LibraryVersion = $env:LLAM_VERSION,
    [string]$AbiMajor = $env:LLAM_ABI_MAJOR
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = if ($env:GITHUB_REF_NAME) { $env:GITHUB_REF_NAME } else { "v1.1.0" }
}
$Version = $Version.TrimStart("v").Replace("/", "-")
if ([string]::IsNullOrWhiteSpace($LibraryVersion)) {
    $LibraryVersion = "1.1.0"
}
if ([string]::IsNullOrWhiteSpace($AbiMajor)) {
    $AbiMajor = "1"
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildRoot = Resolve-Path (Join-Path $Root $BuildDir)
$ConfigDir = Join-Path $BuildRoot $Configuration
if (-not (Test-Path $ConfigDir)) {
    $ConfigDir = $BuildRoot
}

$OutDir = Join-Path $Root "target\dist"
$PackageName = "llam-$Version-$Target"
$Stage = Join-Path $OutDir $PackageName
$Archive = Join-Path $OutDir "$PackageName.zip"

Remove-Item -Recurse -Force $Stage, $Archive, "$Archive.sha256" -ErrorAction SilentlyContinue
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
    New-Item -ItemType Directory -Force -Path (Join-Path $Stage $Dir) | Out-Null
}

function Require-Input([string]$Path) {
    if (-not (Test-Path $Path)) {
        throw "missing release input: $Path"
    }
}

function Find-BuildArtifact([string]$Name, [string]$Fallback) {
    if ($Fallback -and (Test-Path $Fallback)) {
        return (Resolve-Path $Fallback).Path
    }

    $Match = Get-ChildItem -Path $BuildRoot -Recurse -File -Filter $Name |
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
Require-Input (Join-Path $Root "scripts\install.ps1")
Require-Input (Join-Path $Root "docs")
Require-Input (Join-Path $Root "include\llam")
Require-Input (Join-Path $Root "examples")
Require-Input $StaticLib
Require-Input $SharedDll
Require-Input $SharedImportLib
Require-Input $BenchExe

Set-Content -Path (Join-Path $Stage "VERSION") -Value $Version -NoNewline
Copy-Item -Path @((Join-Path $Root "LICENSE"), (Join-Path $Root "README.md"), (Join-Path $Root "CHANGELOG.md")) -Destination $Stage
Copy-Item -Path (Join-Path $Root "scripts\install.ps1") -Destination $Stage
Copy-Item -Recurse -Path (Join-Path $Root "docs\*") -Destination (Join-Path $Stage "docs")
Copy-Item -Recurse -Path (Join-Path $Root "include\llam") -Destination (Join-Path $Stage "include")
Copy-Item -Path @((Join-Path $Root "examples\*.c"), (Join-Path $Root "examples\*.h")) -Destination (Join-Path $Stage "examples")
Copy-Item -Path (Join-Path $Root "scripts\verify_windows.ps1") -Destination (Join-Path $Stage "scripts")
Copy-Item -Path $BenchExe -Destination (Join-Path $Stage "bin")
Copy-Item -Path @($StaticLib, $SharedImportLib) -Destination (Join-Path $Stage "lib")
Copy-Item -Path $SharedDll -Destination (Join-Path $Stage "bin")

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
Set-Content -Path (Join-Path $Stage "lib\pkgconfig\llam.pc") -Value $Pc

$Config = @'
include(CMakeFindDependencyMacro)

find_dependency(Threads)

get_filename_component(_LLAM_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/llam-targets.cmake")

set(llam_FOUND TRUE)
'@
Set-Content -Path (Join-Path $Stage "share\llam\cmake\llam-config.cmake") -Value $Config

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
Set-Content -Path (Join-Path $Stage "share\llam\cmake\llam-targets.cmake") -Value $Targets

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
Set-Content -Path (Join-Path $Stage "share\llam\cmake\llam-config-version.cmake") -Value $VersionConfig

Compress-Archive -Path $Stage -DestinationPath $Archive -Force
$Hash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
Set-Content -Path "$Archive.sha256" -Value "$Hash  $(Split-Path -Leaf $Archive)"
Write-Host $Archive
