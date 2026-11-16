param([Parameter(Mandatory=$true)][string]$Capture, [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$inputFile = (Resolve-Path -LiteralPath $Capture).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$classes = Join-Path $env:TEMP 'armsx-gpu-replay-classes'
[IO.Directory]::CreateDirectory($classes) | Out-Null
if ([IO.Path]::GetExtension($inputFile) -ne '.armgfx') {
    $attachment = Join-Path $output 'graphics-replay.armgfx'
    & python (Join-Path $PSScriptRoot 'extract_report.py') $inputFile $attachment
    if ($LASTEXITCODE -ne 0) { throw 'Graphics extraction failed' }
    $inputFile = $attachment
}
$javac = if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME 'bin/javac.exe' } else { (Get-Command javac).Source }
$java = if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME 'bin/java.exe' } else { (Get-Command java).Source }
$core = Join-Path $repo 'psxlab/app/src/main/java/dev/psxlab/core'
& $javac -d $classes (Join-Path $core 'GpuReplay.java') (Join-Path $core 'SoftwareRasterizer.java') (Join-Path $core 'NativeRasterizer.java') (Join-Path $PSScriptRoot 'ReplayGpu.java')
if ($LASTEXITCODE -ne 0) { throw 'Replay tool compilation failed' }
& $java '-Djava.awt.headless=true' -cp $classes dev.psxlab.core.ReplayGpu $inputFile $output
if ($LASTEXITCODE -ne 0) { throw 'Graphics replay failed' }
