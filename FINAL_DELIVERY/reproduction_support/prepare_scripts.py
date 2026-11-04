"""Historical initial scaffold, not the authoritative final reproduction entry.

Final prepare/execute/audit scripts received additional checkpoint checks after
this scaffold. It refuses existing outputs. Use those three reviewed files.
"""
from pathlib import Path
import argparse,hashlib,json,os
HERE=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--workspace-root", type=Path, default=os.environ.get("PMUX_WORKSPACE_ROOT"), required=not os.environ.get("PMUX_WORKSPACE_ROOT"))
parser.add_argument("--history-root", type=Path, default=os.environ.get("PMUX_HISTORY_ROOT"), required=not os.environ.get("PMUX_HISTORY_ROOT"))
args=parser.parse_args()
ROOT=args.workspace_root.resolve()
HISTORY=args.history_root.resolve()
EXPERIMENTS=HERE.parent
OLD_SHA='0b00f81ecacfd7d34f395105a8b35e4b5b824ea10b01a9e2a3555f6157c0f92c'
NEW_SHA='efebff04cac47ccdfefc3317d4de14b7f727123a7499243ab9da35ba79f596d9'
def replace_one(s,a,b):
 assert s.count(a)==1,a
 return s.replace(a,b)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
records={}
for oldname,newname in [
 ('prepare_relocation_r29_all.py','prepare_relocation_r30_all.py'),
 ('execute_relocation_r29_all_pwsh7.py','execute_relocation_r30_all_pwsh7.py'),
 ('audit_relocation_r29_all.py','audit_relocation_r30_all.py')]:
 src=HISTORY/oldname;dst=HERE/newname;assert not dst.exists()
 s=src.read_text(encoding='utf-8')
 s=s.replace('r29_native_hardware_sclr','r30_native_sload').replace(OLD_SHA,NEW_SHA)
 s=s.replace('R29','R30').replace('r29','r30').replace('dffeas_sclr_sim.v','dffeas_sload_sim.v')
 header='HERE = Path(__file__).resolve().parent' if 'HERE = Path(__file__).resolve().parent' in s else 'HERE=Path(__file__).resolve().parent'
 s=replace_one(s,'\n'+header+'\n','\n'+header+'\nHISTORY=Path(__import__("os").environ["PMUX_HISTORY_ROOT"])\nEXPERIMENTS=HERE.parent\n')
 if oldname.startswith('prepare'):
  s=replace_one(s,'ROOT = HERE.parents[1]','ROOT = Path(__import__("os").environ["PMUX_WORKSPACE_ROOT"])')
  s=s.replace('HERE / "relocation all fixed2"','HISTORY / "relocation all fixed2"')
  s=s.replace('ROOT / "work/20261101_faq_revision/candidates" / CANDIDATE / "source"','EXPERIMENTS / "candidates" / CANDIDATE / "source"')
  s=s.replace('ROOT / "work/20261102_r30_native/manifest.json"','EXPERIMENTS / "r30_native/manifest.json"')
  s=s.replace('ROOT / "work/20261101_pessimistic_optimization/r30_binding_v3.json"','EXPERIMENTS / "r30_binding_v3.json"')
  start=s.index('    historical={}\n')
  end=s.index('    copy_bytes=',start)
  s=s[:start]+'''    historical={}
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
''' + s[end:]
  s=s.replace('sha(HERE/"prepare_relocation_r23_all.py")','sha(HISTORY/"prepare_relocation_r23_all.py")')
  s=s.replace('"historical_evidence_sha256":historical,','"historical_evidence_root":str(HISTORY),"historical_evidence_sha256":historical,')
  # Require the actual new wave package, never silently carry the r29 checkpoint.
  needle='    binding = read(root / "evidence/binding.json")'
  insertion='''    wave_report = read(root / "evidence/modelsim/simulation_report.json")
    # The stage's source capture and archived native provenance are separately
    # hashed above. Actual wave metadata stays intact; its native EXE identity
    # and every package-file hash are audited before relocation.
    require(EXPECTED in json.dumps(wave_report), "Wave report lacks actual r30 compiler identity")
'''
  s=replace_one(s,needle,insertion+'\n'+needle)
 elif oldname.startswith('audit'):
  s=s.replace('sha(HERE / name) == digest','sha(Path(prepared["historical_evidence_root"]) / name) == digest')
  for name in ['RELOCATION_R23_ALL_AUDIT.json','RELOCATION_ALL_AUDIT.json','RELOCATION_FIXED2_AUDIT.json',
               'RELOCATION_R23_PREPARATION_ALIAS_ERROR.json','RELOCATION_R23_ALL_EXECUTION.json']:
   s=s.replace('HERE / "'+name+'"','HISTORY / "'+name+'"')
  s=s.replace('["RELOCATION_R30_STORAGE_EVENT.json"]','["RELOCATION_R29_STORAGE_EVENT.json"]')
  s=s.replace('"prior_r23_failed_and_successful_material_preserved": True,','"prior_r23_failed_and_successful_material_preserved": True, "prior_r29_material_preserved": True,')
  s=s.replace('Snapshot had books pending at copy; this is evidence-package migration QA, not final book archive QA.',
   'This audits the prepared snapshot listed in package_snapshot_identity; final book archive QA is separately required when books were pending at copy.')
  marker='    index = read(PACKAGE / "PACKAGE_FILES.json")'
  inserted='''    r29 = read(HISTORY / "RELOCATION_R29_ALL_AUDIT.json")
    for relative, digest in r29["report_hashes"].items():
        require(sha(Path(r29["run"]) / relative) == digest, "Earlier r29 run changed")
'''
  s=replace_one(s,marker,inserted+marker)
 compile(s,str(dst),'exec');dst.write_text(s,encoding='utf-8')
 records[newname]={'origin':str(src),'origin_sha256':sha(src),'new_sha256':sha(dst)}
(HERE/'SCRIPT_PREPARATION.json').write_text(json.dumps({'status':'SCRIPTS_ONLY_NO_STAGE_COPY_OR_ALL_STARTED',
 'orchestration_sources':records,'history_preserved_at':str(HISTORY),
 'candidate':'r30_native_sload','candidate_exe_sha256':NEW_SHA,
 'new_runtime_simulation_sidecar':'dffeas_sload_sim.v',
 'single_run_policy':'Do not invoke All until root exclusive GO; keep all actual failures without selected reruns.'},indent=2)+'\n',encoding='utf-8')
print('r30 three scripts prepared; no stage copied and no All started.')
