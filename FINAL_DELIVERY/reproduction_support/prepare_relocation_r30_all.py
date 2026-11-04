"""Prepare one fresh r30 migration batch; never launch synthesis or proof.

Old stages, results, and temporary proof directories are not modified. Archived
r20 proof evidence is checked from its persistent files, not historical TEMP.
"""
from pathlib import Path
from datetime import datetime, timezone
import ast
import argparse
import os
import hashlib
import json
import shutil
import sys
import subprocess
import zipfile

HERE = Path(__file__).resolve().parent
HISTORY = None  # Set by explicit configuration in main().
EXPERIMENTS=HERE.parent
ROOT = None  # Set by explicit configuration in main().
SOURCE = HERE / "stage_r30"
DEST = HERE / "relocation r30 all"
PREPARATION = HERE / "RELOCATION_R30_ALL_PREPARATION.json"
LAUNCHER = HERE / "Run-Relocation-R30-All.ps1"
CANDIDATE = "r30_native_sload"
EXPECTED = "efebff04cac47ccdfefc3317d4de14b7f727123a7499243ab9da35ba79f596d9"
BASELINE = "42ae7772e6669815fa25dcdfb8efb06c1c0f0d9a0fb81bdcb9ce4eae4af48858"
ABC = "3d8b4f2e5802682bd74d57acd3b0de292189e983c845880eb3c97b2c94dbe0e4"
WAVE_CHECKPOINT=Path('D:/PMUX_20261102_delivery/research/r30_modelsim_ModelSim_checkpoint.zip')
WAVE_SHA='1a33e2fecca43b16eebc7ff496f385a76280c3ea4c9f021bb8a4fdca87eec7e3'
ENTRY_HASHES = {
    "verification/portable_v3.py": "57e346ff7e5098b27c246d434c6ac4edb6db95db15852d2c818260ed339feaef",
    "Run-OfficialTests.ps1": "53d79887390ab304ab26159ffb62d057ddf7995e81a7fc116400c0df4277dc9f",
}


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_file(root, relative):
    relative = Path(relative)
    require(not relative.is_absolute() and ".." not in relative.parts, "Unsafe package path")
    path = root / relative
    require(not path.is_symlink(), "Symlink in package: " + str(path))
    path = path.resolve()
    path.relative_to(root.resolve())
    require(path.is_file(), "Missing file: " + str(path))
    return path


def indexed_hashes(root, index, path_key):
    result = {}
    for item in index["files"]:
        relative = item[path_key]
        require(relative not in result, "Duplicate index path: " + relative)
        result[relative] = sha(safe_file(root, relative))
        require(result[relative] == item["sha256"], "Index hash mismatch: " + relative)
    return result


