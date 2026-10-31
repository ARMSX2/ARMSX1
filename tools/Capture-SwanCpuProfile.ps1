# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9._:-]+$')][string]$Serial,
    [ValidatePattern('^[A-Za-z][A-Za-z0-9_.]+$')][string]$Package = 'com.nanodata.armsx',
    [ValidateRange(5,60)][int]$Seconds = 20,
    [ValidateSet('cpu-clock:u','cpu-cycles:u')][string]$Event = 'cpu-clock:u',
    [string]$Adb = (Join-Path $env:LOCALAPPDATA 'Android/Sdk/platform-tools/adb.exe'),
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
function Invoke-ProfileAdb([string[]]$Arguments) {
    $taskOutput = @(& $Adb -s $Serial @Arguments 2>&1 | ForEach-Object ToString)
    if ($LASTEXITCODE -ne 0) { throw ($taskOutput -join "`n") }
    return $taskOutput
}
if (-not (Test-Path -LiteralPath $Adb)) { throw 'ADB not found; pass -Adb with its full path.' }
$taskTop = (Invoke-ProfileAdb @('shell','dumpsys','activity','activities') | Select-String 'topResumedActivity=') -join "`n"
if ($taskTop -notmatch ([regex]::Escape($Package) + '/')) { throw 'Leave the target game running in the foreground first.' }
$taskProcess = (Invoke-ProfileAdb @('shell','pidof',$Package)) -join ''
if ($taskProcess -notmatch '^\d+$') { throw 'Expected one running app process.' }
$taskId = [guid]::NewGuid().ToString('N')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $PSScriptRoot) "tmp/cpu-profile-$taskId" }
$taskFolder = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $taskFolder) { throw 'Choose a new output directory to preserve earlier captures.' }
New-Item -ItemType Directory -Path $taskFolder | Out-Null
$taskRemote = "/data/local/tmp/armsx-cpu-$taskId.data"
$taskRecord = Invoke-ProfileAdb @('shell','simpleperf','record','--app',$Package,'-e',$Event,'-f','400','--duration',"$Seconds",'-g','-o',$taskRemote)
$taskRecord | Set-Content -Encoding utf8 (Join-Path $taskFolder 'record.txt')
Invoke-ProfileAdb @('pull',$taskRemote,(Join-Path $taskFolder 'perf.data')) | Out-Null
foreach ($taskReport in @(
    @{Name='functions.txt';Options=@('--sort','comm,dso,symbol','--percent-limit','0.25')},
    @{Name='libraries.txt';Options=@('--sort','comm,dso','--percent-limit','0')},
    @{Name='threads.txt';Options=@('--sort','comm,pid,tid','--percent-limit','0')},
    @{Name='callgraph.txt';Options=@('--children','-g','--sort','comm,dso,symbol','--percent-limit','1')},
    @{Name='functions.csv';Options=@('--csv','--sort','comm,dso,symbol','--percent-limit','0')}
)) {
    Invoke-ProfileAdb (@('shell','simpleperf','report','-i',$taskRemote)+$taskReport.Options) |
        Set-Content -Encoding utf8 (Join-Path $taskFolder $taskReport.Name)
}
$taskTopAfter = (Invoke-ProfileAdb @('shell','dumpsys','activity','activities') | Select-String 'topResumedActivity=') -join "`n"
$taskProcessAfter = (Invoke-ProfileAdb @('shell','pidof',$Package)) -join ''
[ordered]@{
    package=$Package;process_id=$taskProcess;event=$Event;seconds=$Seconds;frequency=400
    same_process=($taskProcess -eq $taskProcessAfter)
    foreground_consistent=($taskTopAfter -match ([regex]::Escape($Package)+'/'))
    recorded_at=(Get-Date).ToString('o');record_output=$taskRecord
    units=if($Event -eq 'cpu-clock:u'){'share of sampled userspace CPU time'}else{'share of sampled userspace CPU cycles'}
    limits=@('Profiling adds overhead; do not substitute it for an unprofiled FPS benchmark.',
        'Unknown symbols can include generated code and stripped driver code; do not label them as a confirmed subsystem.',
        'A hot function is not proof that it causes a difference from another emulator.')
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $taskFolder 'capture.json')
Write-Output "CPU profile saved: $taskFolder"
Write-Output ($taskRecord -join "`n")
