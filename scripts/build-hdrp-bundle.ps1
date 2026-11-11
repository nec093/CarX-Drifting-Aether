# Builds build\unity\aether_hdrp_dx12.bundle: HDRP's runtime compute shaders compiled for Direct3D 12 (the game ships
# them without DX12 kernels; see unity\Assets\Aether\Editor\BuildHdrpDx12Bundle.cs). Only needed once per HDRP version.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-hdrp-bundle.ps1 [-Project <dir>] [-Unity <Unity.exe>]
#
# The Unity project lives outside the repository (default %USERPROFILE%\Aether-Unity: Documents is usually under
# Windows Controlled Folder Access, which stops Unity from writing there). It is created on the first run (Unity
# 2023.2.x with the HDRP package that ships inside the editor: nothing is downloaded); that first run imports HDRP and
# takes a long time. Log: <project>\Logs\aether_build.log. Exit code 0 on success.
param(
	[string]$Project = (Join-Path $env:USERPROFILE 'Aether-Unity'),
	[string]$Unity = 'C:\Program Files\Unity\Hub\Editor\2023.2.20f1\Editor\Unity.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $Unity)) { Write-Host "BUILD FAILED: Unity editor not found at $Unity (pass -Unity; 2023.2.x)"; exit 1 }

if (-not (Test-Path (Join-Path $Project 'Packages\manifest.json'))) {
	New-Item -ItemType Directory -Force (Join-Path $Project 'Packages'), (Join-Path $Project 'Assets'), (Join-Path $Project 'ProjectSettings') | Out-Null
	@'
{
  "dependencies": {
    "com.unity.render-pipelines.high-definition": "16.0.6",
    "com.unity.modules.assetbundle": "1.0.0",
    "com.unity.modules.imageconversion": "1.0.0",
    "com.unity.modules.imgui": "1.0.0",
    "com.unity.modules.jsonserialize": "1.0.0",
    "com.unity.modules.physics": "1.0.0",
    "com.unity.modules.terrain": "1.0.0",
    "com.unity.modules.ui": "1.0.0"
  }
}
'@ | Set-Content -Encoding ascii (Join-Path $Project 'Packages\manifest.json')
	Write-Host "created the Unity project $Project (the first build imports HDRP: expect a long run)"
}

# the editor script (the project keeps what Unity generates next to it)
$target = Join-Path $Project 'Assets\Aether\Editor'
New-Item -ItemType Directory -Force $target | Out-Null
Copy-Item -Force (Join-Path $repo 'unity\Assets\Aether\Editor\*.cs') $target
# the game's HDRP settings the variants are built for
Copy-Item -Force (Join-Path $repo 'unity\hdrp-game-settings.json') (Join-Path $Project 'Assets\Aether\hdrp-game-settings.json')

$out = Join-Path $Project 'Build\Aether'
$log = Join-Path $Project 'Logs\aether_build.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
Write-Host "building (log: $log) ..."
$p = Start-Process -FilePath $Unity -ArgumentList @('-batchmode', '-nographics', '-projectPath', "`"$Project`"", '-executeMethod', 'Aether.Editor.BuildHdrpDx12Bundle.BatchBuild', '-aetherOut', "`"$out`"", '-logFile', "`"$log`"", '-quit') -PassThru -Wait
if (Test-Path $log) { Select-String -Path $log -Pattern '\[Aether\]|error CS|Shader error|Build.*failed' | Select-Object -Last 20 | ForEach-Object { Write-Host "  $($_.Line)" } }
$bundle = Join-Path $out 'aether_hdrp_dx12.bundle'
if ($p.ExitCode -ne 0 -or -not (Test-Path $bundle)) { Write-Host "BUILD FAILED (Unity exit code $($p.ExitCode)); see $log"; exit 1 }
$dest = Join-Path $repo 'build\unity'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Force $bundle $dest
Write-Host "BUILD OK: $dest\aether_hdrp_dx12.bundle ($([math]::Round((Get-Item $bundle).Length / 1KB)) KB). scripts\deploy.ps1 installs it."
exit 0
