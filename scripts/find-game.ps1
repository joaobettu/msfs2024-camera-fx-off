# Finds the MSFS 2024 folder in the Steam libraries, for the scripts' default -GamePath. Other installs, such as the
# Microsoft Store version, need -GamePath.
# The folder is told by the game files the scripts use (its shader package and Dxcompiler.DLL), not by the executable:
# the first Steam build of Sim Update 7 shipped FlightSimulator2024.exe as KittyHawkx64_Steam_PCMBF.exe.
function Test-GamePath([string]$path) {
  return (Test-Path -LiteralPath (Join-Path $path 'Dxcompiler.DLL')) -and
    @(Get-ChildItem -LiteralPath (Join-Path $path 'Shaders') -Filter 'SHADERSPAK.*.SM6.PC.PAK' -ErrorAction SilentlyContinue).Count -ge 1
}

function Find-GamePath {
  $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
  if ($steam) {
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
      # "path"		"E:\\Games\\Steam"
      $libraries += Select-String -LiteralPath $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($library in $libraries) {
      $path = Join-Path $library 'steamapps\common\Limitless'
      if (Test-GamePath $path) { return $path }
    }
  }
  throw 'MSFS 2024 not found in the Steam libraries: pass -GamePath with the game folder (the one with the Shaders folder and Dxcompiler.DLL)'
}

# The sim's process while it runs, under either executable name.
function Get-SimProcess {
  return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'FlightSimulator2024' -or $_.ProcessName -like 'KittyHawk*' })
}
