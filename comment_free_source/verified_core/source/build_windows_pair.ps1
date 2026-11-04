[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [Parameter(Mandatory)][string]$CompilerRoot,
    [Parameter(Mandatory)][string]$FlexBisonRoot,
    [Parameter(Mandatory)][string]$PythonExe,
    [Parameter(Mandatory)][string]$RuntimeRoot,
    [string]$OfflineSourceRoot = '',
    [string]$SourceRepository = 'https://github.com/YosysHQ/yosys.git',
    [ValidateRange(1,2)][int]$Jobs = 2,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$commit = 'd0e71cfb7bcafe2b437f3edc1789b69e99ecd55a'
$overlay = Join-Path $PSScriptRoot 'optimized_intel'
$inputManifest = Join-Path $PSScriptRoot 'build_input_manifest.json'
if (!(Test-Path -LiteralPath $inputManifest)) { $inputManifest = Join-Path $PSScriptRoot 'manifest.json' }
$evidence = Get-Content -Raw -LiteralPath $inputManifest | ConvertFrom-Json
$output = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $output) { throw 'OutputRoot must be a new directory. Existing source/build/install trees are never overwritten.' }
$compiler = (Resolve-Path -LiteralPath $CompilerRoot).Path
$flexRoot = (Resolve-Path -LiteralPath $FlexBisonRoot).Path
$python = (Resolve-Path -LiteralPath $PythonExe).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeRoot).Path
$toolBin = Join-Path $compiler 'bin'
$cmake = Join-Path $toolBin 'cmake.exe'
$gxx = Join-Path $toolBin 'g++.exe'
$gcc = Join-Path $toolBin 'gcc.exe'
$make = Join-Path $toolBin 'mingw32-make.exe'
$flex = Join-Path $flexRoot 'win_flex.exe'
$bison = Join-Path $flexRoot 'win_bison.exe'
$git = (Get-Command git -ErrorAction Stop).Source
foreach ($required in @($cmake,$gxx,$gcc,$make,$flex,$bison,$python,$overlay)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required tool/source missing: $required" }
}
$previousPath = $env:PATH
$hadAbc = Test-Path Env:ABC
$previousAbc = $env:ABC
$buildPath = "$toolBin;$flexRoot;$previousPath"
New-Item -ItemType Directory -Path $output | Out-Null
$logs = Join-Path $output 'logs'
New-Item -ItemType Directory -Path $logs | Out-Null
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Logged {
    param([string]$Name,[string]$Executable,[string[]]$Arguments,[string]$Directory = $output)
    $logFile = Join-Path $logs "$Name.log"
    $errorFile = Join-Path $logs "$Name.stderr.log"
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Push-Location -LiteralPath $Directory
    try {
        & $Executable @Arguments 1> $logFile 2> $errorFile
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $clock.Stop()
    $records.Add([ordered]@{name=$Name;exe=$Executable;arguments=$Arguments;cwd=$Directory;exit_code=$code;seconds=$clock.Elapsed.TotalSeconds;log=$logFile;stderr=$errorFile})
    $records | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'commands.json') -Encoding utf8
    if ($code -ne 0) { throw "$Name failed, exit $code. See $logFile" }
    return (Get-Content -Raw -LiteralPath $logFile)
}
function Assert-Hash([string]$Path,[string]$Expected) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() -ne $Expected.ToLowerInvariant()) { throw "SHA256 mismatch: $Path" }
}
try {
    $env:PATH = $buildPath
    Remove-Item Env:ABC -ErrorAction SilentlyContinue
    $tools = @(
        @('compiler',$gxx,'16.2.0'), @('cmake',$cmake,'4.4.3'),
        @('flex',$flex,'2.6.4'), @('bison',$bison,'3.8.2'), @('python',$python,'3.12.14')
    )
    foreach ($tool in $tools) {
        $text = Invoke-Logged -Name "version_$($tool[0])" -Executable $tool[1] -Arguments @('--version')
        if (!$text.Contains($tool[2])) { throw "Expected $($tool[0]) $($tool[2]); use the documented toolchain or requalify a different one." }
    }

    $baseSource = Join-Path $output 'source_baseline'
    $optSource = Join-Path $output 'source_optimized'
    $submodules = @('libs/cxxopts','libs/fmt','libs/tomlplusplus','libs/boost_regex','libs/symfpu')
    if ($OfflineSourceRoot) {
        $offline = (Resolve-Path -LiteralPath $OfflineSourceRoot).Path
        if (!(Test-Path -LiteralPath (Join-Path $offline '.git'))) { throw 'Use the included baseline_source directory with its offline Git metadata.' }
        Copy-Item -LiteralPath $offline -Destination $baseSource -Recurse -Force
        foreach ($submodule in $submodules) {
            $expected = ((Invoke-Logged -Name ("expected_"+($submodule -replace '/','_')) -Executable $git -Arguments @('-C',$baseSource,'ls-tree',$commit,$submodule)).Trim() -split '\s+')[2]
            $actual = (Invoke-Logged -Name ("offline_"+($submodule -replace '/','_')) -Executable $git -Arguments @('-C',(Join-Path $baseSource $submodule),'rev-parse','HEAD')).Trim()
            if ($actual -ne $expected) { throw "Wrong offline submodule commit: $submodule" }
        }
    } else {
        Invoke-Logged -Name 'clone' -Executable $git -Arguments @('clone','--no-hardlinks','--no-checkout',$SourceRepository,$baseSource) | Out-Null
        Invoke-Logged -Name 'checkout' -Executable $git -Arguments @('-C',$baseSource,'checkout','--detach',$commit) | Out-Null
        Invoke-Logged -Name 'submodules' -Executable $git -Arguments (@('-C',$baseSource,'submodule','update','--init','--recursive','--')+$submodules) | Out-Null
    }
    $head = (Invoke-Logged -Name 'baseline_head' -Executable $git -Arguments @('-C',$baseSource,'rev-parse','HEAD')).Trim()
    if ($head -ne $commit) { throw 'Wrong baseline commit.' }
    $distance = (Invoke-Logged -Name 'baseline_distance' -Executable $git -Arguments @('-C',$baseSource,'rev-list','--count','v0.69..HEAD')).Trim()
    if ($distance -ne '24') { throw 'Version history must identify exactly v0.69 + 24 commits.' }
    Invoke-Logged -Name 'baseline_clean' -Executable $git -Arguments @('-C',$baseSource,'diff','--exit-code') | Out-Null
                                                                           
                                                                              
    Copy-Item -LiteralPath $baseSource -Destination $optSource -Recurse -Force
    foreach ($entry in $evidence.optimized_intel_sha256.PSObject.Properties) {
        $from = Join-Path $overlay $entry.Name
        $to = Join-Path (Join-Path $optSource 'techlibs/intel') $entry.Name
        Assert-Hash $from $entry.Value
        Copy-Item -LiteralPath $from -Destination $to -Force
        (Get-Item -LiteralPath $to).LastWriteTimeUtc = [DateTime]::UtcNow
        Assert-Hash $to $entry.Value
    }
    $changed = (Invoke-Logged -Name 'optimized_diff_names' -Executable $git -Arguments @('-C',$optSource,'diff','--name-only')).Trim() -split '\r?\n'
    if ((($changed | Sort-Object) -join '|') -ne 'techlibs/intel/CMakeLists.txt|techlibs/intel/synth_intel.cc') { throw 'Unexpected tracked source changes.' }
    Invoke-Logged -Name 'optimized_diff' -Executable $git -Arguments @('-C',$optSource,'diff','--binary') | Out-Null

    $runtimeFiles = [ordered]@{}
    foreach ($entry in $evidence.arms.baseline.runtime_sha256.PSObject.Properties) {
        $from = Join-Path (Join-Path $runtime 'bin') $entry.Name
        if (!(Test-Path -LiteralPath $from)) { $from = Join-Path (Join-Path $runtime 'lib') $entry.Name }
        Assert-Hash $from $entry.Value
        $runtimeFiles[$entry.Name] = $from
    }
    $profile = @(
        '-G','MinGW Makefiles',"-DCMAKE_C_COMPILER:FILEPATH=$gcc","-DCMAKE_CXX_COMPILER:FILEPATH=$gxx",
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$make",'-DCMAKE_BUILD_TYPE:STRING=Release','-DBUILD_SHARED_LIBS:BOOL=OFF',
        '-DCMAKE_C_FLAGS:STRING=','-DCMAKE_CXX_FLAGS:STRING=','-DCMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG',
        '-DCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG','-DCMAKE_EXE_LINKER_FLAGS:STRING=',
        '-DYOSYS_COMPONENTS:STRING=everything','-DYOSYS_ENABLE_UNIT_TESTS:BOOL=OFF','-DYOSYS_WITHOUT_ABC:BOOL=OFF',
        '-DYOSYS_INSTALL_DRIVER:BOOL=ON','-DYOSYS_INSTALL_LIBRARY:BOOL=OFF',
        '-DYOSYS_WITHOUT_SLANG:BOOL=ON','-DYOSYS_WITHOUT_TCL:BOOL=ON','-DYOSYS_WITHOUT_READLINE:BOOL=ON',
        '-DYOSYS_WITHOUT_EDITLINE:BOOL=ON','-DYOSYS_WITHOUT_ZLIB:BOOL=ON','-DYOSYS_WITHOUT_LIBFFI:BOOL=ON',
        '-DYOSYS_USE_BUNDLED_LIBS:BOOL=ON','-DYOSYS_ABC_EXECUTABLE:STRING=yosys-abc.exe',
        "-DFLEX_EXECUTABLE:FILEPATH=$flex","-DBISON_EXECUTABLE:FILEPATH=$bison","-DPython3_EXECUTABLE=$python",
        "-DCMAKE_INSTALL_PREFIX:PATH=$(Join-Path $output 'install_common')"
    )
    $profile | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'common_cmake_profile.json') -Encoding utf8
    [ordered]@{commit=$commit;baseline_source=$baseSource;optimized_source=$optSource;parallel_jobs=$Jobs;prepare_only=[bool]$PrepareOnly;source_overlay=$overlay;runtime_files=$runtimeFiles} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'source_manifest.json') -Encoding utf8
    if ($PrepareOnly) { Write-Output "Source snapshots prepared: $baseSource and $optSource"; return }

    foreach ($arm in @('baseline','optimized')) {
        $env:PATH = $buildPath
        $source = Join-Path $output "source_$arm"
        $build = Join-Path $output "build_$arm"
        $install = Join-Path $output "install_$arm"
        Invoke-Logged -Name "${arm}_configure" -Executable $cmake -Arguments (@('-S',$source,'-B',$build)+$profile) | Out-Null
        Invoke-Logged -Name "${arm}_build" -Executable $cmake -Arguments @('--build',$build,'--target','yosys','--parallel',"$Jobs") | Out-Null
        Invoke-Logged -Name "${arm}_install" -Executable $cmake -Arguments @('--install',$build,'--prefix',$install) | Out-Null
        foreach ($name in $runtimeFiles.Keys) { Copy-Item -LiteralPath $runtimeFiles[$name] -Destination (Join-Path (Join-Path $install 'bin') $name) }
        $runtimeNotices = Join-Path $PSScriptRoot 'third_party_runtime'
        if (Test-Path -LiteralPath $runtimeNotices) { Copy-Item -LiteralPath $runtimeNotices -Destination (Join-Path $install 'third_party_runtime') -Recurse -Force }
        $validation = Join-Path $output "validation_$arm"
        New-Item -ItemType Directory -Path $validation | Out-Null
        foreach ($name in @('native_smoke.v','native_smoke.ys')) { Copy-Item -LiteralPath (Join-Path (Join-Path $PSScriptRoot 'smoke_baseline') $name) -Destination $validation }
        $exe = Join-Path (Join-Path $install 'bin') 'yosys.exe'
        $env:PATH = "$(Join-Path $install 'bin');$env:SystemRoot\System32;$env:SystemRoot"
        Remove-Item Env:ABC -ErrorAction SilentlyContinue
        Invoke-Logged -Name "${arm}_version" -Executable $exe -Arguments @('-V') -Directory $validation | Out-Null
        $helpText = Invoke-Logged -Name "${arm}_help" -Executable $exe -Arguments @('-Q','-T','-p','help opt_pmux') -Directory $validation
        if ($arm -eq 'baseline' -and !$helpText.Contains('No such command')) { throw 'Baseline unexpectedly contains opt_pmux.' }
        if ($arm -eq 'optimized' -and !$helpText.Contains('opt_pmux [selection]')) { throw 'Optimized opt_pmux registration missing.' }
        Invoke-Logged -Name "${arm}_smoke" -Executable $exe -Arguments @('-l','yosys.log','-s','native_smoke.ys') -Directory $validation | Out-Null
        $smoke = Get-Content -Raw -LiteralPath (Join-Path $validation 'yosys.log')
        $called = $smoke.Contains('Executing OPT_PMUX pass')
        if (($arm -eq 'optimized') -ne $called) { throw 'Actual synth_intel dispatch does not match the arm; do not trust help registration alone.' }
        if (!$smoke.Contains('Running ABC command: "yosys-abc.exe"') -or !$smoke.Contains('Found and reported 0 problems.')) { throw 'Portable smoke did not establish ABC startup and check success.' }
        $sharePath = (($install -replace '\\','/')+'/bin/../share/yosys/')
        if (!(($smoke -replace '\\','/').Contains($sharePath))) { throw 'Relative share/yosys discovery was not observed.' }
        Get-FileHash -Algorithm SHA256 -LiteralPath $exe | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output "${arm}_binary_hash.json") -Encoding utf8
    }
    Write-Output "Both isolated builds and dispatch/portability smoke checks passed. Official nine-case EQY and efficiency qualification are separate. Output: $output"
} finally {
    $env:PATH = $previousPath
    if ($hadAbc) { $env:ABC = $previousAbc } else { Remove-Item Env:ABC -ErrorAction SilentlyContinue }
}
