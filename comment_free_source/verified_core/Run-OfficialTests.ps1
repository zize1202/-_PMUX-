[CmdletBinding()]
param(
    [string]$SuiteRoot = '',
    [string]$ShellBin = '',
    [string]$PythonExe = 'python.exe',
    [ValidateSet('Check','Measure','All','StrictSmoke')][string]$Mode = 'All',
    [string[]]$Case = @(),
    [string]$ProofWorkRoot = '',
    [ValidateRange(1,3600)][int]$ProofTimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
try {
$runner = Join-Path $PSScriptRoot 'verification/portable_v3.py'
$python = (Get-Command $PythonExe -CommandType Application -ErrorAction Stop).Source
$arguments = @($runner,'--root',$PSScriptRoot,'--mode',$Mode,'--proof-timeout',"$ProofTimeoutSeconds")
if ($SuiteRoot) { $arguments += @('--suite',$SuiteRoot) }
if ($ShellBin) { $arguments += @('--shell-bin',$ShellBin) }
if ($ProofWorkRoot) { $arguments += @('--proof-work-root',$ProofWorkRoot) }
foreach ($identifier in $Case) { $arguments += @('--case',$identifier) }
                                                                                     
& $python @arguments
exit $LASTEXITCODE
} catch {
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 2
}
