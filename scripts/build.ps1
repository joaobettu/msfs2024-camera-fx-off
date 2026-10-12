<#
.SYNOPSIS
Builds build\msfs2024-camera-fx-off.addon64 and, with -Tests, the offline tests (build\tests and build\fixture).

.DESCRIPTION
Needs Visual Studio 2022 Build Tools (C++ x64) and the Windows 11 SDK (dxc, rc).
Runs at below-normal priority, which the compiler processes inherit, so a running sim keeps the CPU.
#>
param(
  [switch]$Tests,
  # ReShade with full add-on support, copied next to the fixture (only read). By default, the dxgi.dll in the game folder,
  # which is found in the Steam libraries unless -GamePath gives it.
  [string]$ReShadeDll = '',
  [string]$GamePath = ''
)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
. (Join-Path $PSScriptRoot 'find-game.ps1')

$root = Split-Path $PSScriptRoot
$build = Join-Path $root 'build'
$obj = Join-Path $build 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x64 build tools not found' }
$env:PATH = "$(Split-Path $vswhere);$env:PATH" # vcvars64.bat calls vswhere itself
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }

function Invoke-Tool([string]$exe, [string[]]$arguments) {
  & $exe @arguments
  if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

$common = @('/nologo', '/std:c++20', '/O2', '/MT', '/EHsc', '/W4', '/permissive-', '/utf-8', '/Zi', '/DNDEBUG', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX')

# Add-on
Invoke-Tool 'rc' @('/nologo', '/fo', "$obj\version.res", "$root\src\version.rc")
Invoke-Tool 'cl' ($common + @('/DImTextureID=ImU64', "/I$root\third_party\reshade\include", "/I$root\third_party\imgui",
    "/Fo$obj\addon.obj", "/Fd$obj\addon_cl.pdb", "$root\src\addon.cpp",
    '/link', '/DLL', "/OUT:$build\msfs2024-camera-fx-off.addon64", "/PDB:$build\msfs2024-camera-fx-off.pdb", '/PDBALTPATH:%_PDB%', "$obj\version.res"))
Write-Host "built $build\msfs2024-camera-fx-off.addon64"

if (-not $Tests) { return }

# Stand-in shaders for the fixture, compiled and signed by the Windows SDK dxc.
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory | Where-Object { Test-Path "$($_.FullName)\x64\dxc.exe" } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw 'dxc.exe from the Windows SDK not found' }
$dxc = "$($sdk.FullName)\x64\dxc.exe"
$gen = Join-Path $build 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
$shaders = @(
  @{ Name = 'vs_flare'; Profile = 'vs_6_0'; Entry = 'vh_billboard_lights'; Defines = @('-D', 'WITH_FLARE_UV') },
  @{ Name = 'vs_light'; Profile = 'vs_6_0'; Entry = 'vh_billboard_lights'; Defines = @() },
  @{ Name = 'ps_flare'; Profile = 'ps_6_0'; Entry = 'ph_billboard_lights'; Defines = @('-D', 'WITH_FLARE_UV') },
  @{ Name = 'ps_light'; Profile = 'ps_6_0'; Entry = 'ph_billboard_lights'; Defines = @() },
  @{ Name = 'vs_fullscreen'; Profile = 'vs_6_0'; Entry = 'vh_fullscreen'; Defines = @() },
  @{ Name = 'vs_fullscreen_tex'; Profile = 'vs_6_0'; Entry = 'vh_fullscreen_tex'; Defines = @() },
  @{ Name = 'vs_material'; Profile = 'vs_6_0'; Entry = 'vh_base'; Defines = @() },
  @{ Name = 'ps_material'; Profile = 'ps_6_0'; Entry = 'ph_base'; Defines = @() },
  @{ Name = 'ps_material_b'; Profile = 'ps_6_0'; Entry = 'ph_base'; Defines = @('-D', 'ALT') },
  @{ Name = 'ps_material_c'; Profile = 'ps_6_0'; Entry = 'ph_base'; Defines = @('-D', 'ALT2') },
  @{ Name = 'ps_tonemap_bloom'; Profile = 'ps_6_0'; Entry = 'ph_tonemapping'; Defines = @('-D', 'WITH_BLOOM') },
  @{ Name = 'ps_tonemap_plain'; Profile = 'ps_6_0'; Entry = 'ph_tonemapping'; Defines = @() })
foreach ($s in $shaders) {
  Invoke-Tool $dxc (@('-nologo', '-T', $s.Profile, '-E', $s.Entry) + $s.Defines + @('-Fh', "$gen\$($s.Name).h", '-Vn', "g_$($s.Name)", "$root\tests\fixture_shaders.hlsl"))
}

$testsDir = Join-Path $build 'tests'
$fixtureDir = Join-Path $build 'fixture'
New-Item -ItemType Directory -Force $testsDir, $fixtureDir | Out-Null
Invoke-Tool 'cl' ($common + @("/Fo$obj\classify_test.obj", "/Fd$obj\classify_test_cl.pdb", "$root\tests\classify_test.cpp", "/Fe$testsDir\classify_test.exe"))
Invoke-Tool 'cl' ($common + @("/I$gen", "/Fo$obj\fixture.obj", "/Fd$obj\fixture_cl.pdb", "$root\tests\fixture.cpp", "/Fe$fixtureDir\fixture.exe", '/link', 'd3d12.lib', 'dxgi.lib'))

# The fixture runs inside a real ReShade: a copy of its dxgi.dll, a minimal ReShade.ini (no effects, no update check) and the add-on.
if (-not $ReShadeDll) {
  if (-not $GamePath) { $GamePath = Find-GamePath }
  $ReShadeDll = Join-Path $GamePath 'dxgi.dll'
}
Copy-Item -LiteralPath $ReShadeDll -Destination "$fixtureDir\dxgi.dll" -Force
Copy-Item -LiteralPath "$build\msfs2024-camera-fx-off.addon64" -Destination $fixtureDir -Force
[IO.File]::WriteAllLines("$fixtureDir\ReShade.ini", @(
    '[GENERAL]', 'CheckForUpdates=0', 'EffectSearchPaths=.\', 'TextureSearchPaths=.\', 'NoReloadOnInit=1', 'PerformanceMode=1', 'SkipLoadingDisabledEffects=1',
    '[OVERLAY]', 'TutorialProgress=4'))
Write-Host "built tests in $testsDir and $fixtureDir"
