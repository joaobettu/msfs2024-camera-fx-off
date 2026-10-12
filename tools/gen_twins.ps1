<#
.SYNOPSIS
Generates msfs2024-camera-fx-off.twins: for every MSFS 2024 tonemapping permutation with bloom, the game's own
permutation without it. Run it again after a sim update changes the shader package.

.DESCRIPTION
Bloom permutations of ph_tonemapping bind the lens dirt texture at t8; the others don't. A permutation's twin binds the
same resources minus t8/s8, and its disassembled code is the bloom permutation's code with one block (the bloom) removed.
Candidates are compared with a sequence diff, and the script refuses to write a file unless every pair is unambiguous.
Only reads the game files: the disassembler is a copy of the game's dxcompiler.dll in build\tools.
#>
param(
  # The folder that holds FlightSimulator2024.exe; found in the Steam libraries when left out.
  [string]$GamePath = '',
  [string]$Out = (Join-Path $PSScriptRoot '..\build\msfs2024-camera-fx-off.twins')
)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
. (Join-Path $PSScriptRoot '..\scripts\find-game.ps1')
if (-not $GamePath) { $GamePath = Find-GamePath }

$pak = @(Get-ChildItem (Join-Path $GamePath 'Shaders') -Filter 'SHADERSPAK.*.SM6.PC.PAK' -ErrorAction Stop)
if ($pak.Count -ne 1) { throw "expected one SHADERSPAK.*.SM6.PC.PAK in $GamePath\Shaders, found $($pak.Count)" }
$pak = $pak[0]

$dxcDir = Join-Path $PSScriptRoot '..\build\tools'
New-Item -ItemType Directory -Force $dxcDir | Out-Null
$dxc = Join-Path $dxcDir 'dxcompiler.dll'
Copy-Item -LiteralPath (Join-Path $GamePath 'Dxcompiler.DLL') -Destination $dxc -Force
Add-Type -Path (Join-Path $PSScriptRoot 'ShaderTools.cs')
[CamFx.Dxc]::Init($dxc)

Write-Host "reading $($pak.FullName) ($($pak.Length) bytes)"
$b = [IO.File]::ReadAllBytes($pak.FullName)
$enc = [Text.Encoding]::GetEncoding(28591)
$s = $enc.GetString($b)

# Every ph_tonemapping container: its bindings without t8/s8, whether it binds t8 (bloom), and its code as a sequence.
$variants = New-Object System.Collections.Generic.List[object]
$i = 0
while (($i = $s.IndexOf('DXBC', $i, [StringComparison]::Ordinal)) -ge 0) {
  $dx = $i; $i += 4
  if ($dx + 32 -ge $b.Length) { break }
  if ([BitConverter]::ToUInt16($b, $dx + 20) -ne 1) { continue }
  $size = [BitConverter]::ToUInt32($b, $dx + 24); $count = [BitConverter]::ToUInt32($b, $dx + 28)
  if ($count -lt 1 -or $count -gt 32 -or $size -lt 32 -or ($dx + $size) -gt $b.Length) { continue }
  $entry = $null; $bindings = @(); $bloom = $false
  for ($j = 0; $j -lt $count; $j++) {
    $off = [BitConverter]::ToUInt32($b, $dx + 32 + 4 * $j)
    if ($enc.GetString($b, $dx + $off, 4) -ne 'PSV0') { continue }
    $p = $dx + $off + 8; $partSize = [int][BitConverter]::ToUInt32($b, $dx + $off + 4)
    foreach ($m in [regex]::Matches($s.Substring($p, [Math]::Min($partSize, 8192)), '[A-Za-z_][A-Za-z0-9_]{2,}')) { if ($m.Value -match '^[a-z]{1,3}h_') { $entry = $m.Value; break } }
    if ($entry -ne 'ph_tonemapping') { break }
    $infoSize = [BitConverter]::ToUInt32($b, $p); $q = $p + 4 + $infoSize
    $resources = [BitConverter]::ToUInt32($b, $q)
    if ($resources -gt 0 -and $resources -lt 256) {
      $stride = [BitConverter]::ToUInt32($b, $q + 4)
      for ($r = 0; $r -lt $resources; $r++) {
        $e = $q + 8 + $r * $stride
        $type = [BitConverter]::ToUInt32($b, $e); $space = [BitConverter]::ToUInt32($b, $e + 4); $lower = [BitConverter]::ToUInt32($b, $e + 8)
        if ($space -eq 0 -and $lower -eq 8 -and ($type -eq 3 -or $type -eq 1)) { if ($type -eq 3) { $bloom = $true }; continue }
        $bindings += "$type/$space/$lower"
      }
    }
  }
  if ($entry -ne 'ph_tonemapping') { continue }
  $variants.Add([pscustomobject]@{
      Index = $variants.Count; Offset = $dx; Size = [int]$size; Bloom = $bloom
      Digest = ($b[($dx + 4)..($dx + 19)] | ForEach-Object { $_.ToString('x2') }) -join ''
      Bindings = (($bindings | Sort-Object) -join ',')
      Code = [CamFx.Seq]::Build([CamFx.Dxc]::Disassemble($b, $dx, [int]$size))
    })
}
$bloomVariants = @($variants | Where-Object Bloom)
$plainVariants = @($variants | Where-Object { -not $_.Bloom })
Write-Host "ph_tonemapping permutations: $($variants.Count) ($($bloomVariants.Count) with bloom)"
if ($bloomVariants.Count -eq 0) { throw 'no bloom permutation found: the shader package layout changed' }

