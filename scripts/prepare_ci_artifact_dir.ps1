# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$ErrorActionPreference = "Stop"

function Assert-NoReparseComponent {
    param([string]$Candidate)

    $probe = $Candidate
    while (-not [string]::IsNullOrWhiteSpace($probe)) {
        $item = Get-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        if ($null -ne $item) {
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "refusing reparse point artifact path: $($item.FullName)"
            }
            if (-not $item.PSIsContainer -and $probe -ne $Candidate) {
                throw "refusing non-directory artifact parent: $($item.FullName)"
            }
        }

        $parent = Split-Path -Parent $probe
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $probe) {
            break
        }
        $probe = $parent
    }
}

if ([string]::IsNullOrWhiteSpace($Path)) {
    throw "artifact path must not be empty"
}

Assert-NoReparseComponent $Path
# New-Item does not consistently expose -LiteralPath across the PowerShell
# versions installed on hosted Windows images.  The .NET API treats the string
# literally, which preserves the no-wildcard safety property we need here.
[System.IO.Directory]::CreateDirectory($Path) | Out-Null
Assert-NoReparseComponent $Path

$item = Get-Item -LiteralPath $Path -Force
if (-not $item.PSIsContainer) {
    throw "artifact path is not a directory: $($item.FullName)"
}
