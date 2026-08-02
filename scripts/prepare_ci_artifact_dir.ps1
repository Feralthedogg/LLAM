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
# See LICENSES/OLD-LICENSE/Apache-2.0.txt.

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
