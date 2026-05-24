param(
    [string]$Prefix,
    [string]$Version = $env:LLAM_INSTALL_VERSION,
    [string]$Target,
    [string]$BaseUrl,
    [switch]$DryRun,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Prefix)) {
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $Prefix = Join-Path $env:LOCALAPPDATA "LLAM"
    } else {
        $Prefix = Join-Path $HOME ".local\llam"
    }
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = "2.0.0"
}

$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ScriptName = Split-Path -Leaf $MyInvocation.MyCommand.Path

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

function Test-UnsafeArchivePath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $true
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $true
    }
    foreach ($Part in ($Path -split '[\\/]+' | Where-Object { $_ -ne "" })) {
        if ($Part -eq "..") {
            return $true
        }
    }
    return $false
}

function Test-NonCanonicalArchivePath {
    param([string]$Path)

    if (($Path -eq ".") -or
        $Path.StartsWith("./", [System.StringComparison]::Ordinal) -or
        $Path.EndsWith("/.", [System.StringComparison]::Ordinal) -or
        $Path.Contains("/./", [System.StringComparison]::Ordinal) -or
        $Path.Contains("//", [System.StringComparison]::Ordinal)) {
        return $true
    }
    return $false
}

function Test-ReservedWindowsArchivePath {
    param([string]$Path)

    foreach ($Part in ($Path -split '[\\/]+' | Where-Object { $_ -ne "" })) {
        $BaseName = ($Part -split '\.')[0]
        if ($BaseName -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
            return $true
        }
        if ($Part.EndsWith(" ", [System.StringComparison]::Ordinal) -or
            $Part.EndsWith(".", [System.StringComparison]::Ordinal)) {
            return $true
        }
    }
    return $false
}

function Assert-ReleaseComponent {
    param(
        [string]$Name,
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or ($Value -notmatch '^[A-Za-z0-9._+-]+$')) {
        throw "invalid ${Name}: ${Value}; allowed characters are ASCII letters, digits, '.', '_', '+', and '-'"
    }
}

function Assert-NoReparsePoints {
    param([string]$Root)

    if (-not (Test-Path -LiteralPath $Root)) {
        return
    }
    $RootItem = Get-Item -LiteralPath $Root -Force
    if (($RootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "refusing reparse point in LLAM install tree: $($RootItem.FullName)"
    }
    if (-not $RootItem.PSIsContainer) {
        $linkCount = Get-FileLinkCount $RootItem.FullName
        if ($linkCount -gt 1) {
            throw "refusing hard-linked file in LLAM install tree: $($RootItem.FullName)"
        }
        return
    }
    foreach ($Item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "refusing reparse point in LLAM install tree: $($Item.FullName)"
        }
        if (-not $Item.PSIsContainer) {
            $linkCount = Get-FileLinkCount $Item.FullName
            if ($linkCount -gt 1) {
                throw "refusing hard-linked file in LLAM install tree: $($Item.FullName)"
            }
        }
    }
}