def verify(root):
    indexed = indexed_hashes(root, read(root / "PACKAGE_FILES.json"), "path")
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    require(actual == set(indexed) | {"PACKAGE_FILES.json"}, "Unexpected or missing package files")
    archived = indexed_hashes(root, read(root / "ARCHIVE_PATH_INDEX.json"), "package_path")
    require(set(archived) <= set(indexed), "Archive map contains unindexed files")
    release = read(root / "release.json")
    require(release["candidate"] == CANDIDATE, "Wrong release candidate")
    require(release["binaries"] == {"baseline": BASELINE, "optimized": EXPECTED}, "Wrong binary binding")
    require(release["abc_sha256"] == ABC, "Wrong ABC binding")
    for arm, expected in release["binaries"].items():
        require(sha(root / arm / "bin/yosys.exe") == expected, "Frozen executable mismatch")
        require(sha(root / arm / "bin/yosys-abc.exe") == ABC, "Frozen ABC mismatch")

    entrypoints = {}
    for relative, expected in ENTRY_HASHES.items():
        require(sha(root / relative) == expected, "Entrypoint changed since fixed2: " + relative)
        require(sha(HISTORY / "relocation all fixed2" / relative) == expected, "Fixed2 control entrypoint changed")
        entrypoints[relative] = expected
    tree = ast.parse((root / "verification/portable_v3.py").read_text(encoding="utf-8-sig"))
    exit_function = next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == "exit_status")
    context = {}
    exec(compile(ast.Module(body=[exit_function], type_ignores=[]), "isolated_exit_status", "exec"), context)
    require(context["exit_status"]({"success": True, "mode": "All", "all_local_objective_checks_pass": True}) == 0, "Exit PASS semantics")
    require(context["exit_status"]({"success": True, "mode": "All", "all_local_objective_checks_pass": False}) == 3, "Exit objective-failure semantics")
    require(context["exit_status"]({"success": False, "mode": "All"}) == 2, "Exit execution-failure semantics")

    inputs = read(root / "source/build_input_manifest.json")
    overlay = inputs["optimized_intel_sha256"]
    frozen = EXPERIMENTS / "candidates" / CANDIDATE / "source"
    expected_overlay = {p.relative_to(frozen).as_posix(): sha(p) for p in frozen.rglob("*") if p.is_file()}
    require(inputs["candidate"] == CANDIDATE and overlay == expected_overlay, "Expected exact frozen r30 overlay")
    require({p.name for p in (root / "source/optimized_intel").iterdir() if p.is_file()} == set(overlay), "Overlay file set mismatch")
    native = read(root / "evidence/native_build/manifest.json")
    require(native["yosys_sha256"] == EXPECTED and native["frozen_unchanged"] is True, "Native manifest binary/freeze mismatch")
    require(all(x["exit_code"] == 0 for x in native["commands"]), "Native build command failed")
    require({x["label"] for x in native["commands"]} >= {"build", "install", "version", "help"}, "Missing native command evidence")
    frozen_source = EXPERIMENTS / "candidates" / CANDIDATE / "source"
    for relative, expected in overlay.items():
        require(native["source_after"][relative] == expected, "Native source overlay mismatch: " + relative)
        for directory in [root / "source/optimized_intel", root / "evidence/candidate_capture/source", frozen_source]:
            require(sha(safe_file(directory, relative)) == expected, "Source snapshot mismatch: " + str(directory / relative))
    for relative, expected in native["source_after"].items():
        require(sha(safe_file(root / "evidence/modelsim/provenance/native_algorithm/source_candidate/intel", relative)) == expected, "Native full source snapshot mismatch")
    for relative, expected in native["runtime_sha256"].items():
        require(sha(safe_file(root / "optimized/bin", relative)) == expected, "Native runtime mismatch")
    require(sha(root / "evidence/candidate_capture/yosys.exe") == EXPECTED, "Capture binary mismatch")
    require(sha(Path(native["install"]) / "bin/yosys.exe") == EXPECTED, "Original frozen install mismatch")
    require(sha(root / "evidence/native_build/manifest.json") == sha(EXPERIMENTS / "r30_native/manifest.json"), "Native manifest changed")

    wave_root=root / "evidence/modelsim"
    wave_report=read(wave_root / "simulation_report.json")
    wave_binding=read(wave_root / "BINDING_PROVENANCE.json")
    require(wave_report["candidate_label"]==CANDIDATE, "Wrong wave candidate")
    require(wave_binding["candidate_binary_sha256"]==EXPECTED, "Wave compiler identity differs")
    require(wave_binding["binding_v3_sha256"]==sha(root / "evidence/binding.json"), "Wave official binding differs")
    require(wave_binding["strict_eqy_report_sha256"]==sha(root / "evidence/strict/report.json"), "Wave strict binding differs")
    require(wave_binding["performance_report_sha256"]==sha(root / "evidence/performance_v3/report.json"), "Wave performance binding differs")
    require(sha(WAVE_CHECKPOINT)==WAVE_SHA, "Frozen r30 wave checkpoint changed")
    wave_entries={}
    with zipfile.ZipFile(WAVE_CHECKPOINT) as archive:
        for entry in archive.infolist():
            require(not entry.is_dir(), "Unexpected directory entry in frozen wave ZIP")
            prefix="r30_modelsim_ModelSim_checkpoint/"
            require(entry.filename.startswith(prefix), "Unexpected wave ZIP root")
            relative=entry.filename[len(prefix):]
            require(relative not in wave_entries, "Duplicate wave ZIP entry")
            digest=hashlib.sha256(archive.read(entry)).hexdigest()
            require(sha(safe_file(wave_root,relative))==digest, "Wave ZIP/package mismatch: "+relative)
            wave_entries[relative]=digest
    require(len(wave_entries)==365, "Unexpected frozen r30 checkpoint size")
    all_wave_files={p.relative_to(wave_root).as_posix() for p in wave_root.rglob("*") if p.is_file()}
    require(set(wave_entries)<=all_wave_files, "Missing frozen wave checkpoint content")
    wave_cache_extra=sorted(all_wave_files-set(wave_entries))
    case_roots={c["id"] for c in wave_report["cases"]}|{"replay_test4/test4"}
    for relative in wave_cache_extra:
        require(any(relative==case+"/transcript" or relative.startswith(case+"/work/") for case in case_roots), "Unexpected non-checkpoint wave file: "+relative)
        require("evidence/modelsim/"+relative in indexed, "Unindexed retained simulator cache")
    figures=read(wave_root / "FIGURE_LABEL_BINDING_AUDIT.json")
    require(figures["candidate_label"]==CANDIDATE and figures["all_seven_figure_labels_match"] and figures["visual_review_passed"], "Wave figure audit incomplete")
    require(len(figures["figures"])==7, "Expected seven actual r30 figures")
    for figure in figures["figures"]:
        for ext in ("png","svg","pdf"):
            require(sha(wave_root/(figure["figure"]+"."+ext))==figure[ext+"_sha256"], "Wave figure byte binding differs")

    binding = read(root / "evidence/binding.json")
    require(binding["candidate_binary_sha256"] == EXPECTED, "Evidence candidate mismatch")
    require(binding["proof_report_sha256"] == sha(root / "evidence/strict/report.json"), "Strict report binding mismatch")
    require(binding["performance_report_sha256"] == sha(root / "evidence/performance_v3/report.json"), "Performance report binding mismatch")
    for field in ["source_proof_endpoint_and_production_binding_passed", "all_nine_source_logical_zero_proven", "all_logic_not_increased", "all_time_and_memory_within_5_percent"]:
        require(binding[field] is True, "Current official binding failed: " + field)
    require(len(binding["cases"]) == 9, "Expected nine official binding cases")
    require(sha(root / "evidence/binding.json") == sha(EXPERIMENTS / "r30_binding_v3.json"), "Official binding changed")
    strict = read(root / "evidence/strict/report.json")
    runtime = {"endpoint_nozinit_runner.py": strict["toolchain"]["runner_sha256"],
               "dffeas_formal.v": strict["toolchain"]["formal_model_sha256"],
               "dffeas_sload_sim.v": strict["restricted_sclr_contract"]["simulation_counterpart_sha256"],
               "legacy_endpoint_runner.py": strict["restricted_sclr_contract"]["legacy_runner_sha256"],
               "legacy_dffeas_formal.v": strict["restricted_sclr_contract"]["legacy_model_sha256"]}
    for name, digest in runtime.items():
        require(sha(root / "verification" / name) == digest, "Restricted-model runtime mismatch: " + name)
    require(wave_report["validator_sha256"]==runtime["endpoint_nozinit_runner.py"] and wave_report["formal_model_sha256"]==runtime["dffeas_formal.v"] and wave_report["model_sha256"]==runtime["dffeas_sload_sim.v"], "Wave/model runtime differs")
    runner_text = (root / "verification/endpoint_nozinit_runner.py").read_text(encoding="utf-8")
    require("HERE = Path(__file__).resolve().parent" in runner_text, "Model sidecars must be runner-relative")
    require(str(ROOT) not in runner_text and ROOT.as_posix() not in runner_text, "Hardcoded source workspace in runner")
    manifest = read(root / "cases/official_manifest.json")
    require(len(manifest["cases"]) == 9, "Expected nine official cases")
    for case in manifest["cases"]:
        path = safe_file(root / "cases", case["path"])
        require(sha(path) == case["source_sha256"][path.name], "Official source mismatch")
    require(sha(root / "cases/official_manifest.json") == sha(ROOT / "work/20261101_faq_revision/official_cases/official_manifest.json"), "Official manifest/defaults changed")

    historical_root = root / "evidence/migration/fixed2_all/actual_run"
    history = read(historical_root / "strict.archive.json")
    historical_files = indexed_hashes(historical_root / "strict", history, "relative_path")
    history_actual = {p.relative_to(historical_root / "strict").as_posix() for p in (historical_root / "strict").rglob("*") if p.is_file()}
    require(len(historical_files) == 443 and history_actual == set(historical_files), "Persistent historical proof archive incomplete")
    require(release["migration_evidence_is_current"] is False and release["migration_evidence_candidate"] == "r20_native_priority_grouping", "Historical migration wrongly labelled current")
    return {
        "package_index_sha256": sha(root / "PACKAGE_FILES.json"),
        "archive_index_sha256": sha(root / "ARCHIVE_PATH_INDEX.json"),
        "release_sha256": sha(root / "release.json"),
        "indexed_files": len(indexed), "total_files": len(actual), "archive_mappings": len(archived),
        "candidate": CANDIDATE, "candidate_sha256": EXPECTED, "baseline_sha256": BASELINE, "abc_sha256": ABC,
        "entrypoint_hashes_equal_verified_fixed2": entrypoints,
        "restricted_model_runtime_sha256": runtime,
        "wave_checkpoint_sha256":WAVE_SHA,"wave_checkpoint_entries":len(wave_entries),
        "wave_extra_simulator_cache_files":wave_cache_extra,
        "wave_extra_cache_bytes_indexed_and_preserved":True,
        "seven_wave_figure_hashes_checked":True,
        "overlay_file_count": len(overlay), "overlay_sha256": overlay,
        "native_source_after_files_verified": len(native["source_after"]),
        "native_build_manifest_sha256": sha(root / "evidence/native_build/manifest.json"),
        "native_build_and_install_exit_zero": True,
        "current_official_binding_sha256": sha(root / "evidence/binding.json"),
        "historical_r20_persistent_proof_files_verified": len(historical_files),
        "historical_temp_not_used_or_required": True,
        "pending_deliverables_at_copy": release.get("pending_current_deliverables", []),
        "fresh_rebuild_by_this_preparation": False,
    }


