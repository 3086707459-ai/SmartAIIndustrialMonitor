# Run INSIDE a fresh Windows Sandbox / Windows VM after copying the package.
# The script deliberately does not enable Windows features or launch a VM.
[CmdletBinding()]
param(
    [string]$PackageDirectory = (Split-Path -Parent $PSScriptRoot),
    [string]$EvidenceDirectory = (Join-Path $env:USERPROFILE 'Desktop/SAIM-acceptance')
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'verify-package.ps1') -PackageDirectory $PackageDirectory -EvidenceDirectory $EvidenceDirectory
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Package verification failed' }
# Attestation is separate from test output: the operator supplies environment
# provenance, and the JSON never infers a clean VM merely from its OS edition.
Write-Output 'Record the VM/Sandbox image/version and that no Qt or compiler was installed; retain all evidence files.'
