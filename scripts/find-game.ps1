# Finds the MSFS 2024 folder (the one that holds FlightSimulator2024.exe) in the Steam libraries, for the scripts' default
# -GamePath. Other installs, such as the Microsoft Store version, need -GamePath.
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
      if (Test-Path -LiteralPath (Join-Path $path 'FlightSimulator2024.exe')) { return $path }
    }
  }
  throw 'MSFS 2024 not found in the Steam libraries: pass -GamePath with the folder that holds FlightSimulator2024.exe'
}
