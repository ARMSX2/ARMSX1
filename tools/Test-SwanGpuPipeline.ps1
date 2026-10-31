# SPDX-License-Identifier: GPL-3.0-or-later
# PC-only validation. Does not connect to or modify an Android device.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVc = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
$taskClang = Join-Path $env:ProgramFiles 'LLVM/bin/clang++.exe'
if (-not (Test-Path -LiteralPath $taskVc) -or -not (Test-Path -LiteralPath $taskClang)) {
    throw 'Install LLVM and Visual Studio C++ Build Tools for these Windows host tests.'
}
$taskOutput = Join-Path $taskRoot 'tmp'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
Push-Location -LiteralPath $taskRoot
try {
    # All paths are fixed or independently quoted; no data or credentials enter cmd.
    foreach ($taskTest in @('swan_render_task_test', 'swan_render_queue_test', 'swan_gpu_pipeline_test', 'swan_frame_pacing_test', 'swan_cpu_diagnostics_test', 'swan_cpu_analysis_test')) {
        $taskSources = if ($taskTest -eq 'swan_gpu_pipeline_test') {
            'tests\swan_gpu_pipeline_test.cpp native\swanstation\gpu_pipeline.cpp -Itests\swan_pipeline_fixture'
        } else { 'tests\' + $taskTest + '.cpp' }
        $taskCommand = '"{0}" >nul && "{1}" {2} -std=c++17 -Wall -Wextra -Werror -o tmp\{3}.exe && tmp\{3}.exe' -f $taskVc,$taskClang,$taskSources,$taskTest
        & $env:ComSpec /d /s /c $taskCommand
        if ($LASTEXITCODE -ne 0) { throw "$taskTest failed" }
    }
} finally { Pop-Location }
