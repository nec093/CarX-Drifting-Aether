# Downloads the libraries that are not committed into build\deps (the native build finds them there):
#   NRD v4.18.0 (NVIDIA Real-time Denoisers: the reflection denoiser), NVIDIA RTX SDKs License: object code only, so
#     its source stays out of the repository. Running this means accepting that license (build\deps\NRD-4.18.0\LICENSE.txt).
#   MathLib v11 and ShaderMake (NRD's build dependencies, MIT), at the versions NRD's CMakeLists.txt names.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1
#
# NRD's shaders are compiled with the Windows SDK's DXC (src\Aether.Native\CMakeLists.txt): nothing else is downloaded.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $repo 'build\deps'
$downloads = Join-Path $deps 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
$items = @(
	@{ Name = 'NRD-4.18.0'; Url = 'https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v4.18.0.zip'; File = 'NRD-4.18.0.zip' },
	@{ Name = 'MathLib-11'; Url = 'https://github.com/NVIDIA-RTX/MathLib/archive/refs/tags/v11.zip'; File = 'MathLib-v11.zip' },
	@{ Name = 'ShaderMake-3f01623a76b092ac127d2debd79a672f55d77665'; Url = 'https://github.com/NVIDIA-RTX/ShaderMake/archive/3f01623a76b092ac127d2debd79a672f55d77665.zip'; File = 'ShaderMake.zip' }
)
foreach ($item in $items) {
	$target = Join-Path $deps $item.Name
	if (Test-Path $target) { Write-Host "have $($item.Name)"; continue }
	$zip = Join-Path $downloads $item.File
	Write-Host "downloading $($item.Url)"
	Invoke-WebRequest -UseBasicParsing -Uri $item.Url -OutFile $zip
	Expand-Archive -Path $zip -DestinationPath $deps -Force
	if (-not (Test-Path $target)) { Write-Host "FETCH FAILED: $($item.Name) not in the archive"; exit 1 }
}
Write-Host "FETCH OK: $deps"
