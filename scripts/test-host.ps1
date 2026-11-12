# Runs the native runtime on the test host (no game), D3D12 debug layer on. Two runs:
#   1. frame end path: test triangle, overlay, a window resize, a frame capture;
#   2. scene path: the validation scene through the scene data contract, drawn by Aether's scene renderer, with the
#      captures checked pixel by pixel (where things are, what is in front, which side of a face is drawn, a mirrored
#      instance, shadows, blending, the diagnostic material, a scene unload and reload).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-host.ps1 [-Config RelWithDebInfo] [-Seconds 12] [-Warp] [-Show] [-Keep] [-SceneOnly]
#
# Passes when both runs print HOST OK (no D3D12 errors, no Aether errors, no leaked device objects) and every pixel
# check holds. -Show: visible, focused windows. -Keep: keep the data folders (logs, captures) and print where they are.
param(
	[string]$Config = 'RelWithDebInfo',
	[double]$Seconds = 12,
	[switch]$Warp,
	[switch]$Show,
	[switch]$Keep,
	[switch]$SceneOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\native\$Config\AetherHost.exe"
if (-not (Test-Path $exe)) { Write-Host "AetherHost.exe not found: run scripts\build-native.ps1 first"; exit 1 }
$env:AETHER_PIPE_NAME = "Aether.Test.$PID" # not the game's pipe
$script:failed = 0

function New-Data([string]$name) {
	$data = Join-Path $env:TEMP "aether-host-test-$PID-$name"
	Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
	New-Item -ItemType Directory -Force (Join-Path $data 'shaders') | Out-Null
	Copy-Item (Join-Path $repo 'shaders\*') (Join-Path $data 'shaders')
	return $data
}

function Show-LogProblems([string]$data) {
	$log = Join-Path $data 'logs\aether.jsonl'
	if (Test-Path $log) {
		Get-Content $log | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.severity -in 'warning', 'error', 'fatal' } |
			ForEach-Object { Write-Host "  log $($_.severity) [$($_.component)] $($_.code) $($_.message)" }
	}
}

function Check([string]$name, [bool]$ok, [string]$detail = '') {
	if ($ok) { Write-Host "  ok    $name" } else { Write-Host "  FAIL  $name  $detail"; $script:failed++ }
}

