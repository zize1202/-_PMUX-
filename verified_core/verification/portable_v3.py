"""Relocatable C145-scope timing and direct source-logical-zero EQY.

All writes go to a fresh results directory. No system or parent environment changes.
StrictSmoke deliberately skips timing and never creates official acceptance.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, importlib.util, json, os, re, shutil, subprocess, sys, tempfile, uuid

FLAGS = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
ARMS = ('baseline', 'optimized')
PROOF_PATH_LIMIT = 240  # Leave room below legacy Win32 MAX_PATH, including NUL.

def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,d):Path(p).write_text(json.dumps(d,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def require(p):
    p=Path(p).resolve()
    if not p.is_file():raise FileNotFoundError(p)
    return p
def launch(argv,cwd,env,log,timeout=None):
    with Path(log).open('wb') as stream:
        proc=subprocess.Popen(list(map(str,argv)),cwd=cwd,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=FLAGS)
        expired=False
        try:code=proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            expired=True
            subprocess.run(['taskkill','/PID',str(proc.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=FLAGS,check=False)
            code=proc.wait()
    return {'command':list(map(str,argv)),'returncode':code,'timed_out':expired,'log':str(log)}
def load_helper(path):
    spec=importlib.util.spec_from_file_location('package_fair_measure_helper',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def child_environment(runtime,abc):
    env=os.environ.copy();env['PATH']=os.pathsep.join(map(str,runtime))+os.pathsep+env.get('PATH','');env['ABC']=str(abc)
    return env
def execution_scope(mode):
    return {
        'Check':'Static package integrity checks only; no process execution.',
        'Measure':'C145 two-command lifecycle timing, then separate untimed area capture; no EQY.',
        'All':'C145 two-command lifecycle timing, separate untimed area capture, candidate postABC capture, then direct source-zero EQY.',
        'StrictSmoke':'Untimed selected-candidate postABC capture and direct source-zero EQY only; no efficiency measurement or official-nine acceptance.'
    }[mode]
def exit_status(result):
    if not result.get('success'):return 2
    if result['mode']=='All' and not result.get('all_local_objective_checks_pass'):return 3
    if result['mode']=='Measure' and not result.get('area_and_efficiency_pass'):return 3
    return 0
def path_units(path):
    return len(str(path).encode('utf-16-le'))//2
def archive_io_path(path):
    # Only Python archive I/O uses extended-length paths. Formal tools still
    # receive ordinary short paths and require the explicit preflight above.
    path=Path(path).resolve()
    if os.name!='nt':return path
    value=str(path)
    if value.startswith('\\\\?\\'):return path
    return Path('\\\\?\\UNC\\'+value[2:] if value.startswith('\\\\') else '\\\\?\\'+value)
def proof_path_preflight(proof_dir,manifest,bound_proof,external_paths=()):
    """Bound official interfaces plus archived partition names; no strategy changes.

    Count the *unresolved* cwd/../../../partitions path used by EQY's SAT
    strategy. Checking only Path.resolve() would miss the actual MAX_PATH bug.
    """
    cases={c['id']:c for c in bound_proof['cases']};checks=[]
    for case in manifest['cases']:
        archived=cases[case['id']]
        names=set(archived['rtl']['ports'])
        partitions=set()
        for check in archived['checks']:
            names.update(check.get('additional_jointly_proved_state_relations',{}))
            for status in check.get('strategy_statuses',{}):
                parts=status.replace('\\','/').split('/')
                if len(parts)>=4 and parts[0]=='strategies':partitions.add(parts[1])
        partitions.update(case['top']+'.'+name for name in names)
        if not partitions:raise ValueError('No bound proof identifiers for path preflight')
        for partition in sorted(partitions):
            if '/' in partition or '\\' in partition or partition in ('.','..'):raise ValueError('Unsafe partition identifier')
            cwd=proof_dir/case['id']/'rb/strategies'/partition/'induction'
            raw=cwd/'../../../partitions'/(partition+'.sv')
            # Reserve eight additional characters for generated name suffixes.
            checks.append({'kind':'raw_strategy_partition_reference','path':str(raw),'utf16_units':path_units(raw),'reserve':8})
    for path in external_paths:
        checks.append({'kind':'external_input_or_tool','path':str(path),'utf16_units':path_units(path),'reserve':0})
    worst=max(checks,key=lambda x:x['utf16_units']+x['reserve'])
    if worst['utf16_units']+worst['reserve']>=PROOF_PATH_LIMIT:
        raise ValueError('Proof path preflight rejected '+str(worst['utf16_units']+worst['reserve'])+
                         ' UTF-16 units (limit '+str(PROOF_PATH_LIMIT)+'): '+worst['path']+
                         '. Use -ProofWorkRoot C:\\pmux_tmp and, if the rejected path is an input/tool, move the package/tool suite to a shorter path. No proof was started.')
    return {'status':'PASS','limit_utf16_units_exclusive':PROOF_PATH_LIMIT,'max_units_with_reserve':worst['utf16_units']+worst['reserve'],
            'basis':'Bound official public/state identifiers and archived EQY partition names; raw unnormalized strategy references plus 8-character reserve.',
            'checked_paths':checks}
def archive_proof(source,destination,index_path):
    """Copy completed or failed evidence byte-for-byte; retain original workspace."""
    if destination.exists():raise FileExistsError('Proof archive must be new: '+str(destination))
    files=[]
    for p in sorted(source.rglob('*')):
        if p.is_symlink():raise ValueError('Proof evidence symlink is unsupported: '+str(p))
        if p.is_file():files.append({'relative_path':p.relative_to(source).as_posix(),'sha256':sha(p)})
    target_io=archive_io_path(destination)
    shutil.copytree(archive_io_path(source),target_io)
    expected={x['relative_path']:x['sha256'] for x in files}
    actual={p.relative_to(target_io).as_posix():sha(p) for p in target_io.rglob('*') if p.is_file()}
    after={p.relative_to(source).as_posix():sha(p) for p in source.rglob('*') if p.is_file()}
    if actual!=expected or after!=expected:raise ValueError('Proof archive differs from original execution evidence')
    index={'schema':1,'execution_directory':str(source),'archive_directory':str(destination),'raw_report_paths_rewritten':False,
           'original_workspace_retained':True,'file_count':len(files),'all_file_hashes_verified':True,'files':files}
    save(index_path,index);return index
def verify_package(root):
    index=read(require(root/'PACKAGE_FILES.json'))
    for item in index['files']:
        relative=Path(item['path'])
        if relative.is_absolute() or '..' in relative.parts:raise ValueError('Unsafe package index')
        if sha(require(root/relative))!=item['sha256']:raise ValueError('Package file changed: '+item['path'])
    release=read(require(root/'release.json'))
    for arm in ARMS:
        if sha(require(root/arm/'bin/yosys.exe'))!=release['binaries'][arm]:raise ValueError('Bound binary mismatch')
    if sha(require(root/'baseline/bin/yosys-abc.exe'))!=release['abc_sha256']:raise ValueError('ABC mismatch')
    return release
def capture_candidate(root,run,manifest,fm,exe,abc,runtime):
    """Separate postABC capture, never included in the official timing fields."""
    target=run/'capture';target.mkdir();scripts=target/'scripts';scripts.mkdir()
    report={'scope':'UNTIMED_CANDIDATE_POSTABC_CAPTURE','included_in_efficiency':False,'complete':False,'cases':[]}
    binary=sha(exe);abc_hash=sha(abc);library=fm.tree_fingerprint(root/'optimized/share/yosys')
    for case in manifest['cases']:
        folder=target/case['id'];folder.mkdir()
        modes={Path(s['path']).suffix.lower()=='.sv' for s in case['resolved_sources']}
        if len(modes)!=1:raise ValueError('Mixed language source mode requires explicit review')
        lines=['read_verilog'+(' -sv' if True in modes else '')+' '+' '.join(fm.yosys_quote(s['path']) for s in case['resolved_sources']),
               f"synth_intel -family cycloneiv -top {case['top']} -run begin:map_cells",
               'write_rtlil post_abc9.il','write_json post_abc9.json',
               f"synth_intel -family cycloneiv -top {case['top']} -run map_cells:",
               'write_json mapped.netlist.json']
        script=scripts/(case['id']+'.ys');script.write_text('\n'.join(lines)+'\n',encoding='utf-8')
        before=sha(script)
        for s in case['resolved_sources']:
            if sha(s['path'])!=s['sha256']:raise ValueError('Source changed before capture')
        result=launch([exe,'-s',script],folder,child_environment(runtime,abc),folder/'yosys.log',180)
        if result['returncode'] or result['timed_out']:raise RuntimeError('Capture failed: '+case['id'])
        if sha(exe)!=binary or sha(abc)!=abc_hash or sha(script)!=before:raise ValueError('Capture provenance changed')
        for s in case['resolved_sources']:
            if sha(s['path'])!=s['sha256']:raise ValueError('Source changed during capture')
        actual=fm.abc_execution_evidence(folder/'yosys.log',exe,abc_hash)
        if not actual or not all(x['matches_requested_binary_bytes'] for x in actual):raise ValueError('Actual ABC mismatch')
        cells=fm.cell_counts(folder/'mapped.netlist.json',case['top'])
        if not cells['classification_complete']:raise ValueError('Unknown final resources')
        defaults=cells['top_parameter_defaults_raw'];expected=case.get('default_parameters',{})
        if set(defaults)!=set(expected) or any(fm.parse_int(defaults[k])!=v for k,v in expected.items()):raise ValueError('Default parameters changed')
        report['cases'].append({'id':case['id'],'success':True,'cells':cells,'execution':result,'actual_abc_invocations':actual,
                                'postabc_sha256':sha(folder/'post_abc9.il'),'full_json_sha256':sha(folder/'mapped.netlist.json')})
        save(target/'report.json',report)
    if fm.tree_fingerprint(root/'optimized/share/yosys')['sha256']!=library['sha256']:raise ValueError('Gate library changed')
    report['complete']=True;save(target/'report.json',report);return report
def audit_strict(path,captured,manifest,model):
    proof=read(path);expected={c['id'] for c in manifest['cases']}
    if not proof['complete'] or {c['id'] for c in proof['cases']}!=expected:raise ValueError('Incomplete strict proof case set')
    if proof['toolchain']['formal_model_sha256']!=sha(model):raise ValueError('Formal model mismatch')
    cap={c['id']:c for c in captured['cases']};sources={c['id']:{str(Path(s['path']).resolve()):s['sha256'] for s in c['resolved_sources']} for c in manifest['cases']}
    for c in proof['cases']:
        if c['status']!='PASS' or len(c['checks'])!=1:raise ValueError('Strict proof did not pass')
        k=c['checks'][0];folder=path.parent/c['id'];script=(folder/'baseline.normalize.ys').read_text(encoding='utf-8')
        if re.search(r'^\s*(zinit|setundef)\b',script,re.M):raise ValueError('Candidate proof-side initialization repair')
        if k['name']!='rtl_to_baseline' or k['eqy_raw_status']!='PASS' or k['execution']['returncode'] or k['execution']['timed_out']:raise ValueError('EQY execution failure')
        if not (folder/k['result_directory']/'PASS').is_file() or k['partition_count']!=1 or not k['only_original_inputs_used_as_partition_inputs']:raise ValueError('Incomplete whole-output proof')
        if k['proven_output_bit_counts']!=k['expected_output_bit_counts']:raise ValueError('Output coverage mismatch')
        if c['baseline']['source_sha256']!=cap[c['id']]['postabc_sha256'] or sha(folder/'baseline.source.il')!=cap[c['id']]['postabc_sha256']:raise ValueError('Actual endpoint differs from proof input')
        actual={str(Path(s['path']).resolve()):s['sha256'] for s in c['rtl']['source_sha256']}
        if actual!=sources[c['id']]:raise ValueError('Formal/source manifest mismatch')
    return {'selected_cases':len(expected),'all_selected_pass':True,'all_nine_source_logical_zero_to_candidate_pass':len(expected)==9,
            'role_note':'Runner baseline slot contains actual candidate; not original area baseline.',
            'candidate_side_zinit':False,'proof_report':str(path),'proof_report_sha256':sha(path)}
def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True);ap.add_argument('--suite',type=Path)
    ap.add_argument('--shell-bin',type=Path);ap.add_argument('--mode',choices=['Check','Measure','All','StrictSmoke'],default='All')
    ap.add_argument('--case',action='append');ap.add_argument('--proof-timeout',type=int,default=120)
    ap.add_argument('--proof-work-root',type=Path,help='Short parent for a fresh unique proof workspace; default TEMP/pmuxq. Original workspace is retained after verified archival.')
    args=ap.parse_args(argv);root=args.root.resolve();release=verify_package(root)
    if args.mode=='Check':print(json.dumps({'package_integrity':'PASS','candidate':release['candidate']}));return 0
    if os.name!='nt':raise RuntimeError('Windows native measurement/runtime is required')
    if args.case and args.mode!='StrictSmoke':raise ValueError('Case filtering is only permitted for explicitly non-official StrictSmoke')
    if args.mode=='StrictSmoke' and not args.case:raise ValueError('Select at least one --case for StrictSmoke')
    if args.proof_timeout<=0:raise ValueError('Timeout must be positive')
    fm=load_helper(require(root/'verification/fair_measure.py'));mp=require(root/'cases/official_manifest.json')
    manifest=fm.load_manifest(mp,False)
    if args.mode=='StrictSmoke':
        if not set(args.case)<={c['id'] for c in manifest['cases']}:raise ValueError('Unknown smoke case')
        manifest={**manifest,'cases':[c for c in manifest['cases'] if c['id'] in args.case]}
    exe={a:require(root/a/'bin/yosys.exe') for a in ARMS};abc=require(root/'baseline/bin/yosys-abc.exe')
    runtime=[root/'baseline/bin',root/'optimized/bin'];suite=None;shell=None
    if args.mode in ['All','StrictSmoke']:
        if args.suite is None:raise ValueError('--suite is required for EQY')
        suite=args.suite.resolve();shell=args.shell_bin.resolve() if args.shell_bin else suite/'bin'
        for p in [suite/'bin/yosys.exe',suite/'lib/python3.exe',suite/'bin/eqy-script.py',shell/'sh.exe',shell/'make.exe',shell/'grep.exe']:require(p)
    run=root/'results'/('r'+datetime.now().strftime('%Y%m%d_%H%M%S')+'_'+uuid.uuid4().hex[:8]);run.mkdir(parents=True,exist_ok=False)
    result={'schema':3,'candidate':release['candidate'],'mode':args.mode,'complete':False,'success':False,'contest_score':None,
            'official_nine_run':args.mode!='StrictSmoke','stages':[],'scope':execution_scope(args.mode),'all_local_objective_checks_pass':False,
            'reference_contract':'Yosys read_verilog/proc synthesis semantics; source unspecified FF logical zero; explicit values preserved; no candidate proof repair; no original RTL event-list simulation claim.'}
    save(run/'report.json',result);print('Results: '+str(run),flush=True)
    try:
        measured=None
        proof_dir=None
        if args.mode in ['All','StrictSmoke']:
            proof_parent=(args.proof_work_root if args.proof_work_root else Path(tempfile.gettempdir())/'pmuxq').resolve()
            proof_dir=proof_parent/('q'+uuid.uuid4().hex[:12])/'p'
            external=[mp,suite/'bin/yosys.exe',suite/'bin/eqy-script.py',shell/'sh.exe',root/'verification/endpoint_nozinit_runner.py']
            external += [Path(s['path']) for c in manifest['cases'] for s in c['resolved_sources']]
            external += [run/'capture'/c['id']/'post_abc9.il' for c in manifest['cases']]
            preflight=proof_path_preflight(proof_dir,manifest,read(root/'evidence/strict/report.json'),external)
            # Reserve a writable unique parent before spending time on measurement.
            # The runner itself still creates its new --out directory.
            proof_dir.parent.mkdir(parents=True,exist_ok=False)
            result['proof_workspace']={'execution_directory':str(proof_dir),'archive_directory':str(run/'strict'),
                                       'path_preflight':preflight,'original_workspace_retained':True}
            save(run/'report.json',result)
        if args.mode in ['All','Measure']:
            command=[sys.executable,root/'verification/fair_measure_v3.py','--manifest',mp,'--baseline',exe['baseline'],'--optimized',exe['optimized'],'--abc',abc,'--out',run/'measure',
                     '--baseline-techlib',root/'baseline/share/yosys','--optimized-techlib',root/'optimized/share/yosys']
            for p in runtime:command+=['--runtime-dir',p]
            execution=launch(command,run,child_environment(runtime,abc),run/'measurement.console.log')
            measured=read(run/'measure/report.json')
            if execution['returncode'] or measured['schema']!=3 or not measured['complete'] or not measured['all_measurements_valid']:raise ValueError('Incomplete schema3 measurement')
            result['stages'].append({'stage':'v3_measurement','report':'measure/report.json','sha256':sha(run/'measure/report.json')})
            result['area_and_efficiency_pass']=measured['mean_area_reduction']>=.2 and measured['all_logic_not_increased'] and measured['all_time_and_memory_within_5_percent']
            result['mean_area_reduction']=measured['mean_area_reduction'];save(run/'report.json',result)
        if args.mode in ['All','StrictSmoke']:
            captured=capture_candidate(root,run,manifest,fm,exe['optimized'],abc,runtime)
            if measured:
                for c in manifest['cases']:
                    m=next(x for x in measured['cases'] if x['id']==c['id'])
                    if read(run/'capture'/c['id']/'mapped.netlist.json')!=read(m['untimed_area_captures']['optimized']['netlist']):raise ValueError('PostABC capture differs from separately measured production flow')
            result['stages'].append({'stage':'untimed_postabc_capture','report':'capture/report.json'})
            command=[sys.executable,root/'verification/endpoint_nozinit_runner.py','--manifest',mp,'--suite',suite,'--shell-bin',shell,'--baseline-dir',run/'capture','--baseline-only','--state-invariants','--execute','--depth','20','--timeout',str(args.proof_timeout),'--out',proof_dir]
            if args.mode=='StrictSmoke':
                for cid in args.case:command+=['--case',cid]
            try:
                execution=launch(command,proof_dir.parent,child_environment(runtime,abc),run/'strict.console.log')
                result['proof_workspace']['execution']=execution
            finally:
                if proof_dir.is_dir():
                    archived=archive_proof(proof_dir,run/'strict',run/'strict.archive.json')
                    result['proof_workspace']['archive_index']='strict.archive.json'
                    result['proof_workspace']['archive_index_sha256']=sha(run/'strict.archive.json')
                    result['proof_workspace']['archived_file_count']=archived['file_count']
                save(run/'report.json',result)
            if execution['returncode']:raise ValueError('Strict EQY runner returned failure')
            result['strict']=audit_strict(run/'strict/report.json',captured,manifest,root/'verification/dffeas_formal.v')
            result['stages'].append({'stage':'direct_source_zero_eqy','report':'strict/report.json'})
        result['success']=True
        result['all_local_objective_checks_pass']=args.mode=='All' and result['area_and_efficiency_pass'] and result['strict']['all_nine_source_logical_zero_to_candidate_pass']
    except Exception as exc:result['error']=f'{type(exc).__name__}: {exc}'
    result['complete']=True;result['exit_code']=exit_status(result)
    result['success_means']='Requested operations completed; All/Measure additionally require objective thresholds for exit code 0.'
    save(run/'report.json',result);print(json.dumps(result,ensure_ascii=False,indent=2))
    return result['exit_code']
def entry(argv=None):
    try:return main(argv)
    except Exception as exc:
        # Includes package/tool preconditions before a results directory exists.
        print(json.dumps({'complete':False,'success':False,'exit_code':2,'error':f'{type(exc).__name__}: {exc}'},ensure_ascii=False))
        return 2
if __name__=='__main__':raise SystemExit(entry())
