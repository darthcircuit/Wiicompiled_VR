# Builds the pinned Dawn DLL with Aurora's native Windows Vulkan/OpenXR bridge.
# Run on a maintainer machine with VS 2022 C++ tools, CMake, Git and Python 3.
[CmdletBinding()]
param(
    [string]$WorkDirectory = (Join-Path $PSScriptRoot 'artifacts\dawn-vulkan-build'),
    [string]$Destination = (Join-Path $PSScriptRoot 'artifacts\dawn-vulkan'),
    [string]$Python = 'python',
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$revision = '13abc3bc8ea2d3c2050f9e77a12d012108ceee24'
$archiveHash = '713bea5b92d4f6c5175752fd7cbf1c3c5ce36598ff5dd98685d8a1216614ebba'
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$Destination = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null
$archive = Join-Path $WorkDirectory 'dawn-source.tar.gz'
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest "https://github.com/google/dawn/archive/$revision.tar.gz" -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash) {
    throw 'Dawn source archive does not match the pinned SHA-256.'
}
$source = Join-Path $WorkDirectory "dawn-$revision"
if (-not (Test-Path -LiteralPath $source)) {
    # Single quotes inside: Windows PowerShell drops the inner double quotes when it builds a
    # native command line, and Python then reads filter=data as a name.
    & $Python -c "import sys,tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter='data')" $archive $WorkDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Dawn source extraction failed (Python 3.12+ required).' }
}
$patch = Join-Path $PSScriptRoot '..\aurora-main\patches\dawn\apply.py'
& $Python $patch $source
if ($LASTEXITCODE -ne 0) { throw 'Applying the Aurora Dawn bridge failed.' }
$build = Join-Path $WorkDirectory 'build'
& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 `
    "-DPython3_EXECUTABLE=$Python" -DDAWN_FETCH_DEPENDENCIES=ON `
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED -DDAWN_ENABLE_INSTALL=ON `
    -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF `
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_USE_GLFW=OFF `
    -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=ON -DDAWN_ENABLE_VULKAN=ON `
    -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF "-DCMAKE_INSTALL_PREFIX=$Destination"
if ($LASTEXITCODE -ne 0) { throw 'Dawn configuration failed.' }
& cmake --build $build --config Release --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Dawn build failed.' }
& cmake --install $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Dawn installation failed.' }
$dll = Join-Path $Destination 'bin\webgpu_dawn.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Dawn DLL missing: $dll" }
[ordered]@{
    SourceRevision = $revision
    AuroraVulkanAbi = 1
    DllSha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'aurora-vulkan.json') -Encoding UTF8
Write-Host "Custom Dawn package ready: $Destination"