# ---------------------------------------------------------------- 1. frame end path
if (-not $SceneOnly) {
	Write-Host 'frame end path (triangle, overlay, resize, capture)'
	$data = New-Data 'frame'
	$hostArgs = @('--data', $data, '--seconds', $Seconds, '--overlay', '--resize', '--capture')
	if (-not $Show) { $hostArgs += '--hidden' }
	if ($Warp) { $hostArgs += '--warp' }
	$out = & $exe @hostArgs
	$out | Where-Object { $_ -notmatch '^probe ' } | ForEach-Object { Write-Host "  $_" }
	Check 'frame end run' ($LASTEXITCODE -eq 0)
	Show-LogProblems $data
	if ($Keep) { Write-Host "  data: $data" } else { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------- 2. scene path
Write-Host 'scene path (validation scene through the scene data contract)'
$data = New-Data 'scene'
$hostArgs = @('--data', $data, '--seconds', $Seconds, '--scene')
if (-not $Show) { $hostArgs += '--hidden' }
if ($Warp) { $hostArgs += '--warp' }
$out = & $exe @hostArgs
$code = $LASTEXITCODE
$out | Where-Object { $_ -notmatch '^(probe |scene stats|gpu resources)' } | ForEach-Object { Write-Host "  $_" }
Check 'scene run (no D3D12 errors, no leaks, captures written)' ($code -eq 0)
Show-LogProblems $data

$probes = @{}
foreach ($line in $out) { if ($line -match '^probe (\S+) (\d+) (\d+)') { $probes[$Matches[1]] = @([int]$Matches[2], [int]$Matches[3]) } }
Add-Type -AssemblyName System.Drawing
function Get-Pixel([string]$file, [string]$probe) {
	$path = Join-Path $data "captures\$file"
	if (-not (Test-Path $path) -or -not $probes.ContainsKey($probe)) { return $null }
	$bmp = New-Object System.Drawing.Bitmap $path
	try { return $bmp.GetPixel($probes[$probe][0], $probes[$probe][1]) } finally { $bmp.Dispose() }
}
function Lum($c) { return 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B }
function Text($c) { if ($null -eq $c) { return 'no pixel' } return "rgb($($c.R),$($c.G),$($c.B))" }

if ($code -eq 0) {
	# where things are and what is in front (camera, projection, depth)
	$c = Get-Pixel 'scene-lit.png' 'red';          Check 'red box is left of centre, in front of the wall' ($c -and $c.R -gt 2 * $c.G -and $c.R -gt 2 * $c.B) (Text $c)
	$c = Get-Pixel 'scene-lit.png' 'blue';         Check 'blue box is above centre, in front of the wall' ($c -and $c.B -gt 2 * $c.R -and $c.B -gt 2 * $c.G) (Text $c)
	$c = Get-Pixel 'scene-lit.png' 'wall';         Check 'the wall shows where nothing is in front of it' ($c -and $c.R -gt 2 * $c.B -and $c.G -gt 2 * $c.B) (Text $c)
	# blending: the green box through the blue pane
	$c = Get-Pixel 'scene-lit.png' 'green_glass';  Check 'green box seen through the blue pane (alpha blending, back to front)' ($c -and $c.B -gt 1.5 * $c.R -and $c.G -gt 1.5 * $c.R) (Text $c)
	# the diagnostic material: magenta / dark checker, never green
	$c = Get-Pixel 'scene-lit.png' 'unsupported';  Check 'unsupported material is drawn with the diagnostic pattern' ($c -and $c.R -gt 2 * $c.G + 5 -and $c.B -gt 2 * $c.G + 5) (Text $c)
	# shadows
	$lit = Get-Pixel 'scene-lit.png' 'ground_lit'; $shadow = Get-Pixel 'scene-lit.png' 'ground_shadow'
	Check 'the red box shadows the ground on the side away from the sun' ($lit -and $shadow -and (Lum $shadow) -lt 0.75 * (Lum $lit)) "lit $(Text $lit) shadow $(Text $shadow)"
	# which side of a face is drawn: the front face's normal is (0, 0, -1) -> encoded blue channel 0
	$c = Get-Pixel 'scene-normal.png' 'red';       Check 'front faces are drawn (back-face culling, winding)' ($c -and $c.B -lt 40 -and [Math]::Abs($c.R - $c.G) -lt 12 -and $c.R -gt 150) (Text $c)
	$c = Get-Pixel 'scene-normal.png' 'mirrored';  Check 'a mirrored instance still shows its outside (negative determinant)' ($c -and $c.B -lt 40 -and [Math]::Abs($c.R - $c.G) -lt 12 -and $c.R -gt 150) (Text $c)
	# handles after a destroy, a texture re-upload, a scene clear and a re-register
	$c = Get-Pixel 'scene-bvh.png' 'red';          $s = Get-Pixel 'scene-bvh.png' 'mirrored'; Check 'rays through the ray tracing structure find the boxes (their faces towards the camera)' ($c -and $s -and $c.B -lt 40 -and [Math]::Abs($c.R - $c.G) -lt 12 -and $c.R -gt 20 -and $s.R -gt 20) (Text $c)
	$c = Get-Pixel 'scene-after.png' 'blue';       Check 'the scene is back after unload and reload' ($c -and $c.B -gt 2 * $c.R -and $c.B -gt 2 * $c.G) (Text $c)
	$stats = ($out | Where-Object { $_ -match '^scene stats: ' }) -replace '^scene stats: ', '' | ConvertFrom-Json
	Check 'scene store: 8 instances, 1 mesh, 8 materials, no contract errors' ($stats.instances.registered -eq 8 -and $stats.meshes.count -eq 1 -and $stats.materials.count -eq 8 -and $stats.contractErrors -eq 0) ($stats | ConvertTo-Json -Compress -Depth 4)
	Check 'scene store: destroys and the scene clear were applied' ($stats.commands.destroys -ge 1 -and $stats.commands.sceneClears -eq 1)
	$gpu = ($out | Where-Object { $_ -match '^gpu resources: ' }) -replace '^gpu resources: ', '' | ConvertFrom-Json
	Check "GPU resource manager: retirements done ($($gpu.objectsRetired) objects, $($gpu.pendingRetirements) pending), descriptors $($gpu.descriptors.srvUsed) / peak $($gpu.descriptors.srvPeak)" ($gpu.objectsRetired -gt 0 -and $gpu.pendingRetirements -lt 20 -and $gpu.descriptors.srvUsed -lt 400)
}
if ($Keep) { Write-Host "  data: $data" } else { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue }

if ($script:failed -eq 0) { Write-Host 'HOST TEST OK'; exit 0 } else { Write-Host "HOST TEST FAILED ($script:failed)"; exit 1 }
