param(
    [string]$ExePath = (Join-Path $PSScriptRoot "bin\armsx.exe"),
    [switch]$Unregister
)

$registryRoot = "HKCU:\Software\Classes\armsx"

if ($Unregister) {
    if (Test-Path $registryRoot) {
        Remove-Item -Path $registryRoot -Recurse -Force
        Write-Host "Removed ARMSX protocol registration from HKCU."
    } else {
        Write-Host "ARMSX protocol registration was not present."
    }
    exit 0
}

$resolvedExePath = (Resolve-Path $ExePath).Path
if (-not $resolvedExePath) {
    throw "ARMSX executable not found at $ExePath"
}

$command = '"' + $resolvedExePath + '" "%1"'

New-Item -Path $registryRoot -Force | Out-Null
Set-Item -Path $registryRoot -Value "URL:ARMSX Protocol"
New-ItemProperty -Path $registryRoot -Name "URL Protocol" -PropertyType String -Value "" -Force | Out-Null

New-Item -Path "$registryRoot\DefaultIcon" -Force | Out-Null
Set-Item -Path "$registryRoot\DefaultIcon" -Value ('"' + $resolvedExePath + '",0')

New-Item -Path "$registryRoot\shell\open\command" -Force | Out-Null
Set-Item -Path "$registryRoot\shell\open\command" -Value $command

Write-Host "Registered armsx:// for $resolvedExePath in HKCU."
