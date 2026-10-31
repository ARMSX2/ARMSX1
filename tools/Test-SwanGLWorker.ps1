# SPDX-License-Identifier: GPL-3.0-or-later
# Source coverage guard: every GL call made outside the worker must be routed.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskProxy = Get-Content -LiteralPath (Join-Path $taskRoot 'native/swanstation/gl_worker.cpp') -Raw
$taskRegistered = @([regex]::Matches($taskProxy, 'MATCH\((gl[A-Z]\w*)\)') | ForEach-Object { $_.Groups[1].Value })
$taskSources = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'core/swanstation/src/common/gl') -Recurse -File |
    Where-Object Extension -in @('.h','.cpp'))
foreach ($taskRelative in @('core/swanstation/src/core/gpu_hw_opengl.cpp','core/swanstation/src/core/gpu_hw_opengl.h',
    'core/swanstation/src/core/host_display.cpp','native/swanstation/runtime.cpp','native/swanstation/shader_gl.cpp')) {
    $taskSources += Get-Item -LiteralPath (Join-Path $taskRoot $taskRelative)
}
foreach ($taskSource in $taskSources) {
    $taskText = Get-Content -LiteralPath $taskSource.FullName -Raw
    foreach ($taskMatch in [regex]::Matches($taskText, '\b(gl[A-Z]\w*)\s*\(')) {
        $taskName = $taskMatch.Groups[1].Value
        if ($taskRegistered -notcontains $taskName) { throw "Unrouted GL call: $taskName in $($taskSource.Name)" }
    }
}
$taskApi = Get-Content -LiteralPath (Join-Path $taskRoot 'native/swanstation/gl_api.h') -Raw
$taskApiNames = [regex]::Matches($taskApi, 'X\((gl[A-Z]\w*)\)')
foreach ($taskMatch in $taskApiNames) {
    if ($taskRegistered -notcontains $taskMatch.Groups[1].Value) { throw "Unrouted host GL entry: $($taskMatch.Value)" }
}
if ($taskRegistered -notcontains 'glGetStringi') { throw 'GLAD extension discovery needs a routed glGetStringi' }
foreach ($taskRelative in @('native/swanstation/runtime.cpp','native/swanstation/shader_gl.cpp')) {
    $taskText = Get-Content -LiteralPath (Join-Path $taskRoot $taskRelative) -Raw
    foreach ($taskMatch in [regex]::Matches($taskText, '\b(gl[A-Z]\w*)\s*\(')) {
        $taskName = $taskMatch.Groups[1].Value
        if ($taskApi -notmatch ('#define ' + [regex]::Escape($taskName) + '\s')) {
            throw "Host GL call bypasses the selected driver/worker: $taskName"
        }
    }
}
Write-Output "$($taskRegistered.Count) GL entry points cover the pinned renderer and host; unknown probes return null."

# A public core-option definition alone does not prove the core reads it.
$taskSettingsSource = Get-Content -LiteralPath (Join-Path $taskRoot 'core/swanstation/src/core/settings.cpp') -Raw
$taskConsumed = @([regex]::Matches($taskSettingsSource, 'Get(?:Bool|String|Int|Float)Value\(\s*"([^"]+)"\s*,\s*"([^"]+)"') |
    ForEach-Object { 'swanstation_' + $_.Groups[1].Value + '_' + $_.Groups[2].Value })
$taskOptions = Get-Content -LiteralPath (Join-Path $taskRoot 'native/swanstation/performance_options.h') -Raw
$taskWired = @([regex]::Matches($taskOptions, '"(swanstation_[^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
foreach ($taskKey in $taskWired) {
    if ($taskConsumed -notcontains $taskKey) { throw "Performance option has no pinned-core consumer: $taskKey" }
}
Write-Output "$($taskWired.Count) performance options have real pinned-core settings consumers."
