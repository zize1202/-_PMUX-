"""Execute only the newly prepared r30 All batch after exclusive root GO."""
from pathlib import Path, PureWindowsPath
from datetime import datetime,timezone
import argparse,hashlib,json,subprocess,os
HERE=Path(__file__).resolve().parent
HISTORY = None  # Reserved historical context; this executor reads PREP instead.
EXPERIMENTS=HERE.parent
REPORT=HERE/"RELOCATION_R30_ALL_EXECUTION_PWSH7.json"
LOG=HERE/"RELOCATION_R30_ALL_EXECUTION_PWSH7.log"
PREP=HERE/"RELOCATION_R30_ALL_PREPARATION.json"
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
now=lambda:datetime.now(timezone.utc).isoformat()
def configured_path(value):
    return Path(value).resolve() if value is not None else None

def same_path(left,right):
    return str(PureWindowsPath(str(left))).casefold()==str(PureWindowsPath(str(right))).casefold()

def main():
    global PREP, REPORT, LOG
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preparation-sha256",required=True)
    parser.add_argument("--preparation",type=Path,default=PREP,help="Read-only preparation record; its SHA256 must match --preparation-sha256.")
    parser.add_argument("--record-dir",type=Path,default=HERE,help="Fresh output location; existing execution evidence is never overwritten.")
    for option,variable in [("suite-root","PMUX_SUITE_ROOT"),("shell-bin","PMUX_SHELL_BIN"),("python-exe","PMUX_PYTHON_EXE"),("pwsh-exe","PMUX_PWSH_EXE"),("relocation","PMUX_PACKAGE_ROOT"),("proof-work-root","PMUX_PROOF_WORK_ROOT"),("temp-root","PMUX_TEMP_ROOT"),("launcher","PMUX_LAUNCHER")]:
        parser.add_argument("--"+option,type=Path,default=os.environ.get(variable),help="Explicit path override; recorded file hashes remain mandatory.")
    args=parser.parse_args()
    PREP=args.preparation.resolve()
    record_dir=args.record_dir.resolve()
    REPORT=record_dir/REPORT.name
    LOG=record_dir/LOG.name
    if REPORT.exists() or LOG.exists():raise FileExistsError("Single batch already has evidence; no overwrite/retry")
    assert sha(PREP)==args.preparation_sha256
    p=json.loads(PREP.read_text(encoding="utf-8"))
    relocation=configured_path(args.relocation) or Path(p["relocation"])
    assert not (relocation/"results").exists()
    launcher=configured_path(args.launcher) or Path(p["launcher"])
    assert sha(launcher)==p["launcher_sha256"]
    parameters=dict(zip(p["entrypoint_parameters"][::2],p["entrypoint_parameters"][1::2]))
    suite=configured_path(args.suite_root) or Path(parameters["-SuiteRoot"])
    shell_bin=configured_path(args.shell_bin) or Path(parameters["-ShellBin"])
    python=configured_path(args.python_exe) or Path(p["python_execution_path"])
    shell=p["pwsh7"]
    pwsh=configured_path(args.pwsh_exe) or Path(shell["path"])
    runtime_bindings=[(str(PureWindowsPath(parameters["-SuiteRoot"])/relative),suite/relative) for relative in ["bin/yosys.exe","lib/python3.exe","bin/eqy-script.py"]]
    runtime_bindings += [(str(PureWindowsPath(parameters["-ShellBin"])/name),shell_bin/name) for name in ["sh.exe","make.exe","grep.exe"]]
    runtime_bindings += [(p["python_execution_path"],python),(shell["path"],pwsh)]
    actual_runtime=[]
    for item in p["external_runtime"]:
        actual=next((new for old,new in runtime_bindings if same_path(item["path"],old)),Path(item["path"]))
        assert sha(actual)==item["sha256"]
        actual_runtime.append({"path":str(actual),"sha256":item["sha256"]})
    assert shell["version"].startswith("7.") and sha(pwsh)==shell["sha256"]
    command=[str(pwsh),"-NoProfile","-NonInteractive","-File",str(launcher)]
    temp_root=configured_path(args.temp_root) or Path(p["child_temp_root"])
    proof_root=configured_path(args.proof_work_root) or Path(p["proof_work_root"])
    assert temp_root.drive==proof_root.drive==relocation.drive
    parameter_interface="# PMUX_RUNTIME_PARAMETER_INTERFACE=1" in launcher.read_text(encoding="utf-8-sig")
    launch_overrides=any(value is not None for value in [args.suite_root,args.shell_bin,args.python_exe,args.relocation,args.proof_work_root])
    if launch_overrides and not parameter_interface:
        raise ValueError("Path overrides require a prepared parameter-interface launcher; create a fresh preparation instead of changing historical hashes.")
    if parameter_interface:
        command += ["-PackageRoot",str(relocation),"-SuiteRoot",str(suite),"-ShellBin",str(shell_bin),"-PythonExe",str(python),"-ProofWorkRoot",str(proof_root),"-PackageIndexSha256",p["package_identity"]["package_index_sha256"]]
    record_dir.mkdir(parents=True,exist_ok=True)
    temp_root.mkdir(parents=True,exist_ok=True)
    proof_root.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    env['TEMP']=str(temp_root);env['TMP']=str(temp_root)
    record={"schema":1,"started_utc":now(),"command":command,"cwd":str(HERE),"log":str(LOG),
            "preparation_sha256":sha(PREP),"scope":p["predeclared_scope"],
            "root_exclusive_timing_window_authorized":True,"returncode":None,"finished":False,
            "pwsh7_sha256":shell["sha256"],"execution_policy_changed":False,"actual_python_path":str(python),
            "child_only_environment_overrides":{"TEMP":str(temp_root),"TMP":str(temp_root)},"system_environment_modified":False}
    record["actual_external_runtime"]=actual_runtime
    record["source_preparation_path"]=str(PREP)
    record["source_preparation_read_only"]=True
    record["explicit_path_overrides"]={name:str(value) for name,value in vars(args).items() if name not in {"preparation_sha256","preparation","record_dir"} and value is not None}
    record["execution_is_new_record"]=True
    def save():REPORT.write_text(json.dumps(record,indent=2)+"\n",encoding="utf-8")
    save()
    with LOG.open("w",encoding="utf-8") as log:
        child=subprocess.Popen(command,cwd=HERE,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        record["pid"]=child.pid;save();print(json.dumps(record),flush=True)
        record["returncode"]=child.wait()
    record.update(finished=True,finished_utc=now());save()
    print(json.dumps(record),flush=True)
    return record["returncode"]
if __name__=="__main__":raise SystemExit(main())
