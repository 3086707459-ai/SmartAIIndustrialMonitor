param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$CompilerBin,
    [ValidateRange(1, 86400)][int]$DurationSeconds = 1800,
    [ValidateRange(1, 256)][int]$DeviceCount = 16,
    [ValidateRange(1, 10000)][int]$IntervalMs = 20,
    [ValidateRange(0, 3600)][int]$StallEverySeconds = 30,
    [ValidateRange(0, 1000)][int]$StallMs = 250,
    [ValidateRange(30, 600)][int]$ShutdownGraceSeconds = 120,
    [string]$OutputDirectory,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('build/acceptance/stress-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$previousPath = $env:PATH
$previousPrefix = $env:CMAKE_PREFIX_PATH
try {
    $qtPath = (Resolve-Path -LiteralPath $QtRoot).Path
    $compilerPath = (Resolve-Path -LiteralPath $CompilerBin).Path
    $env:PATH = "$(Join-Path $qtPath 'bin');$compilerPath;$previousPath"
    $env:CMAKE_PREFIX_PATH = $qtPath
    Push-Location $projectRoot
    try {
        if (-not $SkipBuild) {
            & cmake --preset demo
            if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
            & cmake --build --preset demo --target pipeline_stress
            if ($LASTEXITCODE -ne 0) { throw 'Stress executable compilation failed' }
        }
        if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory for each acceptance run' }
        New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
        # Keep the exact tested executable even if the build tree changes later.
        $runner = Join-Path $OutputDirectory 'pipeline_stress.exe'
        Copy-Item -LiteralPath (Join-Path $projectRoot 'build/demo/bin/pipeline_stress.exe') -Destination $runner
        @{
            executableSha256 = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash
            startedAtUtc = [DateTime]::UtcNow.ToString('o')
            durationSeconds = $DurationSeconds
            deviceCount = $DeviceCount
            intervalMs = $IntervalMs
            processor = $env:PROCESSOR_IDENTIFIER
            logicalProcessors = [Environment]::ProcessorCount
            operatingSystem = [Environment]::OSVersion.VersionString
            sourceHashes = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Recurse -File | ForEach-Object {
                @{ path = $_.FullName.Substring($projectRoot.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
            })
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run-info.json') -Encoding utf8
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $runner
        $start.Arguments = "--duration $DurationSeconds --devices $DeviceCount --interval-ms $IntervalMs --stall-every $StallEverySeconds --stall-ms $StallMs --output `"$OutputDirectory`""
        $start.WorkingDirectory = $projectRoot
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $start
        if (-not $process.Start()) { throw 'Cannot launch stress executable' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try {
            while (-not $process.WaitForExit(10000)) {
                $timeline = Join-Path $OutputDirectory 'timeline.jsonl'
                if (Test-Path -LiteralPath $timeline) {
                    $point = Get-Content -LiteralPath $timeline -Tail 1 | ConvertFrom-Json
                    Write-Output "elapsed=$([int]($point.elapsed_ms / 1000))s received=$($point.received) written=$($point.written) storage_queue=$($point.persistence.depth)"
                }
                if ($watch.Elapsed.TotalSeconds -gt ($DurationSeconds + $ShutdownGraceSeconds)) {
                    # External test watchdog only; production worker threads are
                    # never terminated. A timeout is failure, never a pass.
                    $process.Kill()
                    $process.WaitForExit()
                    @{ passed=$false; watchdogTimeout=$true; elapsedSeconds=$watch.Elapsed.TotalSeconds } |
                        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'watchdog-failure.json')
                    throw 'Stress process exceeded workload plus shutdown deadline'
                }
            }
            if ($process.ExitCode -ne 0) { throw "Stress acceptance failed; inspect $OutputDirectory" }
        } finally {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $stdout.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory 'console.log') -Encoding utf8
            $stderr.Result | Set-Content -LiteralPath (Join-Path $OutputDirectory 'stderr.log') -Encoding utf8
            $process.Dispose()
        }
        Write-Output "Acceptance report: $(Join-Path $OutputDirectory 'report.json')"
    } finally { Pop-Location }
} finally {
    $env:PATH = $previousPath
    $env:CMAKE_PREFIX_PATH = $previousPrefix
}
