# Stability experiment runner: starts the game, lets it sit in the garage, records how the run ended.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-test.ps1 -Name baseline-compare -Mode compare [-Reps 3] [-Seconds 300]
#       [-NoDebugLayer] [-Triangle 1] [-GpuTiming 1] [-Dev '{"directSubmit":true}'] [-Out results.jsonl]
#
# -Mode / -Triangle / -GpuTiming are written into aether.json before each run; -Dev becomes
# dev-options.json in Aether's config folder (experiment switches, src/Aether.Native/Core/DevOptions.h; "statsSeconds" is added).
# One JSON line per run is appended to -Out (default: benchmarks\soak-results.jsonl in Aether's data folder).
# Afterwards dev-options.json is removed and aether.json is put back to renderMode hdrp.
param(
	[Parameter(Mandatory = $true)][string]$Name,
	[ValidateSet('hdrp', 'aether', 'compare')][string]$Mode = 'hdrp',
	[int]$Reps = 1,
	[int]$Seconds = 300,
	[switch]$NoDebugLayer,
	[int]$Triangle = 1,
	[int]$GpuTiming = 1,
	[string]$Dev = '{}',
	[string]$Out = '',
	[string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# where Aether lives depends on the loader it was deployed for (scripts\deploy.ps1)
$bepinex = Test-Path (Join-Path $GameDir 'BepInEx\plugins\Aether\Aether.dll')
$dataDir = if ($bepinex) { Join-Path $GameDir 'BepInEx\plugins\Aether' } else { Join-Path $GameDir 'kino\mods\Aether' }
$configDir = if ($bepinex) { Join-Path $GameDir 'BepInEx\config\Aether' } else { Join-Path $GameDir 'kino\config\Aether' }
$cfg = Join-Path $configDir 'aether.json'
$devFile = Join-Path $configDir 'dev-options.json'
$log = Join-Path $dataDir 'logs\aether.jsonl'
$crashRoot = Join-Path $env:LOCALAPPDATA 'Temp\CarX Technologies\Drift Racing Online\Crashes'
if (-not $Out) { $Out = Join-Path $dataDir 'benchmarks\soak-results.jsonl' }

function Get-Game { Get-Process | Where-Object { $_.ProcessName -like 'Drift Racing Online*' } | Select-Object -First 1 }
function Set-Config([string]$mode, [int]$triangle, [int]$timing) {
	$c = [IO.File]::ReadAllText($cfg)
	$c = $c -replace '"renderMode":\s*"\w+"', ('"renderMode": "' + $mode + '"')
	$c = $c -replace '"testTriangle":\s*(true|false)', ('"testTriangle": ' + $(if ($triangle) { 'true' } else { 'false' }))
	$c = $c -replace '"gpuTiming":\s*(true|false)', ('"gpuTiming": ' + $(if ($timing) { 'true' } else { 'false' }))
	$c = $c -replace '"debugView":\s*"\w+"', '"debugView": "none"'
	[IO.File]::WriteAllText($cfg, $c)
}
function Read-Crash([string]$dir) {
	$r = @{ folder = (Split-Path $dir -Leaf) }
	$dmp = Join-Path $dir 'crash.dmp'
	$pl = Join-Path $dir 'Player.log'
	if (Test-Path $dmp) {
		$b = [IO.File]::ReadAllBytes($dmp)
		$n = [BitConverter]::ToUInt32($b, 8); $d = [BitConverter]::ToUInt32($b, 12)
		for ($i = 0; $i -lt $n; $i++) {
			$o = $d + $i * 12
			if ([BitConverter]::ToUInt32($b, $o) -eq 6) {
				$rva = [BitConverter]::ToUInt32($b, $o + 8)
				$r.exceptionCode = '{0:X8}' -f [BitConverter]::ToUInt32($b, $rva + 8)
			}
		}
	}
	if (Test-Path $pl) {
		$lines = Get-Content $pl
		$m = $lines | Select-String 'UnityPlayer.dll:UnityPlayer.dll \(([0-9A-F]+)\)' | Select-Object -First 1
		$base = if ($m) { [Convert]::ToUInt64($m.Matches[0].Groups[1].Value, 16) } else { 0 }
		$r.stack = @($lines | Where-Object { $_ -match '^0x([0-9A-F]+) \((\w+)\)' } | Select-Object -First 6 | ForEach-Object {
			$null = $_ -match '^0x([0-9A-F]+) \((\w+)\)'
			if ($Matches[2] -eq 'UnityPlayer' -and $base) { 'UnityPlayer+{0:X}' -f ([Convert]::ToUInt64($Matches[1], 16) - $base) } else { $Matches[2] }
		})
	}
	return $r
}

$devObject = $Dev | ConvertFrom-Json
$devObject | Add-Member -NotePropertyName statsSeconds -NotePropertyValue 5 -Force
try {
	for ($rep = 1; $rep -le $Reps; $rep++) {
		$waitUntil = (Get-Date).AddSeconds(90) # the process of the previous run can take a while to go away
		while ((Get-Game) -and (Get-Date) -lt $waitUntil) { Start-Sleep -Seconds 3 }
		if (Get-Game) { throw 'the game is already running' }
		Set-Config $Mode $Triangle $GpuTiming
		[IO.File]::WriteAllText($devFile, ($devObject | ConvertTo-Json -Compress))
		$crashesBefore = @(Get-ChildItem $crashRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
		$launchArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'scripts\launch-game.ps1'), '-TimeoutSeconds', '240')
		if (-not $NoDebugLayer) { $launchArgs += '-DebugLayer' }
		$launchTime = Get-Date
		$null = & powershell @launchArgs
		$start = Get-Date
		while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and (Get-Game)) { Start-Sleep -Seconds 3 }
		$alive = [bool](Get-Game)
		$ran = [int]((Get-Date) - $start).TotalSeconds
		$result = [ordered]@{ name = $Name; rep = $rep; time = (Get-Date -Format s); mode = $Mode; debugLayer = (-not $NoDebugLayer); triangle = $Triangle; gpuTiming = $GpuTiming
			dev = $devObject; survived = $alive; seconds = $ran }
		if ($alive) {
			$null = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'scripts\launch-game.ps1') -Stop
			$closeUntil = (Get-Date).AddSeconds(60)
			while ((Get-Game) -and (Get-Date) -lt $closeUntil) { Start-Sleep -Seconds 3 }
			$result.closed = -not [bool](Get-Game)
			if (-not $result.closed) { # a test run that hangs on exit must not block the next one
				Get-Game | ForEach-Object { Stop-Process -Id $_.Id -Force -Confirm:$false }
				$result.forcedKill = $true
				Start-Sleep -Seconds 5
			}
		} else {
			Start-Sleep -Seconds 12 # the crash handler writes its folder after the process is gone
			$new = Get-ChildItem $crashRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $crashesBefore -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
			if ($new) { $result.crash = Read-Crash $new.FullName }
		}
		$entries = @(Get-Content $log | ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
		$result.aetherStarted = [bool]($entries.Count -gt 0 -and [datetime]$entries[0].timestamp -ge $launchTime.AddSeconds(-5))
		$stats = $entries | Where-Object { $_.message -eq 'Submission statistics' } | Select-Object -Last 1
		if ($stats) { $result.lastStats = $stats.extra }
		$d3d = @($entries | Where-Object { $_.component -eq 'D3D12' })
		$result.d3d12Messages = $d3d.Count
		if ($d3d.Count) {
			$result.lastD3D12 = $d3d[-1].code + ': ' + $d3d[-1].message.Substring(0, [Math]::Min(200, $d3d[-1].message.Length))
			$result.firstD3D12 = $d3d[0].code + ': ' + $d3d[0].message.Substring(0, [Math]::Min(200, $d3d[0].message.Length))
		}
		$result.aetherErrors = @($entries | Where-Object { $_.severity -eq 'error' -and $_.component -ne 'D3D12' }).Count
		$result.aetherErrorCodes = @($entries | Where-Object { ($_.severity -eq 'error' -or $_.severity -eq 'fatal') -and $_.component -ne 'D3D12' } | ForEach-Object { $_.code } | Select-Object -Unique)
		$result.d3d12Kinds = @($d3d | ForEach-Object { ($_.message -split ':')[0..1] -join ':' } | Group-Object | ForEach-Object { "$($_.Count)x $($_.Name)" })
		$result.cleanShutdown =[bool]($entries | Where-Object { $_.message -like '*stopped cleanly*' })
		$result.leakWarning = [bool]($entries | Where-Object { $_.code -eq 'W_SHUTDOWN_LEAK' })
		Add-Content -Path $Out -Value ($result | ConvertTo-Json -Compress -Depth 6) -Encoding utf8
		$frames = if ($stats) { $stats.extra.frame } else { '?' }
		$subs = if ($stats) { $stats.extra.submissions } else { '?' }
		Write-Host ("{0} #{1}: {2} after {3} s, frames {4}, submissions {5}, d3d12 messages {6}{7}" -f $Name, $rep, $(if (-not $result.aetherStarted) { 'INVALID (Aether did not start)' } elseif ($alive) { 'SURVIVED' } else { 'CRASHED' }), $ran, $frames, $subs, $d3d.Count,
			$(if ($result.lastD3D12) { ' | ' + $result.lastD3D12.Substring(0, [Math]::Min(110, $result.lastD3D12.Length)) } else { '' }))
		Start-Sleep -Seconds 5
	}
} finally {
	if (Test-Path $devFile) { [IO.File]::Delete($devFile) }
	Set-Config 'hdrp' 1 1
}
