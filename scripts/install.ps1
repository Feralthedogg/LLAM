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
    $Version = "1.0.2"
}

$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Copy-DirectoryIfPresent {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        return
    }
    if ($DryRun) {
        Write-Host "copy $Source -> $Destination"
        return
    }
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

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -Recurse -Force -Path (Join-Path $Source "*") -Destination $Destination
}

function Copy-FileIfPresent {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        return
    }
    if ((Test-Path -LiteralPath $Destination) -and -not $Force) {
        throw "refusing to overwrite $Destination; pass -Force"
    }
    if ($DryRun) {
        Write-Host "copy $Source -> $Destination"
        return
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -Force -Path $Source -Destination $Destination
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

function Install-ArchiveContents {
    Copy-DirectoryIfPresent (Join-Path $SourceDir "include") (Join-Path $Prefix "include")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "lib") (Join-Path $Prefix "lib")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "bin") (Join-Path $Prefix "bin")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "share") (Join-Path $Prefix "share")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "docs") (Join-Path $Prefix "share\llam\docs")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "examples") (Join-Path $Prefix "share\llam\examples")
    Copy-DirectoryIfPresent (Join-Path $SourceDir "scripts") (Join-Path $Prefix "share\llam\scripts")

    Copy-FileIfPresent (Join-Path $SourceDir "README.md") (Join-Path $Prefix "share\llam\README.md")
    Copy-FileIfPresent (Join-Path $SourceDir "LICENSE") (Join-Path $Prefix "share\llam\LICENSE")
    Copy-FileIfPresent (Join-Path $SourceDir "CHANGELOG.md") (Join-Path $Prefix "share\llam\CHANGELOG.md")
    Copy-FileIfPresent (Join-Path $SourceDir "VERSION") (Join-Path $Prefix "share\llam\VERSION")

    if (-not $DryRun) {
        Write-Host "installed LLAM into $Prefix"
    }
}

function Install-Standalone {
    $releaseTag = $Version
    if (-not $releaseTag.StartsWith("v")) {
        $releaseTag = "v$releaseTag"
    }
    $releaseVersion = $Version.TrimStart("v")
    if ([string]::IsNullOrWhiteSpace($Target)) {
        $Target = Get-DefaultTarget
    }
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
        Write-Host "run $package\install.ps1 -Prefix $Prefix"
        return
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        $archivePath = Join-Path $tmp $archive
        $checksumPath = Join-Path $tmp $checksum

        Invoke-WebRequest "$BaseUrl/$archive" -OutFile $archivePath
        Invoke-WebRequest "$BaseUrl/$checksum" -OutFile $checksumPath

        $expected = (Get-Content $checksumPath).Split(" ")[0].ToLowerInvariant()
        $actual = (Get-FileHash $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            throw "checksum mismatch for $archive"
        }

        Expand-Archive $archivePath -DestinationPath $tmp -Force
        $installer = Join-Path (Join-Path $tmp $package) "install.ps1"
        if ($Force) {
            & $installer -Prefix $Prefix -Force
        } else {
            & $installer -Prefix $Prefix
        }
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

if ((Test-Path -LiteralPath (Join-Path $SourceDir "include\llam\runtime.h") -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $SourceDir "lib") -PathType Container)) {
    Install-ArchiveContents
} else {
    Install-Standalone
}
