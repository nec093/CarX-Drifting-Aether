# Calls one tool of the Aether MCP server from the command line (the same path an AI agent uses) and prints the result.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mcp-call.ps1 get_vram_usage
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mcp-call.ps1 get_logs minSeverity=warning limit=20
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mcp-call.ps1 run_benchmark seconds=10 label=garage -AllowControl
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mcp-call.ps1 capture_frame -AllowControl
#
# Arguments are key=value pairs (numbers and true / false are sent as such) or one JSON object.
# -Raw prints the tool's JSON on one line (for scripts); otherwise it is indented. Exit code 1 when the tool reports an error.
param(
	[Parameter(Mandatory = $true, Position = 0)][string]$Tool,
	[Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Arguments = @(),
	[switch]$AllowControl,
	[switch]$Raw,
	[int]$TimeoutSeconds = 400
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\mcp\Release\Aether.MCP.exe'
if (-not (Test-Path $exe)) { Write-Host 'Aether.MCP.exe not found: dotnet build src\Aether.MCP -c Release'; exit 1 }
$json = '{}'
if ($Arguments.Count -gt 0) {
	if ($Arguments[0].TrimStart().StartsWith('{')) {
		$json = $Arguments -join ' '
	} else {
		$map = [ordered]@{}
		foreach ($pair in $Arguments) {
			$i = $pair.IndexOf('=')
			if ($i -lt 1) { Write-Host "not a key=value argument: $pair"; exit 1 }
			$value = $pair.Substring($i + 1)
			$number = 0L
			$real = 0.0
			$map[$pair.Substring(0, $i)] = if ($value -eq 'true') { $true } elseif ($value -eq 'false') { $false } elseif ([long]::TryParse($value, [ref]$number)) { $number } elseif ([double]::TryParse($value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$real)) { $real } else { $value }
		}
		$json = $map | ConvertTo-Json -Compress
	}
}
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = if ($AllowControl) { '--allow-control' } else { '' }
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$p = [System.Diagnostics.Process]::Start($psi)
$p.BeginErrorReadLine()
function Send([string]$line) {
	$bytes = [System.Text.Encoding]::UTF8.GetBytes($line + "`n")
	$p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
	$p.StandardInput.BaseStream.Flush()
	$task = $p.StandardOutput.ReadLineAsync()
	if (-not $task.Wait($TimeoutSeconds * 1000)) { throw 'the MCP server did not answer' }
	return $task.Result
}
try {
	$null = Send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp-call","version":"0"}}}'
	$answer = Send ('{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"' + $Tool + '","arguments":' + $json + '}}')
	$parsed = $answer | ConvertFrom-Json
	if ($parsed.error) { Write-Host "MCP error $($parsed.error.code): $($parsed.error.message)"; exit 1 }
	$result = $parsed.result
	$text = ($result.content | Where-Object type -eq 'text' | Select-Object -First 1).text
	if ($result.isError) { Write-Host $text; exit 1 }
	if ($Raw) { Write-Output $text } else { Write-Output ($text | ConvertFrom-Json | ConvertTo-Json -Depth 12) }
	exit 0
} finally {
	if (-not $p.HasExited) { $p.StandardInput.Close(); if (-not $p.WaitForExit(3000)) { $p.Kill() } }
}
