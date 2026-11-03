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
# See LICENSE.

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$outputRoot = Join-Path $projectRoot 'build-baseline\layers-final'
$buildRoot = Join-Path $projectRoot 'build-baseline'
$plan = @('A', 'B', 'C', 'D', 'D', 'C', 'B', 'A')

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$env:LLAM_IO_COOP_YIELD = '0'
$env:LLAM_RAW_IOCP_READS_ONLY = '1'
Remove-Item Env:LLAM_BENCH_DUMP -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_WAKE_LATENCY_METRICS -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_RUN_TIMING -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD -ErrorAction SilentlyContinue

for ($index = 0; $index -lt $plan.Count; ++$index) {
    $layer = $plan[$index]
    if ($layer -eq 'A') {
        $executable = Join-Path $buildRoot 'bench_windows_iocp_raw.exe'
        $prefix = '^raw-iocp: round='
    } elseif ($layer -eq 'B') {
        $executable = Join-Path $buildRoot 'bench_windows_iocp_engine.exe'
        $prefix = '^engine-iocp: round='
    } else {
        $executable = Join-Path $buildRoot 'bench_windows_iocp_full.exe'
        $prefix = '^full-llam: round='
    }
    if ($layer -eq 'C') {
        $env:LLAM_BENCH_MINIMAL = '1'
    } else {
        Remove-Item Env:LLAM_BENCH_MINIMAL -ErrorAction SilentlyContinue
    }
    $stdout = Join-Path $outputRoot ("trial-{0:D2}-{1}.out" -f $index, $layer)
    $stderr = Join-Path $outputRoot ("trial-{0:D2}-{1}.err" -f $index, $layer)
    $process = Start-Process -FilePath $executable -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $process.WaitForExit(10000)) {
        Stop-Process -Id $process.Id -Force
        throw "trial $index timed out"
    }
    if ((Get-Item $stderr).Length -ne 0) {
        Get-Content $stderr
        throw "trial $index wrote an error"
    }
    $samples = @(Get-Content $stdout | Where-Object { $_ -match $prefix })
    if ($samples.Count -ne 4) {
        throw "trial $index did not complete four rounds"
    }
    foreach ($sample in $samples) {
        if ($layer -eq 'A' -and
            ($sample -notmatch 'submissions=512' -or
             $sample -notmatch 'completions=512' -or
             $sample -notmatch 'direct_sends=512')) {
            throw "raw IOCP accounting failed in trial $index"
        }
        if ($layer -eq 'B' -and
            $sample -notmatch 'submissions=512 completions=512 direct_sends=512') {
            throw "LLAM engine accounting failed in trial $index"
        }
        if (($layer -eq 'C' -or $layer -eq 'D') -and
            $sample -notmatch 'io_submits=512 io_completions=512') {
            throw "LLAM task accounting failed in trial $index"
        }
        Write-Output ("trial={0} layer={1} {2}" -f $index, $layer, $sample)
    }
}
