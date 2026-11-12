# End-to-end test without the game: AetherHost (the runtime on a test window) + the MCP server over stdio.
# Calls every tool, checks the permission rules (read-only server, in-game "ask" timing out, bad arguments) and that a
# stopped runtime is reported cleanly.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-mcp.ps1 [-Config RelWithDebInfo] [-Keep]
#
# Prints one line per check and "MCP TEST OK" / "MCP TEST FAILED"; exit code 0 / 1.
param(
	[string]$Config = 'RelWithDebInfo',
	[switch]$Keep
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$hostExe = Join-Path $repo "build\native\$Config\AetherHost.exe"
$mcpExe = Join-Path $repo 'build\mcp\Release\Aether.MCP.exe'
foreach ($f in $hostExe, $mcpExe) { if (-not (Test-Path $f)) { Write-Host "missing $f (build first)"; exit 1 } }

$data = Join-Path $env:TEMP "aether-mcp-test-$PID"
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $data 'shaders'), (Join-Path $data 'config') | Out-Null
Copy-Item (Join-Path $repo 'shaders\*') (Join-Path $data 'shaders')
$pipe = "Aether.Test.$PID"
$env:AETHER_PIPE_NAME = $pipe
$script:failed = 0
$script:nextId = 1

function Check([string]$name, [bool]$ok, [string]$detail = '') {
	if ($ok) { Write-Host "  ok    $name" } else { Write-Host "  FAIL  $name  $detail"; $script:failed++ }
}

function Start-Mcp([string[]]$mcpArgs) {
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = $mcpExe
	$psi.Arguments = ($mcpArgs -join ' ')
	$psi.RedirectStandardInput = $true
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	$psi.UseShellExecute = $false
	$psi.CreateNoWindow = $true
	$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
	$p = [System.Diagnostics.Process]::Start($psi)
	$p.BeginErrorReadLine()
	return $p
}

function Send($mcp, [string]$method, $params) {
	$id = $script:nextId++
	$msg = @{ jsonrpc = '2.0'; id = $id; method = $method }
	if ($null -ne $params) { $msg.params = $params }
	$line = $msg | ConvertTo-Json -Depth 12 -Compress
	$bytes = [System.Text.Encoding]::UTF8.GetBytes($line + "`n")
	$mcp.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
	$mcp.StandardInput.BaseStream.Flush()
	$task = $mcp.StandardOutput.ReadLineAsync()
	if (-not $task.Wait(60000)) { throw "no answer to $method within 60 s" }
	return $task.Result | ConvertFrom-Json
}

function Call($mcp, [string]$tool, $arguments = @{}) {
	return (Send $mcp 'tools/call' @{ name = $tool; arguments = $arguments }).result
}

