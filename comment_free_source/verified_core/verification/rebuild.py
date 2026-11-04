"""Launch the preserved build_windows_pair.ps1 in a child process, using fresh directories."""
from pathlib import Path
import argparse, json, os, shutil, subprocess, sys, zipfile

def powershell7(explicit=None):
    executable=explicit or shutil.which('pwsh.exe') or shutil.which('pwsh')
    if not executable:raise FileNotFoundError('PowerShell 7 is required. Run Build-From-Source.ps1 in your existing PowerShell 7 terminal; do not change execution policy.')
    executable=Path(executable).resolve()
    if executable.name.lower() not in ('pwsh.exe','pwsh'):raise ValueError('Use the PowerShell 7 pwsh executable, not Windows PowerShell 5.1.')
    result=subprocess.run([str(executable),'-NoProfile','-NonInteractive','-Command','$PSVersionTable.PSVersion.Major'],capture_output=True,text=True,check=True,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    if int(result.stdout.strip())<7:raise ValueError('PowerShell 7 or later is required; no execution-policy changes are made.')
    return executable

def safe_extract(archive,directory):
    destination=Path(directory).resolve()
    if destination.exists():raise FileExistsError(destination)
    with zipfile.ZipFile(archive) as z:
        for item in z.infolist():
            relative=Path(item.filename.replace('\\','/'))
            target=(destination/relative).resolve()
            if relative.is_absolute() or not target.is_relative_to(destination) or (item.external_attr>>16)&0o170000==0o120000:raise ValueError('Unsafe source archive member: '+item.filename)
        destination.mkdir(parents=True)
        z.extractall(destination)
    return destination/'baseline_source'
def main():
    ap=argparse.ArgumentParser()
    for name in ['root','output-root','compiler-root','flex-bison-root','python-exe']:ap.add_argument('--'+name,required=True,type=Path)
    ap.add_argument('--runtime-root',type=Path);ap.add_argument('--prepare-only',action='store_true');ap.add_argument('--jobs',type=int,choices=[1,2],default=2)
    ap.add_argument('--powershell',type=Path)
    a=ap.parse_args();root=a.root.resolve();output=a.output_root.resolve()
    if output.exists():raise FileExistsError('Build output must be new: '+str(output))
    powershell=powershell7(a.powershell)
    from portable_v3 import verify_package
    verify_package(root)
    source=safe_extract(root/'source/yosys_d0e71cfb7_baseline_offline.zip',output.with_name(output.name+'.offline_sources'))
    argv=[powershell,'-NoProfile','-NonInteractive','-File',root/'source/build_windows_pair.ps1','-OutputRoot',output,'-CompilerRoot',a.compiler_root.resolve(),'-FlexBisonRoot',a.flex_bison_root.resolve(),'-PythonExe',a.python_exe.resolve(),'-RuntimeRoot',(a.runtime_root or root/'baseline').resolve(),'-OfflineSourceRoot',source,'-Jobs',str(a.jobs)]
    if a.prepare_only:argv.append('-PrepareOnly')
                                                                                         
    return subprocess.run(list(map(str,argv)),env=os.environ.copy(),creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0).returncode
if __name__=='__main__':raise SystemExit(main())
