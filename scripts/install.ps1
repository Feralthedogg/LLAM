param(
    [string]$Prefix,
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

Copy-DirectoryIfPresent (Join-Path $SourceDir "include") (Join-Path $Prefix "include")
Copy-DirectoryIfPresent (Join-Path $SourceDir "lib") (Join-Path $Prefix "lib")
Copy-DirectoryIfPresent (Join-Path $SourceDir "bin") (Join-Path $Prefix "bin")
Copy-DirectoryIfPresent (Join-Path $SourceDir "share") (Join-Path $Prefix "share")
Copy-DirectoryIfPresent (Join-Path $SourceDir "docs") (Join-Path $Prefix "share\llam\docs")
Copy-DirectoryIfPresent (Join-Path $SourceDir "examples") (Join-Path $Prefix "share\llam\examples")
Copy-DirectoryIfPresent (Join-Path $SourceDir "scripts") (Join-Path $Prefix "share\llam\scripts")

Copy-FileIfPresent (Join-Path $SourceDir "README.md") (Join-Path $Prefix "share\llam\README.md")
Copy-FileIfPresent (Join-Path $SourceDir "LICENSE") (Join-Path $Prefix "share\llam\LICENSE")
Copy-FileIfPresent (Join-Path $SourceDir "VERSION") (Join-Path $Prefix "share\llam\VERSION")

if (-not $DryRun) {
    Write-Host "installed LLAM into $Prefix"
}