function Assert-SafeZipArchive {
    param(
        [string]$ArchivePath,
        [string]$Package
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $Zip = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    # Windows package extraction is case-insensitive on the common target
    # filesystems, so reject payload/PAYLOAD style collisions before expand.
    $Seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($Entry in $Zip.Entries) {
            $Name = $Entry.FullName.Replace("\", "/")
            $CanonicalName = $Name.TrimEnd("/")
            if ([string]::IsNullOrWhiteSpace($CanonicalName)) {
                $CanonicalName = $Name
            }
            if ($Name -notmatch '^[A-Za-z0-9._+/-]+$') {
                throw "refusing archive member with unsafe name characters: $Name"
            }
            if (Test-UnsafeArchivePath $Name) {
                throw "refusing unsafe archive member: $Name"
            }
            if (Test-NonCanonicalArchivePath $Name) {
                throw "refusing non-canonical archive member: $Name"
            }
            if (Test-ReservedWindowsArchivePath $Name) {
                throw "refusing Windows-reserved archive member: $Name"
            }
            if (($Name -ne $Package) -and
                ($Name -ne "$Package/") -and
                (-not $Name.StartsWith("$Package/", [System.StringComparison]::Ordinal))) {
                throw "refusing archive member outside ${Package}: $Name"
            }
            if (-not $Seen.Add($CanonicalName)) {
                throw "refusing duplicate archive member: $Name"
            }
            $Mode = ($Entry.ExternalAttributes -shr 16) -band 0xF000
            if ($Mode -eq 0xA000) {
                throw "refusing symlink archive member: $Name"
            }
            if (($Mode -ne 0) -and ($Mode -ne 0x4000) -and ($Mode -ne 0x8000)) {
                throw "refusing special archive member: $Name"
            }
        }
    } finally {
        $Zip.Dispose()
    }
}

function Assert-ArchiveChecksum {
    param(
        [string]$ArchivePath,
        [string]$ChecksumPath,
        [string]$ArchiveName
    )

    $NonEmptyLines = @(Get-Content -LiteralPath $ChecksumPath | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    })
    if ($NonEmptyLines.Count -ne 1) {
        throw "invalid checksum file for $ArchiveName"
    }
    # Release .sha256 sidecars are exact two-field files, not general
    # SHA256SUMS manifests.  Refuse trailing fields and extra lines.
    $Parts = $NonEmptyLines[0] -split '\s+'
    if ($Parts.Count -ne 2) {
        throw "invalid checksum file for $ArchiveName"
    }
    $Expected = $Parts[0].ToLowerInvariant()
    if ($Expected -notmatch '^[0-9a-f]{64}$') {
        throw "invalid checksum digest for $ArchiveName"
    }
    $ChecksumTarget = $Parts[1].TrimStart("*")
    if ($ChecksumTarget -ne $ArchiveName) {
        throw "checksum target $ChecksumTarget does not match $ArchiveName"
    }
    $Actual = (Get-FileHash $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Actual -ne $Expected) {
        throw "checksum mismatch for $ArchiveName"
    }
}

function Copy-DirectoryIfPresent {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        return
    }
    Assert-SafeDestinationPath $Destination
    if ((Test-Path -LiteralPath $Destination -PathType Container) -and -not $Force) {
        $sourceRoot = (Resolve-Path -LiteralPath $Source).Path.TrimEnd("\", "/")
        foreach ($Item in Get-ChildItem -LiteralPath $Source -Recurse -File) {
            $relative = $Item.FullName.Substring($sourceRoot.Length).TrimStart("\", "/")
            $target = Join-Path $Destination $relative
            if (Test-Path -LiteralPath $target) {
                throw "refusing to overwrite $target; pass -Force"
            }
        }
    }
    $sourceRootForSafety = (Resolve-Path -LiteralPath $Source).Path.TrimEnd("\", "/")
    foreach ($Item in Get-ChildItem -LiteralPath $Source -Recurse -File -Force) {
        $relative = $Item.FullName.Substring($sourceRootForSafety.Length).TrimStart("\", "/")
        Assert-SafeOverwriteTarget (Join-Path $Destination $relative)
    }
    if ($DryRun) {
        Write-Host "copy $Source -> $Destination"
        return
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Assert-SafeDestinationPath $Destination
    foreach ($Item in Get-ChildItem -LiteralPath $Source -Force) {
        Copy-Item -Recurse -Force -LiteralPath $Item.FullName -Destination $Destination
    }
}

function Copy-FileIfPresent {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        return
    }
    Assert-SafeDestinationPath $Destination
    if ((Test-Path -LiteralPath $Destination) -and -not $Force) {
        throw "refusing to overwrite $Destination; pass -Force"
    }
    Assert-SafeOverwriteTarget $Destination
    if ($DryRun) {
        Write-Host "copy $Source -> $Destination"
        return
    }

    $Parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $Parent | Out-Null
    Assert-SafeDestinationPath $Parent
    Assert-SafeDestinationPath $Destination
    Copy-Item -Force -LiteralPath $Source -Destination $Destination
}

function Assert-SafeDestinationPath {
    param([string]$Destination)

    $Probe = $Destination
    while (-not [string]::IsNullOrWhiteSpace($Probe)) {
        $Item = Get-Item -LiteralPath $Probe -Force -ErrorAction SilentlyContinue
        if ($null -ne $Item -and
            (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "refusing reparse point destination in LLAM install prefix: $($Item.FullName)"
        }
        $Parent = Split-Path -Parent $Probe
        if ([string]::IsNullOrWhiteSpace($Parent) -or $Parent -eq $Probe) {
            break
        }
        $Probe = $Parent
    }
}

function Get-FileLinkCount {
    param([string]$Path)

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

function Assert-SafeOverwriteTarget {
    param([string]$Destination)

    $Item = Get-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    if ($null -eq $Item) {
        return
    }
    Assert-SafeDestinationPath $Destination
    if ($Item.PSIsContainer) {
        throw "refusing directory destination where LLAM expects a file: $($Item.FullName)"
    }
    if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        return
    }
    $linkCount = Get-FileLinkCount $Item.FullName
    if ($linkCount -gt 1) {
        throw "refusing hard-linked destination inside LLAM install prefix: $($Item.FullName)"
    }
}

function Get-DefaultTarget {
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ([string]::IsNullOrWhiteSpace($arch)) {
        $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    }
    switch -Regex ($arch) {
        "AMD64|X64|x64" { return "windows-x86_64" }
        default { throw "unsupported architecture for LLAM release installer: $arch" }
    }
}

function Install-ArchiveContentsFrom {
    param([string]$ArchiveSourceDir)

    Assert-NoReparsePoints $ArchiveSourceDir

    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "include") (Join-Path $Prefix "include")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "lib") (Join-Path $Prefix "lib")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "bin") (Join-Path $Prefix "bin")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "share") (Join-Path $Prefix "share")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "docs") (Join-Path $Prefix "share\llam\docs")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "examples") (Join-Path $Prefix "share\llam\examples")
    Copy-DirectoryIfPresent (Join-Path $ArchiveSourceDir "scripts") (Join-Path $Prefix "share\llam\scripts")

    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "README.md") (Join-Path $Prefix "share\llam\README.md")
    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "LICENSE") (Join-Path $Prefix "share\llam\LICENSE")
    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "CHANGELOG.md") (Join-Path $Prefix "share\llam\CHANGELOG.md")
    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "VERSION") (Join-Path $Prefix "share\llam\VERSION")
    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "ABI_MAJOR") (Join-Path $Prefix "share\llam\ABI_MAJOR")
    Copy-FileIfPresent (Join-Path $ArchiveSourceDir "LIBRARY_VERSION") (Join-Path $Prefix "share\llam\LIBRARY_VERSION")

    if (-not $DryRun) {
        Write-Host "installed LLAM into $Prefix"
    }
}

