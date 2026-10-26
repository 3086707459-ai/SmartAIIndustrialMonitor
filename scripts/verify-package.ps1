[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [string]$EvidenceDirectory,
    [ValidateRange(10, 120)][int]$TimeoutSeconds = 40
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\', '/')
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('saim-acceptance-' + [Guid]::NewGuid().ToString('N'))
}
$evidence = [System.IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $evidence) { throw "Refusing to overwrite existing evidence: $evidence" }
if ($evidence.StartsWith($package + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Evidence must be outside the immutable package directory'
}
New-Item -ItemType Directory -Path $evidence | Out-Null
$runRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('saim-runtime-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.sha256.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or @($manifest.files).Count -eq 0) { throw 'Invalid package manifest' }
$seen = @{}
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.path
    $file = [System.IO.Path]::GetFullPath((Join-Path $package $relative))
    if (-not $file.StartsWith($package + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Manifest path escapes package: $relative" }
    if ($seen.ContainsKey($relative)) { throw "Duplicate manifest entry: $relative" }
    $seen[$relative] = $true
    $item = Get-Item -LiteralPath $file
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw "Package link is not allowed: $relative" }
    if ($item.Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Package integrity mismatch: $relative" }
}
$actual = @(Get-ChildItem -LiteralPath $package -File -Recurse)
if ($actual.Count -ne (@($manifest.files).Count + 1)) { throw 'Package contains unmanifested files' }
foreach ($item in Get-ChildItem -LiteralPath $package -Recurse) {
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Package contains a filesystem link' }
}
Get-ChildItem -LiteralPath $package | Copy-Item -Destination $runRoot -Recurse

function Invoke-IsolatedProcess([string]$Name, [string]$Arguments, [string]$LogName) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $runRoot $Name
    $start.Arguments = $Arguments
    $start.WorkingDirectory = $runRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($key in @($start.EnvironmentVariables.Keys)) {
        if ($key -match '^(QT|QML|SAIM|CMAKE|VCPKG|CONAN)') { $start.EnvironmentVariables.Remove($key) }
    }
    $start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
    $start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
    $start.EnvironmentVariables['QT_DEBUG_PLUGINS'] = '1'
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Cannot launch $Name" }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Acceptance timed out: $Name"
    }
    $stdout.Result | Set-Content -LiteralPath (Join-Path $evidence "$LogName.stdout.log") -Encoding utf8
    $stderr.Result | Set-Content -LiteralPath (Join-Path $evidence "$LogName.stderr.log") -Encoding utf8
    if ($process.ExitCode -ne 0) { throw "$Name exited with $($process.ExitCode); see $evidence" }
    $process.Dispose()
}

# The shipped entry point must load its dependencies and command parser, too.
Invoke-IsolatedProcess 'MedSignalWorkbench.exe' '--version' 'application'
Invoke-IsolatedProcess 'saim_deployment_probe.exe' ('--evidence-dir "' + $evidence + '"') 'probe'
$result = Get-Content -LiteralPath (Join-Path $evidence 'probe-result.json') -Raw | ConvertFrom-Json
if (-not $result.ok -or $result.platform -ne 'windows' -or -not $result.guiVisible -or -not $result.svgRendered -or -not $result.missingValuePreserved -or $result.chartPoints -ne 3753 -or $result.waveformRows -ne 60001 -or $result.numericRows -ne 481) {
    throw "Deployment probe did not meet acceptance conditions: $evidence"
}
$runtimePrefix = $runRoot.TrimEnd('\') + '\'
$runtimeModules = @($result.loadedModules | Where-Object { [System.IO.Path]::GetFileName($_) -match '^(Qt6|qwindows|qsqlite|qsvg|libgcc|libstdc\+\+|libwinpthread)' })
if ($runtimeModules.Count -lt 5) { throw 'Loaded-module evidence is incomplete' }
foreach ($module in $runtimeModules) {
    $modulePath = [System.IO.Path]::GetFullPath($module)
    if (-not $modulePath.StartsWith($runtimePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Dependency escaped package: $module" }
}
foreach ($required in @('qwindows.dll', 'Qt6Charts.dll', 'Qt6Svg.dll', 'Qt6Core.dll')) {
    if (-not @($runtimeModules | Where-Object { [System.IO.Path]::GetFileName($_) -eq $required }).Count) { throw "Required module was not loaded: $required" }
}
$sandboxPath = Join-Path $env:SystemRoot 'System32/WindowsSandbox.exe'
$acceptance = @{
    schemaVersion = 1
    ok = $true
    verification = 'local process isolation; not a clean VM certification'
    testedAtUtc = [DateTime]::UtcNow.ToString('o')
    packageDirectory = $package
    temporaryRuntime = $runRoot
    operatingSystem = [Environment]::OSVersion.VersionString
    windowsSandboxExecutablePresent = (Test-Path -LiteralPath $sandboxPath)
    manifestFiles = @($manifest.files).Count
    nativePlatform = $result.platform
    waveformRows = $result.waveformRows
    numericRows = $result.numericRows
    missingValuePreserved = $result.missingValuePreserved
    chartPoints = $result.chartPoints
    runtimeModules = $runtimeModules
    path = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
}
$acceptance | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'acceptance-result.json') -Encoding utf8
Write-Output "PASS: medical GUI, publisher-hashed BIDMC record, missing numerics, respiratory evaluation, Charts/SVG and app-local DLLs"
Write-Output "Evidence: $evidence"
Write-Output "Temporary runtime retained: $runRoot"
Write-Output 'Scope: developer-machine process isolation. A clean Windows VM/Sandbox must be tested separately.'
