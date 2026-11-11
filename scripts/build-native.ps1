# Configures and builds the native runtime (AetherNative.dll) and the test host (AetherHost.exe).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1 [-Config RelWithDebInfo] [-Clean] [-UnityPluginApi <dir>]
#
# Output: build\native\<Config>\AetherNative.dll, AetherHost.exe. Needs Visual Studio's C++ tools, CMake and the Unity
# editor's PluginAPI headers (default: the 2023.2.20f1 editor from Unity Hub; override with -UnityPluginApi or AETHER_UNITY_PLUGIN_API).
# Exit code 0 on success; on failure the compiler's errors are printed and the exit code is 1.
param(
	[string]$Config = 'RelWithDebInfo',
	[switch]$Clean,
	[string]$UnityPluginApi = $env:AETHER_UNITY_PLUGIN_API
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033' # English compiler messages
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'src\Aether.Native'
$build = Join-Path $repo 'build\native'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
if (-not (Test-Path $cmake)) { Write-Host 'BUILD FAILED: cmake not found'; exit 1 }
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force $build | Out-Null
$log = Join-Path $build 'build.log'
Remove-Item -Force -ErrorAction SilentlyContinue $log

function Invoke-Logged([string]$what, [string]$command) {
	"=== $what ===" | Out-File -Append -Encoding utf8 $log
	cmd /c "$command >> `"$log`" 2>&1"
	return $LASTEXITCODE
}

$unity = if ($UnityPluginApi) { " -DUNITY_PLUGIN_API_DIR=`"$UnityPluginApi`"" } else { '' }
$code = 0
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
	$code = Invoke-Logged 'configure' "`"$cmake`" -S `"$source`" -B `"$build`" -G `"Visual Studio 18 2026`" -A x64$unity"
}
if ($code -eq 0) {
	$code = Invoke-Logged 'build' "`"$cmake`" --build `"$build`" --config $Config --parallel -- /nologo /verbosity:minimal"
}
if ($code -ne 0) {
	Write-Host 'BUILD FAILED. Errors:'
	$errors = Select-String -Path $log -Encoding Default -Pattern ' error |fatal error|CMake Error|LNK\d+' | ForEach-Object { $_.Line.Trim() } | Select-Object -Unique -First 40
	if ($errors) { $errors | ForEach-Object { Write-Host "  $_" } } else { Get-Content $log -Encoding Default -Tail 30 | ForEach-Object { Write-Host "  $_" } }
	exit 1
}
$warnings = @(Select-String -Path $log -Encoding Default -Pattern 'warning C\d+' | Where-Object { $_.Line -notmatch 'third_party' } | ForEach-Object { $_.Line.Trim() } | Select-Object -Unique)
Write-Host "BUILD OK ($Config): $build\$Config\AetherNative.dll, AetherHost.exe; $($warnings.Count) warning(s) in Aether's code"
$warnings | Select-Object -First 15 | ForEach-Object { Write-Host "  $_" }
exit 0