$pairs = New-Object System.Collections.Generic.List[object]
$problems = 0
foreach ($v in $bloomVariants) {
  $candidates = foreach ($w in $plainVariants) {
    if ($w.Bindings -ne $v.Bindings) { continue }
    $d = [CamFx.Seq]::EditDistance($v.Code, $w.Code, 600)
    if ($d -lt 0) { continue }
    $lengthDiff = $v.Code.Length - $w.Code.Length
    [pscustomobject]@{ W = $w; Inserted = [int](($d - $lengthDiff) / 2); Removed = [int](($d + $lengthDiff) / 2); Total = $d }
  }
  $sorted = @($candidates | Sort-Object Inserted, Removed)
  if ($sorted.Count -eq 0) { Write-Warning "no candidate for $($v.Digest)"; $problems++; continue }
  $best = $sorted[0]
  # A twin only lacks the bloom block: few inserted lines, and clearly closer than any other permutation.
  $margin = if ($sorted.Count -gt 1) { $sorted[1].Total - $best.Total } else { [int]::MaxValue }
  if ($best.Inserted -gt 20 -or $margin -lt 5) { Write-Warning ("ambiguous twin for {0}: inserted {1}, margin {2}" -f $v.Digest, $best.Inserted, $margin); $problems++; continue }
  $pairs.Add([pscustomobject]@{ Bloom = $v.Digest; Twin = $best.W.Digest; Offset = $best.W.Offset; Size = $best.W.Size })
}
$distinct = @($pairs | Select-Object -ExpandProperty Twin -Unique).Count
if ($problems -ne 0 -or $distinct -ne $pairs.Count) { throw "pairing is not clean ($problems problems, $distinct distinct twins for $($pairs.Count) pairs); not writing $Out" }

$lines = @(
  '# MSFS 2024 Camera FX Off: tonemapping permutations with bloom and their twins without it',
  "# generated by tools\gen_twins.ps1 on $(Get-Date -Format 'yyyy-MM-dd HH:mm') from $($pak.Name)",
  '# <bloom permutation digest> <twin digest> <twin offset in the package> <twin size>',
  "pak $($pak.Name) $($pak.Length)"
) + ($pairs | ForEach-Object { '{0} {1} {2} {3}' -f $_.Bloom, $_.Twin, $_.Offset, $_.Size })
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($Out), $lines)
Write-Host "wrote $($pairs.Count) pairs to $([IO.Path]::GetFullPath($Out))"
