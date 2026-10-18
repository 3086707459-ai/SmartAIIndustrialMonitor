[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$CompilerBin,
    [ValidateRange(1, 1800)][int]$StressSeconds = 60,
    [string]$EvidenceDirectory = 'build/acceptance/ci'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtPath = (Resolve-Path -LiteralPath $QtRoot).Path
$compilerPath = (Resolve-Path -LiteralPath $CompilerBin).Path
$compiler = Join-Path $compilerPath 'g++.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "Missing compiler: $compiler" }
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory, $projectRoot)
if (Test-Path -LiteralPath $evidence) { throw "Use a fresh evidence directory: $evidence" }
$previousPath = $env:PATH
$previousPrefix = $env:CMAKE_PREFIX_PATH
$previousCompiler = $env:CXX
$transcriptStarted = $false
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Path $evidence | Out-Null
    Start-Transcript -Path (Join-Path $evidence 'ci.log') | Out-Null
    $transcriptStarted = $true
    $env:PATH = "$(Join-Path $qtPath 'bin');$compilerPath;$previousPath"
    $env:CMAKE_PREFIX_PATH = $qtPath
    # CXX selects the compiler for a fresh checkout without forcing CMake to
    # discard an existing cache and silently reset preset options to defaults.
    $env:CXX = $compiler.Replace('\', '/')
    & cmake --preset demo
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    $cache = Get-Content -LiteralPath 'build/demo/CMakeCache.txt' -Raw
    foreach ($expected in @('SAIM_WARNINGS_AS_ERRORS:BOOL=ON', 'SAIM_WITH_ONNX:BOOL=OFF', 'SAIM_WITH_OPENCV:BOOL=OFF', 'CMAKE_BUILD_TYPE:STRING=Release')) {
        if ($cache -notmatch ('(?m)^' + [regex]::Escape($expected) + '\r?$')) {
            throw "Unexpected CMake cache; required: $expected"
        }
    }
    & cmake --build --preset demo
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    & ctest --preset demo --output-junit (Join-Path $evidence 'regression.xml')
    if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
    & (Join-Path $PSScriptRoot 'stress.ps1') -QtRoot $qtPath -CompilerBin $compilerPath -DurationSeconds $StressSeconds -OutputDirectory (Join-Path $evidence 'stress') -SkipBuild
    & (Join-Path $PSScriptRoot 'package.ps1') -QtRoot $qtPath -CompilerBin $compilerPath -OutputDirectory (Join-Path $projectRoot 'build/packages/ci') -SkipBuild
    & (Join-Path $PSScriptRoot 'verify-package.ps1') -PackageDirectory (Join-Path $projectRoot 'build/packages/ci') -EvidenceDirectory (Join-Path $evidence 'deployment')
    Write-Output 'All CI acceptance stages passed.'
} finally {
    $env:PATH = $previousPath
    $env:CMAKE_PREFIX_PATH = $previousPrefix
    $env:CXX = $previousCompiler
    if ($transcriptStarted) { Stop-Transcript | Out-Null }
    Pop-Location
}
