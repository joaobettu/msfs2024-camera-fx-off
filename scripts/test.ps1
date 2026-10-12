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
  [string]$GamePath = 'D:\SteamLibrary\steamapps\common\Limitless',
  [switch]$Gpu,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'

if ($Gpu -and -not $Force -and (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue)) { throw 'the sim is running: GPU tests only run with it closed (or pass -Force)' }

$build = Join-Path (Split-Path $PSScriptRoot) 'build'
$pak = @(Get-ChildItem (Join-Path $GamePath 'Shaders') -Filter 'SHADERSPAK.*.SM6.PC.PAK')[0].FullName
$twins = "$build\msfs2024-camera-fx-off.twins"
$adapter = @()
if ($Gpu) { $adapter = @('--adapter=gpu') }
$failed = 0

Write-Host "== classify_test"
& "$build\tests\classify_test.exe" $pak $twins
if ($LASTEXITCODE -ne 0) { $failed++ }

foreach ($run in @(
    @('--sun-flare-off=1', '--bloom-off=1'),
    @('--sun-flare-off=0', '--bloom-off=0'),
    @('--sun-flare-off=1', '--bloom-off=1', '--keep-glow=0', '--keep-star=1', '--keep-ghosts=1'),
    @('--sun-flare-off=1', '--bloom-off=1', "--real-pak=$pak", "--real-twins=$twins"))) {
  $arguments = @($adapter) + @($run)
  Write-Host "== fixture $($arguments -join ' ')"
  & "$build\fixture\fixture.exe" @arguments
  if ($LASTEXITCODE -ne 0) { $failed++ }
}

if ($failed -ne 0) { Write-Host "$failed test run(s) failed"; exit 1 }
Write-Host 'all tests passed'
