[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$CompilerBin,
    [string]$OutputDirectory,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$qtDirectory = (Resolve-Path -LiteralPath $QtRoot).Path
$compilerDirectory = (Resolve-Path -LiteralPath $CompilerBin).Path
$deployTool = Join-Path $qtDirectory 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployTool)) { throw "Missing deployment tool: $deployTool" }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('build/packages/MedSignal-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$destination = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite an existing package: $destination" }
$previousPath = $env:PATH
$previousPrefix = $env:CMAKE_PREFIX_PATH
try {
    $env:PATH = "$(Join-Path $qtDirectory 'bin');$compilerDirectory;$previousPath"
    $env:CMAKE_PREFIX_PATH = $qtDirectory
    Push-Location $projectRoot
    try {
        if (-not $SkipBuild) {
            & cmake --preset demo
            if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
            & cmake --build --preset demo --target MedSignalWorkbench bidmc_benchmark saim_deployment_probe
            if ($LASTEXITCODE -ne 0) { throw 'Package build failed' }
        }
        $cachePath = Join-Path $projectRoot 'build/demo/CMakeCache.txt'
        $cache = Get-Content -LiteralPath $cachePath -Raw
        foreach ($option in @('SAIM_WITH_ONNX', 'SAIM_WITH_OPENCV')) {
            if ($cache -notmatch "(?m)^${option}:BOOL=OFF\r?$") { throw "Demo package requires $option=OFF" }
        }
        if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'Demo package requires a Release build' }
        New-Item -ItemType Directory -Path $destination | Out-Null
        foreach ($name in @('MedSignalWorkbench.exe', 'bidmc_benchmark.exe', 'saim_deployment_probe.exe')) {
            Copy-Item -LiteralPath (Join-Path $projectRoot "build/demo/bin/$name") -Destination $destination
        }
        & $deployTool --release --compiler-runtime --no-translations --no-system-d3d-compiler --dir $destination (Join-Path $destination 'MedSignalWorkbench.exe') (Join-Path $destination 'bidmc_benchmark.exe') (Join-Path $destination 'saim_deployment_probe.exe')
        if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
        # Explicit app-local paths also override Qt's compiled-in SDK prefix.
        "[Paths]`nPrefix=.`nPlugins=.`nLibraries=.`n" | Set-Content -LiteralPath (Join-Path $destination 'qt.conf') -Encoding ascii
        New-Item -ItemType Directory -Path (Join-Path $destination 'datasets') | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot 'datasets/bidmc-demo') -Destination (Join-Path $destination 'datasets') -Recurse
        Copy-Item -LiteralPath (Join-Path $projectRoot 'datasets/README.md') -Destination (Join-Path $destination 'datasets')
        Copy-Item -LiteralPath (Join-Path $projectRoot 'datasets/SHA256SUMS-bidmc-1.0.0.txt') -Destination (Join-Path $destination 'datasets')
        Copy-Item -LiteralPath (Join-Path $projectRoot 'docs/package-readme.md') -Destination (Join-Path $destination 'README.md')
        New-Item -ItemType Directory -Path (Join-Path $destination 'docs/acceptance') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot 'docs/medical-validation.md') -Destination (Join-Path $destination 'docs')
        foreach ($evidenceName in @('bidmc-20261026.json', 'medical-20261026-run-info.json', 'regression-20261026.xml')) {
            Copy-Item -LiteralPath (Join-Path $projectRoot "docs/acceptance/$evidenceName") -Destination (Join-Path $destination 'docs/acceptance')
        }
        New-Item -ItemType Directory -Path (Join-Path $destination 'acceptance') | Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify-package.ps1') -Destination (Join-Path $destination 'acceptance')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify-in-sandbox.ps1') -Destination (Join-Path $destination 'acceptance')
        @{
            schemaVersion = 1
            product = 'MedSignalWorkbench'
            buildType = 'Release'
            profile = 'medical replay: original BIDMC record 01 + fixed RESP baseline; no clinical model'
            qtVersion = (& (Join-Path $qtDirectory 'bin/qmake.exe') -query QT_VERSION)
            builtAtUtc = [DateTime]::UtcNow.ToString('o')
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'package-info.json') -Encoding utf8
        $files = @(Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
            @{
                path = $_.FullName.Substring($destination.Length + 1).Replace('\', '/')
                bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
        @{ schemaVersion = 1; files = $files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.sha256.json') -Encoding utf8
        $archive = $destination + '.zip'
        if (Test-Path -LiteralPath $archive) { throw "Archive already exists: $archive" }
        Compress-Archive -LiteralPath $destination -DestinationPath $archive -CompressionLevel Optimal
        Write-Output "Package: $destination"
        Write-Output "Archive: $archive"
        Write-Output "Archive SHA256: $((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)"
    } finally { Pop-Location }
} finally {
    $env:PATH = $previousPath
    $env:CMAKE_PREFIX_PATH = $previousPrefix
}