def main():
    global SOURCE, DEST, ROOT, HISTORY
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source",type=Path,required=True)
    parser.add_argument("--relocation",type=Path,required=True)
    parser.add_argument("--proof-work-root",type=Path,required=True)
    parser.add_argument("--temp-root",type=Path,required=True)
    parser.add_argument("--workspace-root", type=Path, default=os.environ.get("PMUX_WORKSPACE_ROOT"), required=not os.environ.get("PMUX_WORKSPACE_ROOT"), help="External original workspace with the unchanged official manifest.")
    parser.add_argument("--history-root", type=Path, default=os.environ.get("PMUX_HISTORY_ROOT"), required=not os.environ.get("PMUX_HISTORY_ROOT"), help="External unchanged earlier preparation/audit evidence.")
    parser.add_argument("--pwsh-exe", type=Path, default=os.environ.get("PMUX_PWSH_EXE"), required=not os.environ.get("PMUX_PWSH_EXE"), help="PowerShell 7 executable; version and hash checks remain active.")
    parser.add_argument("--suite-root", type=Path, default=os.environ.get("PMUX_SUITE_ROOT"))
    parser.add_argument("--shell-bin", type=Path, default=os.environ.get("PMUX_SHELL_BIN"))
    args=parser.parse_args()
    ROOT=args.workspace_root.resolve()
    HISTORY=args.history_root.resolve()
    SOURCE=args.source.resolve()
    DEST=args.relocation.resolve()
    proof_root=args.proof_work_root.resolve()
    temp_root=args.temp_root.resolve()
    require(proof_root.drive==DEST.drive==temp_root.drive, "Keep proof and child TEMP on the relocation drive")
    require(SOURCE.is_dir() and (SOURCE/"PACKAGE_FILES.json").is_file(), "Source stage is incomplete")
    require(" " in DEST.name, "Relocation must exercise a space-containing directory name")
    require(not DEST.is_relative_to(SOURCE) and not SOURCE.is_relative_to(DEST), "Stage and relocation must not overlap")
    for path in [DEST, PREPARATION, LAUNCHER]:
        require(not path.exists(), "Refuse overwrite; use one new batch: " + str(path))
    print("Verifying the new immutable r30 stage...", flush=True)
    before=verify(SOURCE)
    historical={}
    for name in ["RELOCATION_R23_ALL_AUDIT.json", "RELOCATION_R23_ALL_EXECUTION_PWSH7.json",
                 "RELOCATION_R23_PREPARATION_ALIAS_ERROR.json", "RELOCATION_R23_ALL_EXECUTION.json",
                 "RELOCATION_R23_ALL_EXECUTION.log", "RELOCATION_R23_ALL_PREPARATION.json",
                 "RELOCATION_ALL_AUDIT.json", "RELOCATION_FIXED2_AUDIT.json", "RELOCATION_R29_STORAGE_EVENT.json",
                 "RELOCATION_R29_PREPARATION_PROVENANCE_PATH_ERROR.json", "prepare_relocation_r29_all.initial_legacy_provenance_path.py",
                 "RELOCATION_R29_ALL_PREPARATION.json", "RELOCATION_R29_ALL_EXECUTION_PWSH7.json",
                 "RELOCATION_R29_ALL_EXECUTION_PWSH7.log", "RELOCATION_R29_ALL_AUDIT.json"]:
        historical[name]=sha(HISTORY/name)
    for previous in ["RELOCATION_R23_ALL_AUDIT.json", "RELOCATION_R29_ALL_AUDIT.json"]:
        prior=read(HISTORY/previous)
        for relative,digest in prior["report_hashes"].items():
            require(sha(Path(prior["run"])/relative)==digest, "Historical run report changed: "+previous)
    local_history={name:sha(HERE/name) for name in ["SCRIPT_PREPARATION_INITIAL_ERROR.json","prepare_scripts.initial_header_guard.py",
        "RELOCATION_R30_PREPARATION_WAVE_SET_ERROR.json","prepare_relocation_r30_all.initial_wave_exact_set.py"]}
    copy_bytes=sum(p.stat().st_size for p in SOURCE.rglob("*") if p.is_file())
    space_probe=DEST.parent
    while not space_probe.exists():
        space_probe=space_probe.parent
    free_bytes=shutil.disk_usage(space_probe).free
    require(free_bytes >= copy_bytes + 512*1024*1024, "Insufficient destination capacity for fresh package plus result reserve")
    print("Copying the immutable stage to a new space-containing directory; no top-level execution results...", flush=True)
    shutil.copytree(SOURCE, DEST, copy_function=shutil.copy2)
    relocated=verify(DEST)
    require(before==relocated==verify(SOURCE), "Source or relocated identity changed")
    suite=(args.suite_root or ROOT/"work/toolchain/oss-cad-suite").resolve()
    shell=(args.shell_bin or ROOT/"work/v4_native/compiler_native/w64devkit/bin").resolve()
    actual_python=Path(sys.base_prefix)/"python.exe"
    pwsh=args.pwsh_exe.resolve()
    external=[suite/"bin/yosys.exe",suite/"lib/python3.exe",suite/"bin/eqy-script.py",shell/"sh.exe",shell/"make.exe",shell/"grep.exe",actual_python,pwsh]
    require(all(p.is_file() for p in external), "Missing explicit runtime")
    version=subprocess.run([str(pwsh),"-NoProfile","-NonInteractive","-Command","$PSVersionTable.PSVersion.ToString()"],capture_output=True,text=True,check=True).stdout.strip()
    require(version.startswith("7."), "Must use PowerShell 7")
    argv=["-SuiteRoot",str(suite),"-ShellBin",str(shell),"-PythonExe",str(actual_python),"-Mode","All","-ProofTimeoutSeconds","180","-ProofWorkRoot",str(proof_root)]
    quoted=lambda value:"'"+str(value).replace("'","''")+"'"
    launcher='# Preparation only; invoke once after exclusive GO.\n# PMUX_RUNTIME_PARAMETER_INTERFACE=1\n[CmdletBinding()]\nparam(\n    [string]$PackageRoot = $env:PMUX_PACKAGE_ROOT,\n    [string]$SuiteRoot = $env:PMUX_SUITE_ROOT,\n    [string]$ShellBin = $env:PMUX_SHELL_BIN,\n    [string]$PythonExe = $env:PMUX_PYTHON_EXE,\n    [string]$ProofWorkRoot = $env:PMUX_PROOF_WORK_ROOT,\n    [ValidatePattern(\'^[0-9a-fA-F]{64}$\')][string]$PackageIndexSha256 = \'__PACKAGE_INDEX_SHA256__\',\n    [switch]$ValidateParametersOnly\n)\n$ErrorActionPreference = \'Stop\'\nforeach ($name in @(\'PackageRoot\',\'SuiteRoot\',\'ShellBin\',\'PythonExe\',\'ProofWorkRoot\')) {\n    if ([string]::IsNullOrWhiteSpace((Get-Variable -Name $name -ValueOnly))) { throw "Supply -$name or the corresponding PMUX_* environment variable." }\n}\nif ($ValidateParametersOnly) { return }\nif (Test-Path -LiteralPath (Join-Path $PackageRoot \'results\')) { throw \'Existing results: refusing selected rerun.\' }\nif ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PackageRoot \'PACKAGE_FILES.json\')).Hash.ToLowerInvariant() -ne $PackageIndexSha256.ToLowerInvariant()) { throw \'Prepared package identity changed.\' }\n& (Join-Path $PackageRoot \'Run-OfficialTests.ps1\') -SuiteRoot $SuiteRoot -ShellBin $ShellBin -PythonExe $PythonExe -Mode \'All\' -ProofTimeoutSeconds \'180\' -ProofWorkRoot $ProofWorkRoot\nexit $LASTEXITCODE\n'
    launcher=launcher.replace("__PACKAGE_INDEX_SHA256__",relocated["package_index_sha256"])
    LAUNCHER.write_text(launcher,encoding="utf-8")
    report={"schema":1,"status":"PREPARED_ONLY_ALL_NOT_STARTED","created_utc":datetime.now(timezone.utc).isoformat(),
      "source":str(SOURCE),"relocation":str(DEST),"package_identity":relocated,
      "source_and_relocation_bytes_match_index":True,
      "copy_space_preflight":{"source_bytes":copy_bytes,"destination_free_bytes":free_bytes,"result_reserve_bytes":512*1024*1024},
      "copy_method":"Fresh full stage copy to space-containing path; all previous runs retained",
      "entrypoint":str(DEST/"Run-OfficialTests.ps1"),"entrypoint_parameters":argv,
      "launcher":str(LAUNCHER),"launcher_sha256":sha(LAUNCHER),
      "pwsh7":{"path":str(pwsh),"sha256":sha(pwsh),"version":version},
      "external_runtime":[{"path":str(p),"sha256":sha(p)} for p in external],
      "preparation_python_alias":sys.executable,"python_execution_path":str(actual_python),
      "python_hash_basis":"Actual sys.base_prefix/python.exe; no attempt to read a Windows alias as a normal file",
      "proof_work_root":str(proof_root),"child_temp_root":str(temp_root),
      "temporary_environment_policy":"Only this All child process receives TEMP/TMP on D; no persistent/system environment change",
      "historical_evidence_root":str(HISTORY),"historical_evidence_sha256":historical,
      "local_preexecution_error_history_sha256":local_history,
      "predeclared_scope":"One new full-nine r30 delivery All reproduction; supplementary to r30_run01, not a replacement",
      "predeclared_policy":"One execution only; retain failures. No overwrite or selected retry.",
      "all_mode_stages":["v3 timed batch with 10 baseline warmups and fixed conditional five-pair rule","separate full area captures after timing","actual postABC capture","nine source-logical-zero to candidate EQY with no candidate zinit","complete all-file proof archive"],
      "acceptance":"rc0, complete, success, all_local_objective_checks_pass, all nine real proofs, model and endpoint binding, complete archive",
      "new_results_directory_exists":(DEST/"results").exists(),"all_started":False,
      "authorization_gate":"WAIT for explicit root exclusive GO before executing All",
      "preparation_only_no_yosys_eqy_build_process_launched":True,
      "source_scripts":{"prepare_relocation_r23_all.py":sha(HISTORY/"prepare_relocation_r23_all.py"),"prepare_relocation_r30_all.py":sha(Path(__file__))}}
    require(not report["new_results_directory_exists"],"Unexpected results")
    PREPARATION.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"status":report["status"],"preparation_sha256":sha(PREPARATION),"package_identity":relocated,"pwsh7":report["pwsh7"]},ensure_ascii=False),flush=True)

if __name__=="__main__":main()