function Start-AetherHost([string]$control, [int]$seconds) {
	@{ mcpControl = $control; mcpApprovalTimeoutSec = 5 } | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $data 'config\aether.json')
	$p = Start-Process -FilePath $hostExe -ArgumentList @('--data', "`"$data`"", '--seconds', $seconds, '--overlay', '--hidden') -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $data "host-$control.txt")
	Start-Sleep -Milliseconds 2500 # device, first frames, pipe
	return $p
}

$mcp = $null; $ro = $null; $hostProc = $null
try {
	# ---------------------------------------------------------------- game side allows control
	Write-Host 'runtime with mcpControl = allow, server with --allow-control'
	$hostProc = Start-AetherHost 'allow' 40
	$mcp = Start-Mcp @('--allow-control')
	$init = Send $mcp 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'test'; version = '0' } }
	Check 'initialize' ($init.result.serverInfo.name -eq 'aether' -and $init.result.protocolVersion -eq '2025-06-18')
	$tools = (Send $mcp 'tools/list' $null).result.tools
	Check "tools/list ($($tools.Count) tools, each with input and output schema)" ($tools.Count -eq 21 -and @($tools | Where-Object { -not $_.inputSchema -or -not $_.outputSchema }).Count -eq 0)

	$r = Call $mcp 'get_status'
	Check 'get_status' (-not $r.isError -and $r.structuredContent.renderer.state -eq 'ready') ($r.content[0].text)
	$r = Call $mcp 'get_gpu_info'
	Check "get_gpu_info ($($r.structuredContent.adapter.vendor) $($r.structuredContent.adapter.name), SM $($r.structuredContent.features.shaderModel))" (-not $r.isError -and $r.structuredContent.available -and $r.structuredContent.features.resourceBindingTier -ge 1)
	$r = Call $mcp 'get_performance' @{ historyFrames = 30 }
	Check ("get_performance ({0:N1} fps, GPU frame {1:N2} ms)" -f $r.structuredContent.fps, $r.structuredContent.gpuFrameTimeMs.avg) (-not $r.isError -and $r.structuredContent.fps -gt 1 -and $r.structuredContent.frameTimeHistoryMs.Count -eq 30 -and $r.structuredContent.gpuFrameTimeMs.avg -gt 0)
	$r = Call $mcp 'get_vram_usage'
	Check ("get_vram_usage (process {0:N0} MB, Aether {1:N0} KB)" -f ($r.structuredContent.process.dedicatedUsageBytes / 1MB), ($r.structuredContent.aether.gpuBytes / 1KB)) (-not $r.isError -and $r.structuredContent.process.dedicatedUsageBytes -gt 0)
	$r = Call $mcp 'get_resource_list' @{ limit = 50 }
	Check "get_resource_list ($($r.structuredContent.count) resources)" (-not $r.isError -and $r.structuredContent.count -ge 2 -and ($r.structuredContent.resources | Where-Object name -eq 'TestTriangle.VertexBuffer'))
	$r = Call $mcp 'get_render_graph'
	Check "get_render_graph ($($r.structuredContent.passes.Count) passes)" (-not $r.isError -and ($r.structuredContent.passes | Where-Object name -eq 'Overlay').lastFrame.executed)
	$r = Call $mcp 'get_shader_errors' @{ includeAllShaders = $true }
	Check 'get_shader_errors (none)' (-not $r.isError -and $r.structuredContent.errorCount -eq 0 -and $r.structuredContent.all.shaders.Count -ge 2)
	$r = Call $mcp 'get_logs' @{ limit = 20; minSeverity = 'info' }
	Check "get_logs ($($r.structuredContent.count) entries)" (-not $r.isError -and $r.structuredContent.count -gt 3 -and $r.structuredContent.entries[0].timestamp)
	$r = Call $mcp 'get_crash_report'
	Check 'get_crash_report (no failure)' (-not $r.isError -and $null -eq $r.structuredContent.currentSession)
	$r = Call $mcp 'get_settings'
	Check 'get_settings' (-not $r.isError -and $r.structuredContent.values.quality -and $r.structuredContent.schema.quality.enum.Count -eq 4)
	$r = Call $mcp 'get_scene_info'
	Check 'get_scene_info (no game: reported as not available)' (-not $r.isError -and $r.structuredContent.available -eq $false)
	$r = Call $mcp 'get_material_info' @{ refresh = $false }
	Check 'get_material_info (no game: reported as not available)' (-not $r.isError -and $r.structuredContent.available -eq $false)

	$r = Call $mcp 'set_render_quality' @{ quality = 'low' }
	$s = Call $mcp 'get_settings'
	Check 'set_render_quality low' (-not $r.isError -and $s.structuredContent.values.quality -eq 'low') ($r.content[0].text)
	$r = Call $mcp 'set_render_quality' @{ quality = 'insane' }
	Check 'set_render_quality rejects an unknown preset' ($r.isError -and $r.content[0].text -like 'E_INVALID_PARAMS*')
	$r = Call $mcp 'set_render_feature' @{ feature = 'testTriangle'; enabled = $false }
	Start-Sleep -Milliseconds 300
	$g = Call $mcp 'get_render_graph'
	Check 'set_render_feature testTriangle off (the pass stops)' (-not $r.isError -and -not ($g.structuredContent.passes | Where-Object name -eq 'TestTriangle').lastFrame.executed)
	$r = Call $mcp 'set_render_feature' @{ feature = 'testTriangle'; enabled = $true }
	$r2 = Call $mcp 'set_render_feature' @{ feature = 'mcpControl'; enabled = $true }
	Check 'set_render_feature cannot reach other settings' ((-not $r.isError) -and $r2.isError)

	# a shader error is data: break the shader, reload, read the compiler output, fix it, reload
	$shader = Join-Path $data 'shaders\test_triangle.hlsl'
	$good = Get-Content $shader -Raw
	Set-Content -Encoding ascii $shader ($good -replace 'return float4\(input.color, 1.0\);', 'return float4(input.colour, 1.0);')
	$r = Call $mcp 'reload_shader'
	# both entry points live in the broken file: two failed shaders
	Check 'reload_shader reports the compile error' (-not $r.isError -and $r.structuredContent.errorCount -eq 2 -and $r.structuredContent.errors[0].compilerOutput -match "error X3018: invalid subscript 'colour'") ($r.content[0].text)
	$st = Call $mcp 'get_status'
	Check 'a shader error does not stop the renderer' ($st.structuredContent.renderer.state -eq 'ready')
	Set-Content -Encoding ascii $shader $good
	$r = Call $mcp 'reload_shader'
	Check 'reload_shader after the fix' (-not $r.isError -and $r.structuredContent.errorCount -eq 0)

	$r = Call $mcp 'capture_frame' @{ withOverlay = $true; includeImage = $true }
	$file = $r.structuredContent.file
	Check "capture_frame ($($r.structuredContent.width)x$($r.structuredContent.height))" (-not $r.isError -and $r.structuredContent.state -eq 'done' -and (Test-Path $file) -and (Get-Item $file).Length -gt 1000 -and ($r.content | Where-Object type -eq 'image'))
	$r = Call $mcp 'capture_render_graph'
	Check 'capture_render_graph' (-not $r.isError -and (Test-Path $r.structuredContent.file))
	$r = Call $mcp 'run_benchmark' @{ seconds = 2; label = 'test' }
	Check ("run_benchmark ({0} frames, {1:N1} fps avg, {2:N1} fps 1% low)" -f $r.structuredContent.result.frames, $r.structuredContent.result.averageFps, $r.structuredContent.result.onePercentLowFps) (-not $r.isError -and $r.structuredContent.state -eq 'done' -and $r.structuredContent.result.frames -gt 20 -and (Test-Path $r.structuredContent.result.file))

	# a read-only server next to it
	$ro = Start-Mcp @()
	$null = Send $ro 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'test'; version = '0' } }
	$r = Call $ro 'set_render_quality' @{ quality = 'high' }
	Check 'a read-only server refuses control tools' ($r.isError -and $r.content[0].text -like 'E_CONTROL_DISABLED*')
	$r = Call $ro 'get_vram_usage'
	Check 'a read-only server still reads (second client at the same time)' (-not $r.isError)

	# the runtime goes away
	$hostProc.WaitForExit(60000) | Out-Null
	$hostOut = Get-Content (Join-Path $data 'host-allow.txt') -Raw
	# (the host itself reports FAILED here: the two shader errors provoked above are in its log)
	Check 'the runtime shut down cleanly with clients connected' ($hostOut -match 'd3d12 errors: 0' -and $hostOut -match 'aether failed: no' -and $hostOut -match 'device references left: 0' -and $hostOut -match 'aether log errors: 2') $hostOut
	$r = Call $mcp 'get_status'
	Check 'a stopped game is reported, not hung on' ($r.isError -and ($r.content[0].text -like 'E_NOT_RUNNING*' -or $r.content[0].text -like 'E_DISCONNECTED*')) ($r.content[0].text)

	# ---------------------------------------------------------------- game side asks the player; nobody answers
	Write-Host 'runtime with mcpControl = ask (approval times out after 5 s)'
	$hostProc = Start-AetherHost 'ask' 16
	$r = Call $mcp 'get_status'
	Check 'the server reconnects to a new session' (-not $r.isError -and $r.structuredContent.mcpControl -eq 'ask') ($r.content[0].text)
	$sw = [System.Diagnostics.Stopwatch]::StartNew()
	$r = Call $mcp 'set_render_quality' @{ quality = 'ultra' }
	$s = Call $mcp 'get_settings'
	Check ("an unanswered approval is a refusal ({0:N1} s) and changes nothing" -f $sw.Elapsed.TotalSeconds) ($r.isError -and $r.content[0].text -like 'E_NOT_APPROVED*' -and $s.structuredContent.values.quality -ne 'ultra') ($r.content[0].text)
	$hostProc.WaitForExit(60000) | Out-Null
	$hostOut = Get-Content (Join-Path $data 'host-ask.txt') -Raw
	Check 'second session shut down cleanly (HOST OK)' ($hostOut -match 'HOST OK') $hostOut
} catch {
	Write-Host "  FAIL  exception: $($_.Exception.Message)"
	$script:failed++
} finally {
	foreach ($p in $mcp, $ro) { if ($p -and -not $p.HasExited) { $p.StandardInput.Close(); if (-not $p.WaitForExit(3000)) { $p.Kill() } } }
	if ($hostProc -and -not $hostProc.HasExited) { $hostProc.Kill() }
}
if ($Keep) { Write-Host "data: $data" } else { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue }
if ($script:failed -eq 0) { Write-Host 'MCP TEST OK'; exit 0 } else { Write-Host "MCP TEST FAILED ($script:failed)"; exit 1 }
