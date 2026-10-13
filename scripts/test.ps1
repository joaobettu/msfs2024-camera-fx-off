<#
.SYNOPSIS
Runs the offline tests: the shader recognition against the game's shader package (read-only), and the fixture,
which runs the add-on inside ReShade. The fixture uses the WARP software rasterizer unless -Gpu is given.

.PARAMETER Gpu
Runs the fixture on the graphics card instead. Refuses to while the sim is running, unless -Force is given.

.PARAMETER Force
Allows -Gpu with the sim running (the fixture only draws a few 4x4 targets).
#>
param(
  # The folder that holds FlightSimulator2024.exe; found in the Steam libraries when left out.
  [string]$GamePath = '',
  [switch]$Gpu,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
. (Join-Path $PSScriptRoot 'find-game.ps1')
if (-not $GamePath) { $GamePath = Find-GamePath }

if ($Gpu -and -not $Force -and (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue)) { throw 'the sim is running: GPU tests only run with it closed (or pass -Force)' }

$build = Join-Path (Split-Path $PSScriptRoot) 'build'
$pak = @(Get-ChildItem (Join-Path $GamePath 'Shaders') -Filter 'SHADERSPAK.*.SM6.PC.PAK')[0].FullName
$twins = "$build\msfs2024-camera-fx-off.twins"
# The sim's own dxcompiler.dll, which rebuilds the light glow shaders without star rays (only loaded, never changed).
$dxc = Join-Path $GamePath 'Dxcompiler.DLL'
$adapter = @()
if ($Gpu) { $adapter = @('--adapter=gpu') }
$failed = 0

Write-Host "== classify_test"
& "$build\tests\classify_test.exe" $pak $twins $dxc
if ($LASTEXITCODE -ne 0) { $failed++ }

foreach ($run in @(
    @('--sun-flare-off=1', '--bloom-off=1'),
    @('--sun-flare-off=0', '--bloom-off=0', '--light-star-off=0', '--light-glow-size=50'),
    @('--sun-flare-off=1', '--bloom-off=1', '--keep-glow=0', '--keep-star=1', '--keep-ghosts=1', '--light-glow-size=100'),
    @('--sun-flare-off=1', '--bloom-off=1', '--light-glow-size=25', "--real-pak=$pak", "--real-twins=$twins"))) {
  $arguments = @($adapter) + @($run) + @("--dxcompiler=$dxc")
  Write-Host "== fixture $($arguments -join ' ')"
  & "$build\fixture\fixture.exe" @arguments
  if ($LASTEXITCODE -ne 0) { $failed++ }
}

# The default lookup players rely on: the shader package in the Shaders folder next to the executable, no pak_path in
# the settings, from a folder whose path has spaces and accents ("ção", written as code points: this file has no BOM).
$elsewhere = Join-Path $build "fixture elsewhere $([char]0x00E7)$([char]0x00E3)o"
New-Item -ItemType Directory -Force $elsewhere | Out-Null
foreach ($file in 'fixture.exe', 'dxgi.dll', 'ReShade.ini', 'msfs2024-camera-fx-off.addon64') {
  Copy-Item -LiteralPath (Join-Path "$build\fixture" $file) -Destination $elsewhere -Force
}
$arguments = @($adapter) + @('--sun-flare-off=1', '--bloom-off=1', '--keep-glow=1', '--light-glow-size=75', '--pak-next-to-exe', "--dxcompiler=$dxc")
Write-Host "== fixture (in '$elsewhere') $($arguments -join ' ')"
& (Join-Path $elsewhere 'fixture.exe') @arguments
if ($LASTEXITCODE -ne 0) { $failed++ }

if ($failed -ne 0) { Write-Host "$failed test run(s) failed"; exit 1 }
Write-Host 'all tests passed'
