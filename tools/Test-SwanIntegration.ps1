# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$Apk)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskApi = Get-Content -LiteralPath (Join-Path $taskRoot 'android/app/src/main/java/kr/co/iefriends/pcsx2/NativeApp.java') -Raw
$taskJni = Get-Content -LiteralPath (Join-Path $taskRoot 'native/swanstation/jni.cpp') -Raw
$taskMethods = [regex]::Matches($taskApi, '(?m)^\s*(?:public|private|protected)\s+static\s+native\s+\w+(?:\[\])?\s+(\w+)\s*\(([^;]*)\)\s*;')
foreach ($taskMethod in $taskMethods) {
    $taskName = $taskMethod.Groups[1].Value
    $taskBinding = [regex]::Match($taskJni, ('NativeApp_' + $taskName + '\(([^)]*)\)'))
    if (-not $taskBinding.Success) { throw "Missing SwanStation binding: $taskName" }
    $taskJavaArgs = $taskMethod.Groups[2].Value.Trim()
    $taskJavaCount = if ($taskJavaArgs) { $taskJavaArgs.Split(',').Count } else { 0 }
    $taskNativeCount = $taskBinding.Groups[1].Value.Split(',').Count - 2
    if ($taskJavaCount -ne $taskNativeCount) { throw "JNI argument-count mismatch: $taskName" }
}
$taskCorePin = & git -C (Join-Path $taskRoot 'core/swanstation') rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskCorePin -ne 'dd4805619bbbfc6e0d1776372cb16377088b448e') { throw 'Wrong core revision' }
Write-Output "$($taskMethods.Count) JNI contracts and pinned core revision checked."
$taskAnglePin = & git -C (Join-Path $taskRoot 'third_party/angle-prebuilt') rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskAnglePin -ne '9a0834cc8a5f3427dae2f6e87de4dfae50cb781c') { throw 'Wrong ANGLE revision' }
if ($Apk) {
    $taskApkPath = [IO.Path]::GetFullPath($Apk)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $taskZip = [IO.Compression.ZipFile]::OpenRead($taskApkPath)
    try {
        $taskLibraries = @($taskZip.Entries | Where-Object { $_.FullName -like 'lib/*/*.so' } | ForEach-Object FullName)
        foreach ($taskRequired in @('lib/arm64-v8a/libemucorepsx.so', 'lib/arm64-v8a/libswanstation.so','lib/arm64-v8a/libEGL_angle.so','lib/arm64-v8a/libGLESv2_angle.so','lib/arm64-v8a/liblibrashader_capi.so','lib/arm64-v8a/libc++_shared.so')) {
            if ($taskLibraries -notcontains $taskRequired) { throw "Missing engine library: $taskRequired" }
        }
        foreach ($taskName in @('libEGL_angle.so','libGLESv2_angle.so')) {
            $taskEntry = $taskZip.GetEntry("lib/arm64-v8a/$taskName")
            $taskStream = $taskEntry.Open()
            $taskSha = [Security.Cryptography.SHA256]::Create()
            try { $taskHash = ([BitConverter]::ToString($taskSha.ComputeHash($taskStream))).Replace('-','') }
            finally { $taskStream.Dispose(); $taskSha.Dispose() }
            $taskExpected = (Get-FileHash -LiteralPath (Join-Path $taskRoot "third_party/angle-prebuilt/lib/arm64/$taskName") -Algorithm SHA256).Hash
            if ($taskHash -ne $taskExpected) { throw "APK ANGLE differs from pinned input: $taskName" }
        }
        foreach ($taskLibrary in $taskLibraries) {
            if ($taskLibrary -match 'lib(?:armsx|SDL2|duckstation)\.so$') { throw "Old/private core library packaged: $taskLibrary" }
        }
        if ($taskZip.Entries | Where-Object FullName -Match '(?i)(^|/)(discord_creds\.env|.*\.(?:jks|keystore|chd))$') {
            throw 'Unexpected credentials/key/game fixture packaged'
        }
        $taskLibraries | Write-Output
        Write-Output 'APK engine boundary checked; this is not a distribution-licence audit.'
    } finally { $taskZip.Dispose() }
}
