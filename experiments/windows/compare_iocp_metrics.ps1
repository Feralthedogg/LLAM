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
$outputRoot = Join-Path $projectRoot 'build-baseline\comparison'
$baselineExe = Join-Path $projectRoot 'build-baseline\bench_windows_iocp_full.exe'
$candidateExe = Join-Path $projectRoot 'build-atomic-metrics\bench_windows_iocp_full.exe'
$plan = @('baseline', 'baseline', 'candidate', 'baseline',
          'candidate', 'candidate', 'baseline', 'candidate')

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$env:LLAM_IO_COOP_YIELD = '0'
Remove-Item Env:LLAM_BENCH_MINIMAL -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_BENCH_DUMP -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_WAKE_LATENCY_METRICS -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_RUN_TIMING -ErrorAction SilentlyContinue
Remove-Item Env:LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD -ErrorAction SilentlyContinue

for ($index = 0; $index -lt $plan.Count; ++$index) {
    $name = $plan[$index]
    $executable = if ($name -eq 'baseline') { $baselineExe } else { $candidateExe }
    $stdout = Join-Path $outputRoot ("trial-{0:D2}-{1}.out" -f $index, $name)
    $stderr = Join-Path $outputRoot ("trial-{0:D2}-{1}.err" -f $index, $name)
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
    $samples = @(Get-Content $stdout | Where-Object { $_ -match '^full-llam: round=' })
    if ($samples.Count -ne 4 -or
        @($samples | Where-Object { $_ -notmatch 'io_submits=512 io_completions=512' }).Count -ne 0) {
        throw "trial $index did not complete four exact-accounting rounds"
    }
    foreach ($sample in $samples) {
        Write-Output ("trial={0} build={1} {2}" -f $index, $name, $sample)
    }
}
