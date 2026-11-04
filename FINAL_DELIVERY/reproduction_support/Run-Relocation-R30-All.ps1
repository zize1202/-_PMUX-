# Preparation only; invoke once after exclusive GO.
# PMUX_RUNTIME_PARAMETER_INTERFACE=1
[CmdletBinding()]
param(
    [string]$PackageRoot = $env:PMUX_PACKAGE_ROOT,
    [string]$SuiteRoot = $env:PMUX_SUITE_ROOT,
    [string]$ShellBin = $env:PMUX_SHELL_BIN,
    [string]$PythonExe = $env:PMUX_PYTHON_EXE,
    [string]$ProofWorkRoot = $env:PMUX_PROOF_WORK_ROOT,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$PackageIndexSha256 = 'ee5826f867040189be497d0302871351ea13a87025e2051e3861b5ec74dccccd',
    [switch]$ValidateParametersOnly
)
$ErrorActionPreference = 'Stop'
foreach ($name in @('PackageRoot','SuiteRoot','ShellBin','PythonExe','ProofWorkRoot')) {
    if ([string]::IsNullOrWhiteSpace((Get-Variable -Name $name -ValueOnly))) { throw "Supply -$name or the corresponding PMUX_* environment variable." }
}
if ($ValidateParametersOnly) { return }
if (Test-Path -LiteralPath (Join-Path $PackageRoot 'results')) { throw 'Existing results: refusing selected rerun.' }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PackageRoot 'PACKAGE_FILES.json')).Hash.ToLowerInvariant() -ne $PackageIndexSha256.ToLowerInvariant()) { throw 'Prepared package identity changed.' }
& (Join-Path $PackageRoot 'Run-OfficialTests.ps1') -SuiteRoot $SuiteRoot -ShellBin $ShellBin -PythonExe $PythonExe -Mode 'All' -ProofTimeoutSeconds '180' -ProofWorkRoot $ProofWorkRoot
exit $LASTEXITCODE
