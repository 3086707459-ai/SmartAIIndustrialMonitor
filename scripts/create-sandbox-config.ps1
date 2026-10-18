[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
$output = [IO.Path]::GetFullPath($OutputFile)
if (Test-Path -LiteralPath $output) { throw 'Refusing to overwrite an existing Sandbox configuration' }
if (Test-Path -LiteralPath $evidence) { throw 'Choose a new empty evidence directory' }
if (-not (Test-Path -LiteralPath (Join-Path $package 'acceptance/verify-in-sandbox.ps1'))) { throw 'Package acceptance script missing' }
New-Item -ItemType Directory -Path $evidence | Out-Null
$packageXml = [Security.SecurityElement]::Escape($package)
$evidenceXml = [Security.SecurityElement]::Escape($evidence)
@"
<Configuration>
  <Networking>Disable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$packageXml</HostFolder><SandboxFolder>C:\SAIM</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$evidenceXml</HostFolder><SandboxFolder>C:\Evidence</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\SAIM\acceptance\verify-in-sandbox.ps1 -PackageDirectory C:\SAIM -EvidenceDirectory C:\Evidence\run</Command></LogonCommand>
</Configuration>
"@ | Set-Content -LiteralPath $output -Encoding utf8
Write-Output "Created $output. Run it on a Windows host with Windows Sandbox enabled."
Write-Output 'This command only creates configuration; it does not certify or enable a clean environment.'
