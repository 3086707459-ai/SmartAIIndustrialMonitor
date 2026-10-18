param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$CompilerBin
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtBin = Join-Path (Resolve-Path -LiteralPath $QtRoot) 'bin'
$compilerPath = (Resolve-Path -LiteralPath $CompilerBin).Path
$previousPath = $env:PATH
$previousPrefix = $env:CMAKE_PREFIX_PATH
try {
    $env:PATH = "$qtBin;$compilerPath;$previousPath"
    $env:CMAKE_PREFIX_PATH = (Resolve-Path -LiteralPath $QtRoot).Path
    Push-Location $projectRoot
    try {
        & cmake --preset demo
        if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
        & cmake --build --preset demo
        if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
        & ctest --preset demo
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    } finally { Pop-Location }
} finally {
    $env:PATH = $previousPath
    $env:CMAKE_PREFIX_PATH = $previousPrefix
}
