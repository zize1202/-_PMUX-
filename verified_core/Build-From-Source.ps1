[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [Parameter(Mandatory)][string]$CompilerRoot,
    [Parameter(Mandatory)][string]$FlexBisonRoot,
    [Parameter(Mandatory)][string]$BuildPythonExe,
    [string]$PythonExe = 'python.exe',
    [string]$RuntimeRoot = '',
    [ValidateRange(1,2)][int]$Jobs = 2,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use the existing PowerShell 7 terminal. Do not change execution policy or invoke Windows PowerShell 5.1.' }
$buildShell = Join-Path $PSHOME 'pwsh.exe'
$python = (Get-Command $PythonExe -CommandType Application -ErrorAction Stop).Source
$arguments = @((Join-Path $PSScriptRoot 'verification/rebuild.py'),'--root',$PSScriptRoot,'--output-root',$OutputRoot,
    '--compiler-root',$CompilerRoot,'--flex-bison-root',$FlexBisonRoot,'--python-exe',$BuildPythonExe,'--jobs',"$Jobs",'--powershell',$buildShell)
if ($RuntimeRoot) { $arguments += @('--runtime-root',$RuntimeRoot) }
if ($PrepareOnly) { $arguments += '--prepare-only' }
& $python @arguments
exit $LASTEXITCODE
