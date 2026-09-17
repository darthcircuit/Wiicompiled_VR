# Exports the Quest game kit (see QuestGameKit.psm1) from a configured and built Android CMake
# tree. android/app/build.gradle.kts runs this after the native build and packages the result as
# the APK's game_kit assets.
#
#   powershell -ExecutionPolicy Bypass -File android/Export-QuestGameKit.ps1 -CMakeBinaryDir <dir> -OutputDir <dir> -LlvmStrip <llvm-strip>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$CMakeBinaryDir,
    [Parameter(Mandatory)] [string]$OutputDir,
    [Parameter(Mandatory)] [string]$LlvmStrip
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'QuestGameKit.psm1') -Force
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fingerprint = Export-QuestGameKit -CMakeBinaryDir $CMakeBinaryDir -OutputDir $OutputDir -RepoRoot $repo -LlvmStrip $LlvmStrip
Write-Host "Quest game kit $fingerprint -> $OutputDir"
