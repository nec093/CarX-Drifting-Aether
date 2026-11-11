# Installs the built files into the game (game closed), or removes them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Config RelWithDebInfo] [-GameDir <dir>] [-Build] [-Remove]
#
#   kino\mods\Aether.ksm                     the signed KSL mod: built straight into kino\mods by the C# project's Release
#                                            build when tools\maykr.exe and the build key (kino\dev\Aether_maykr.kmc) exist.
#                                            Without them the raw Aether.dll is copied, which KSL only loads for accounts
#                                            allowed to load raw dlls.
#   kino\mods\Aether\native\AetherNative.dll the native runtime (the mod copies it to <Game>_Data\Plugins\x86_64 on start)
#   kino\mods\Aether\shaders\*.hlsl
# -Build builds everything first. -Remove deletes Aether's files (settings in kino\config\Aether are kept).
# Nothing of the game's own files is changed.
param(
	[string]$Config = 'RelWithDebInfo',
	[string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online',
	[switch]$Build,
	[switch]$Remove
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $GameDir 'kino\mods'))) { Write-Host "DEPLOY FAILED: $GameDir\kino\mods not found (is KSL installed? pass -GameDir)"; exit 1 }
if (Get-Process | Where-Object { $_.ProcessName -like 'Drift Racing Online*' }) { Write-Host 'DEPLOY FAILED: the game is running (the DLLs are locked). Close it first.'; exit 1 }

$mods = Join-Path $GameDir 'kino\mods'
$modData = Join-Path $mods 'Aether'
$plugin = Get-ChildItem $GameDir -Directory -Filter '*_Data' | ForEach-Object { Join-Path $_.FullName 'Plugins\x86_64\AetherNative.dll' } | Select-Object -First 1

if ($Remove) {
	foreach ($f in (Join-Path $mods 'Aether.dll'), (Join-Path $mods 'Aether.ksm'), $plugin) {
		if ($f -and (Test-Path $f)) { Remove-Item -Force $f; Write-Host "removed $f" }
	}
	if (Test-Path $modData) { Remove-Item -Recurse -Force $modData; Write-Host "removed $modData" }
	Write-Host 'Aether removed (settings kept in kino\config\Aether)'
	exit 0
}

if ($Build) {
	& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-native.ps1') -Config $Config
	if ($LASTEXITCODE -ne 0) { exit 1 }
	& dotnet build (Join-Path $repo 'src\Aether.Managed\Aether.Managed.csproj') -c Release -nologo -v q "-p:GameDir=$GameDir"
	if ($LASTEXITCODE -ne 0) { Write-Host 'DEPLOY FAILED: the C# mod did not build'; exit 1 }
}

$native = Join-Path $repo "build\native\$Config\AetherNative.dll"
$managed = Join-Path $repo 'build\managed\Release\Aether.dll'
foreach ($f in $native, $managed) { if (-not (Test-Path $f)) { Write-Host "DEPLOY FAILED: $f not found (run with -Build)"; exit 1 } }

$nativeDir = Join-Path $modData 'native' # a sub-folder: KSL examines every .dll directly inside kino\mods\<folder>
New-Item -ItemType Directory -Force (Join-Path $modData 'shaders'), $nativeDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $modData 'AetherNative.dll'), (Join-Path $modData 'AetherNative.pdb')
Copy-Item -Force $native $nativeDir
$pdb = [System.IO.Path]::ChangeExtension($native, '.pdb')
if (Test-Path $pdb) { Copy-Item -Force $pdb $nativeDir }
Copy-Item -Force (Join-Path $repo 'shaders\*') (Join-Path $modData 'shaders')
# HDRP's compute shaders for DX12 (scripts\build-hdrp-bundle.ps1): without them the game's own renderer shows nothing on DX12
$bundle = Join-Path $repo 'build\unity\aether_hdrp_dx12.bundle'
if (Test-Path $bundle) {
	New-Item -ItemType Directory -Force (Join-Path $modData 'hdrp') | Out-Null
	Copy-Item -Force $bundle (Join-Path $modData 'hdrp')
} elseif (-not (Test-Path (Join-Path $modData 'hdrp\aether_hdrp_dx12.bundle'))) {
	Write-Host 'WARNING: build\unity\aether_hdrp_dx12.bundle is missing (scripts\build-hdrp-bundle.ps1): the game picture stays black on DX12'
}
$ksm = Join-Path $mods 'Aether.ksm'
$signed = (Test-Path $ksm) -and ((Get-Item $ksm).LastWriteTimeUtc -ge (Get-Item $managed).LastWriteTimeUtc)
if ($signed) {
	Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $mods 'Aether.dll') # one copy of the mod only
	Write-Host "mod: $ksm (signed)"
} else {
	Remove-Item -Force -ErrorAction SilentlyContinue $ksm
	Copy-Item -Force $managed $mods
	Write-Host "mod: $mods\Aether.dll (RAW dll: KSL loads it only for accounts allowed to load raw dlls; build the .ksm with maykr and the build key)"
}
Write-Host "native runtime and shaders: $modData"
Write-Host 'DEPLOY OK'
exit 0
