# Runs the native runtime on the test host (no game): D3D12 debug layer on, overlay, a window resize and a frame capture.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-host.ps1 [-Config RelWithDebInfo] [-Seconds 6] [-Warp] [-Show] [-Keep]
#
# Passes when AetherHost prints HOST OK (no D3D12 errors, no Aether errors, no leaked device objects, capture written).
# -Show: a visible, focused window. -Keep: keep the data folder (logs, capture) and print where it is.
param(
	[string]$Config = 'RelWithDebInfo',
	[double]$Seconds = 6,
	[switch]$Warp,
	[switch]$Show,
	[switch]$Keep
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\native\$Config\AetherHost.exe"
if (-not (Test-Path $exe)) { Write-Host "AetherHost.exe not found: run scripts\build-native.ps1 first"; exit 1 }
$data = Join-Path $env:TEMP "aether-host-test-$PID"
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $data 'shaders') | Out-Null
Copy-Item (Join-Path $repo 'shaders\*') (Join-Path $data 'shaders')
$env:AETHER_PIPE_NAME = "Aether.Test.$PID" # not the game's pipe
$hostArgs = @('--data', $data, '--seconds', $Seconds, '--overlay', '--resize', '--capture')
if (-not $Show) { $hostArgs += '--hidden' }
if ($Warp) { $hostArgs += '--warp' }
& $exe @hostArgs
$code = $LASTEXITCODE
$log = Join-Path $data 'logs\aether.jsonl'
if (Test-Path $log) {
	$bad = Get-Content $log | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.severity -in 'warning', 'error', 'fatal' }
	$bad | ForEach-Object { Write-Host "  log $($_.severity) [$($_.component)] $($_.code) $($_.message)" }
}
if ($Keep) { Write-Host "data: $data" } else { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue }
exit $code