function Install-ArchiveContents {
    Install-ArchiveContentsFrom $SourceDir
}

function Install-Standalone {
    $releaseTag = $Version
    if (-not $releaseTag.StartsWith("v", [System.StringComparison]::Ordinal)) {
        $releaseTag = "v$releaseTag"
    }
    $releaseVersion = $Version -replace '^v', ''
    if ([string]::IsNullOrWhiteSpace($Target)) {
        $Target = Get-DefaultTarget
    }
    Assert-ReleaseComponent "version" $releaseVersion
    Assert-ReleaseComponent "target" $Target
    if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
        $BaseUrl = "https://github.com/Feralthedogg/LLAM/releases/download/$releaseTag"
    }

    $package = "llam-$releaseVersion-$Target"
    $archive = "$package.zip"
    $checksum = "$archive.sha256"

    if ($DryRun) {
        Write-Host "download $BaseUrl/$archive"
        Write-Host "download $BaseUrl/$checksum"
        Write-Host "verify $checksum"
        Write-Host "extract $archive"
        Write-Host "install extracted $package -> $Prefix"
        return
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $archivePath = Join-Path $tmp $archive
        $checksumPath = Join-Path $tmp $checksum

        Invoke-WebRequest "$BaseUrl/$archive" -OutFile $archivePath
        Invoke-WebRequest "$BaseUrl/$checksum" -OutFile $checksumPath

        Assert-ArchiveChecksum $archivePath $checksumPath $archive

        Assert-SafeZipArchive $archivePath $package
        Expand-Archive -LiteralPath $archivePath -DestinationPath $tmp -Force
        $installer = Join-Path (Join-Path $tmp $package) "install.ps1"
        $installerItem = Get-Item -LiteralPath $installer -ErrorAction Stop
        if ($installerItem.PSIsContainer -or
            (($installerItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "archive installer must be a regular file: $installer"
        }
        Assert-NoReparsePoints (Join-Path $tmp $package)
        # Install the extracted tree with this verified script instead of
        # executing installer code supplied by the downloaded archive.
        Install-ArchiveContentsFrom (Join-Path $tmp $package)
    } finally {
        Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
}

if (($ScriptName -eq "install.ps1") -and
    (Test-Path -LiteralPath (Join-Path $SourceDir "install.ps1") -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $SourceDir "include\llam\runtime.h") -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $SourceDir "lib") -PathType Container)) {
    Install-ArchiveContents
} else {
    Install-Standalone
}
