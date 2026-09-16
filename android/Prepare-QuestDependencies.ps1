# Fetches the one dependency the Gradle project cannot resolve from a Maven
# repository: the SDL3 Android AAR (SDLActivity plus the matching libSDL3.so).
# Dawn, the OpenXR loader and every other native dependency are fetched and
# pinned by the runtime's CMake during the Gradle build.
#
# Usage: powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1
#
# The AAR version must equal AURORA_SDL3_VERSION in aurora-main/CMakeLists.txt:
# the Java classes in the AAR and libSDL3.so must come from the same release.
$ErrorActionPreference = 'Stop'

$sdlVersion = '3.4.4'
$sdlArchiveSha256 = 'da67b5a43442e449511399c65aa86b724419f92850cf36a2a8c7de72eb992bc0'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$libs = Join-Path $root 'app\libs'
$cache = Join-Path $root '.dependencies'
New-Item -ItemType Directory -Force $libs, $cache | Out-Null

$archive = Join-Path $cache "SDL3-devel-$sdlVersion-android.zip"
if (-not (Test-Path $archive)) {
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVersion/SDL3-devel-$sdlVersion-android.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $archive
}
$actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
if ($actual -ne $sdlArchiveSha256) {
    Remove-Item $archive
    throw "SDL3 Android archive digest mismatch: $actual"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entry = $zip.Entries | Where-Object { $_.Name -eq "SDL3-$sdlVersion.aar" } | Select-Object -First 1
    if (-not $entry) { throw "SDL3-$sdlVersion.aar not found in the archive" }
    $target = Join-Path $libs "SDL3-$sdlVersion.aar"
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    Write-Host "SDL3 AAR staged at $target"
}
finally {
    $zip.Dispose()
}
