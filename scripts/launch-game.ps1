# Starts the game through Steam in DirectX 12 and waits until Aether answers on its pipe.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\launch-game.ps1 [-TimeoutSeconds 180] [-DebugLayer] [-Stop]
#
# -DebugLayer adds Unity's -force-d3d12-debug (D3D12 debug layer; slow, messages go to the game's Player.log).
# -Stop closes the game instead (asks its window to close, like the X button).
# Exit code 0 = Aether answered; 1 = the game started but Aether did not answer in time; 2 = the game did not start.
param(
	[int]$TimeoutSeconds = 180,
	[switch]$DebugLayer,
	[switch]$Stop
)
$ErrorActionPreference = 'Stop'
$appId = 635260

function Get-Game { Get-Process | Where-Object { $_.ProcessName -like 'Drift Racing Online*' } | Select-Object -First 1 }

if ($Stop) {
	$game = Get-Game
	if (-not $game) { Write-Host 'the game is not running'; exit 0 }
	$null = $game.CloseMainWindow()
	if (-not $game.WaitForExit(30000)) { Write-Host 'the game did not close within 30 s (close it by hand)'; exit 1 }
	Write-Host 'game closed'
	exit 0
}

if (Get-Game) { Write-Host 'the game is already running'; exit 2 }
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamExe
if (-not $steam -or -not (Test-Path $steam)) { $steam = "${env:ProgramFiles(x86)}\Steam\steam.exe" }
if (-not (Test-Path $steam)) { Write-Host 'steam.exe not found'; exit 2 }
$steamArgs = @('-applaunch', $appId, '-force-d3d12')
if ($DebugLayer) { $steamArgs += '-force-d3d12-debug' }
Start-Process -FilePath $steam -ArgumentList $steamArgs
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not (Get-Game)) {
	if ((Get-Date) -gt $deadline) { Write-Host 'the game process did not appear'; exit 2 }
	Start-Sleep -Milliseconds 500
}
Write-Host "game started (pid $((Get-Game).Id)); waiting for Aether's pipe"
while ((Get-Date) -lt $deadline) {
	if (-not (Get-Game)) { Write-Host 'the game exited'; exit 2 }
	if (Test-Path '\\.\pipe\Aether.Runtime') { Write-Host 'Aether is listening'; exit 0 }
	Start-Sleep -Milliseconds 500
}
Write-Host "Aether did not answer within $TimeoutSeconds s (see kino\output.log and kino\mods\Aether\logs\aether.jsonl)"
exit 1
