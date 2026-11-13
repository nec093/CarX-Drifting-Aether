# Installs the built files into the game (game closed), or removes them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Loader BepInEx|Ksl] [-Config RelWithDebInfo] [-GameDir <dir>] [-Build] [-Remove]
#
# -Loader BepInEx (default): BepInEx 5 must be installed in the game folder (docs/development/build.md).
#   BepInEx\plugins\Aether\Aether.dll               the plugin (no signing)
#   BepInEx\plugins\Aether\native\AetherNative.dll  the native runtime (the plugin copies it to <Game>_Data\Plugins\x86_64 on start)
#   BepInEx\plugins\Aether\native\dxcompiler.dll, dxil.dll  DXC for the DXR shaders (the Windows SDK's redistributables)
#   BepInEx\plugins\Aether\shaders\*.hlsl, hdrp\aether_hdrp_dx12.bundle
#   Settings: BepInEx\config\Aether. A copy of Aether left in kino\mods (KSL) is removed so that it cannot start twice.
# -Loader Ksl: kino\mods\Aether.ksm (signed by tools\maykr.exe with the build key, else the raw Aether.dll) and
#   kino\mods\Aether\ for the native runtime, shaders and bundle. Settings: kino\config\Aether.
# -Build builds everything first. -Remove deletes Aether's files for that loader (settings are kept).
# Nothing of the game's own files is changed.
param(
	[ValidateSet('BepInEx', 'Ksl')][string]$Loader = 'BepInEx',
	[string]$Config = 'RelWithDebInfo',
	[string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online',
	[switch]$Build,
	[switch]$Remove
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (Get-Process | Where-Object { $_.ProcessName -like 'Drift Racing Online*' }) { Write-Host 'DEPLOY FAILED: the game is running (the DLLs are locked). Close it first.'; exit 1 }

$kslMods = Join-Path $GameDir 'kino\mods'
if ($Loader -eq 'BepInEx') {
	if (-not (Test-Path (Join-Path $GameDir 'BepInEx\core\BepInEx.dll'))) { Write-Host "DEPLOY FAILED: BepInEx is not installed in $GameDir (docs/development/build.md)"; exit 1 }
	$modData = Join-Path $GameDir 'BepInEx\plugins\Aether'
} else {
	if (-not (Test-Path $kslMods)) { Write-Host "DEPLOY FAILED: $kslMods not found (is KSL installed? pass -GameDir)"; exit 1 }
	$modData = Join-Path $kslMods 'Aether'
}
$plugin = Get-ChildItem $GameDir -Directory -Filter '*_Data' | ForEach-Object { Join-Path $_.FullName 'Plugins\x86_64\AetherNative.dll' } | Select-Object -First 1
$kslFiles = @((Join-Path $kslMods 'Aether.dll'), (Join-Path $kslMods 'Aether.ksm'))

if ($Remove) {
	$files = @($plugin)
	if ($Loader -eq 'Ksl') { $files += $kslFiles }
	foreach ($f in $files) { if ($f -and (Test-Path $f)) { Remove-Item -Force $f; Write-Host "removed $f" } }
	if (Test-Path $modData) { Remove-Item -Recurse -Force $modData; Write-Host "removed $modData" }
	Write-Host "Aether removed ($Loader; settings kept)"
	exit 0
}

if ($Build) {
	& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-native.ps1') -Config $Config
	if ($LASTEXITCODE -ne 0) { exit 1 }
	& dotnet build (Join-Path $repo 'src\Aether.Managed\Aether.Managed.csproj') -c Release -nologo -v q "-p:GameDir=$GameDir" "-p:Loader=$Loader"
	if ($LASTEXITCODE -ne 0) { Write-Host 'DEPLOY FAILED: the C# mod did not build'; exit 1 }
}

$native = Join-Path $repo "build\native\$Config\AetherNative.dll"
$managed = Join-Path $repo "build\managed\$Loader\Release\Aether.dll"
foreach ($f in $native, $managed) { if (-not (Test-Path $f)) { Write-Host "DEPLOY FAILED: $f not found (run with -Build)"; exit 1 } }

$nativeDir = Join-Path $modData 'native' # a sub-folder: the loaders look at the .dll files next to the mod as possible mods
New-Item -ItemType Directory -Force (Join-Path $modData 'shaders'), $nativeDir | Out-Null
Copy-Item -Force $native $nativeDir
$pdb = [System.IO.Path]::ChangeExtension($native, '.pdb')
if (Test-Path $pdb) { Copy-Item -Force $pdb $nativeDir }
# DXC (copied beside the build by CMake): without it the DXR option traces with the shader walk
foreach ($dxc in 'dxcompiler.dll', 'dxil.dll') {
	$from = Join-Path (Split-Path $native) $dxc
	if (Test-Path $from) { Copy-Item -Force $from $nativeDir } else { Write-Host "note: $dxc not in the build folder: DXR shaders will not compile" }
}
Copy-Item -Force (Join-Path $repo 'shaders\*') (Join-Path $modData 'shaders')
# HDRP's compute shaders for DX12 (scripts\build-hdrp-bundle.ps1): without them the game's own renderer shows nothing on DX12
$bundle = Join-Path $repo 'build\unity\aether_hdrp_dx12.bundle'
if (Test-Path $bundle) {
	New-Item -ItemType Directory -Force (Join-Path $modData 'hdrp') | Out-Null
	Copy-Item -Force $bundle (Join-Path $modData 'hdrp')
} elseif (-not (Test-Path (Join-Path $modData 'hdrp\aether_hdrp_dx12.bundle'))) {
	Write-Host 'WARNING: build\unity\aether_hdrp_dx12.bundle is missing (scripts\build-hdrp-bundle.ps1): the game picture stays black on DX12'
}

if ($Loader -eq 'BepInEx') {
	Copy-Item -Force $managed $modData
	foreach ($f in $kslFiles) { if (Test-Path $f) { Remove-Item -Force $f; Write-Host "removed the KSL copy $f (Aether must not be started by two loaders)" } }
	Write-Host "plugin: $modData\Aether.dll"
} else {
	$ksm = $kslFiles[1]
	$signed = (Test-Path $ksm) -and ((Get-Item $ksm).LastWriteTimeUtc -ge (Get-Item $managed).LastWriteTimeUtc)
	if ($signed) {
		Remove-Item -Force -ErrorAction SilentlyContinue $kslFiles[0] # one copy of the mod only
		Write-Host "mod: $ksm (signed)"
	} else {
		Remove-Item -Force -ErrorAction SilentlyContinue $ksm
		Copy-Item -Force $managed $kslMods
		Write-Host "mod: $kslMods\Aether.dll (RAW dll: KSL loads it only for accounts allowed to load raw dlls; build the .ksm with maykr and the build key)"
	}
}
Write-Host "native runtime and shaders: $modData"
Write-Host 'DEPLOY OK'
exit 0
