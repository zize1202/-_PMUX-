"""Audit the predeclared single delivery run without rewriting any run evidence.

This checks provenance even when the end-to-end run failed. It never promotes a
failed proof to PASS and never reexecutes Yosys, ABC, EQY, or the measurement.
"""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import argparse
import os
import importlib.util
import json
import re
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
HISTORY = None  # Set by explicit configuration in main().
EXPERIMENTS=HERE.parent
PACKAGE = HERE / "relocation r30 all"
RUN = None  # Discovered only from the one new actual results directory.
DEST = HERE / "RELOCATION_R30_ALL_AUDIT.json"


def read(p):
    return json.loads(Path(p).read_text(encoding="utf-8-sig"))


def sha(p):
    h = hashlib.sha256()
    with Path(p).open("rb") as f:
        for block in iter(lambda: f.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def under(p, parent=None):
    if parent is None:
        parent = PACKAGE
    Path(p).resolve().relative_to(parent.resolve())


def main():
    global RUN, PACKAGE, HISTORY
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history-root", type=Path, default=os.environ.get("PMUX_HISTORY_ROOT"), required=not os.environ.get("PMUX_HISTORY_ROOT"), help="External unchanged historical evidence; no history is rewritten.")
    args=parser.parse_args()
    HISTORY=args.history_root.resolve()
    require(not DEST.exists(), "Refuse to replace prior audit")
    prepared = read(HERE / "RELOCATION_R30_ALL_PREPARATION.json")
    PACKAGE = Path(prepared["relocation"]).resolve()
    execution_record = read(HERE / "RELOCATION_R30_ALL_EXECUTION_PWSH7.json")
    require(execution_record["finished"] and execution_record["returncode"] == 0, "Actual outer process failed")
    require(sha(HERE / "RELOCATION_R30_ALL_PREPARATION.json") == execution_record["preparation_sha256"], "Actual invocation preparation binding")
    require(len(list((PACKAGE / "results").iterdir())) == 1, "Expected exactly one actual All batch")
    RUN = next((PACKAGE / "results").iterdir())
    require(RUN.is_dir(), "Actual result is not a directory")
    for name, digest in prepared["historical_evidence_sha256"].items():
        require(sha(HISTORY / name) == digest, "Historical evidence changed: " + name)
    for name,digest in prepared["local_preexecution_error_history_sha256"].items():
        require(sha(HERE/name)==digest,"Preparation-only error history changed")
    r23 = read(HISTORY / "RELOCATION_R23_ALL_AUDIT.json")
    for relative, digest in r23["report_hashes"].items():
        require(sha(Path(r23["run"]) / relative) == digest, "Earlier r23 run changed")
    r29 = read(HISTORY / "RELOCATION_R29_ALL_AUDIT.json")
    for relative, digest in r29["report_hashes"].items():
        require(sha(Path(r29["run"]) / relative) == digest, "Earlier r29 run changed")
    index = read(PACKAGE / "PACKAGE_FILES.json")
    identity = prepared["package_identity"]
    for filename, key in [("PACKAGE_FILES.json", "package_index_sha256"),
                          ("ARCHIVE_PATH_INDEX.json", "archive_index_sha256"),
                          ("release.json", "release_sha256")]:
        require(sha(PACKAGE / filename) == identity[key], "Preparation identity changed: " + filename)
    for item in index["files"]:
        under(PACKAGE / item["path"])
        require(sha(PACKAGE / item["path"]) == item["sha256"], "Indexed byte changed: " + item["path"])
    archive = read(PACKAGE / "ARCHIVE_PATH_INDEX.json")
    for item in archive["files"]:
        require(sha(PACKAGE / item["package_path"]) == item["sha256"], "Archive mapping byte changed")
    indexed = {i["path"] for i in index["files"]} | {"PACKAGE_FILES.json"}
    extra = sorted(p.relative_to(PACKAGE).as_posix() for p in PACKAGE.rglob("*")
                   if p.is_file() and p.relative_to(PACKAGE).as_posix() not in indexed
                   and not p.is_relative_to(PACKAGE / "results"))
    require(all(p.startswith("verification/__pycache__/") and p.endswith(".pyc") for p in extra),
            "Unexpected package-root mutation")
    result = read(RUN / "report.json")
    m = read(RUN / "measure/report.json")
    cap = read(RUN / "capture/report.json")
    proof = read(RUN / "strict/report.json")
    require(result["success"] and result["complete"] and result["all_local_objective_checks_pass"] and result["exit_code"] == 0, "All acceptance not met")
    workspace = result["proof_workspace"]
    archive = read(RUN / "strict.archive.json")
    execution_dir = Path(workspace["execution_directory"])
    execution_dir.resolve().relative_to(Path(prepared["proof_work_root"]).resolve())
    require(execution_record["child_only_environment_overrides"] == {"TEMP":prepared["child_temp_root"],"TMP":prepared["child_temp_root"]}, "Child TEMP/TMP binding differs")
    require(execution_record["system_environment_modified"] is False, "Unexpected system environment modification")
    require(execution_dir.is_dir() and workspace["original_workspace_retained"], "Original short proof workspace missing")
    require(workspace["path_preflight"]["status"] == "PASS" and workspace["path_preflight"]["max_units_with_reserve"] < 240, "Path preflight mismatch")
    require(workspace["execution"]["returncode"] == 0 and not workspace["execution"]["timed_out"], "Proof runner failure")
    require(archive["execution_directory"] == str(execution_dir) and archive["archive_directory"] == str(RUN / "strict"), "Archive mapping differs")
    require(archive["raw_report_paths_rewritten"] is False and archive["original_workspace_retained"], "Archive changed raw report paths")
    require(sha(RUN / "strict.archive.json") == workspace["archive_index_sha256"], "Archive index hash mismatch")
    expected_archive = {item["relative_path"]: item["sha256"] for item in archive["files"]}
    require(len(expected_archive) == len(archive["files"]) == archive["file_count"] == workspace["archived_file_count"], "Archive count mismatch")
    def fingerprint(folder):
        values = {}
        for item in folder.rglob("*"):
            require(not item.is_symlink(), "Proof symlink unsupported")
            if item.is_file(): values[item.relative_to(folder).as_posix()] = sha(item)
        return values
    require(fingerprint(execution_dir) == expected_archive == fingerprint(RUN / "strict"), "Archive file set or SHA mismatch")
    require(sha(execution_dir / "report.json") == result["strict"]["proof_report_sha256"] == sha(RUN / "strict/report.json"), "Original/archive strict report mismatch")
    require(result["strict"]["all_nine_source_logical_zero_to_candidate_pass"], "All nine acceptance false")
    prior = read(HISTORY / "RELOCATION_ALL_AUDIT.json")
    for relative, digest in prior["report_hashes"].items():
        require(sha(Path(prior["run"]) / relative) == digest, "Prior failure evidence changed")
    prior_success = read(HISTORY / "RELOCATION_FIXED2_AUDIT.json")
    for relative, digest in prior_success["report_hashes"].items():
        require(sha(Path(prior_success["run"]) / relative) == digest, "Prior successful r20 evidence changed")
    archive_validation = {"file_count": len(expected_archive), "all_actual_file_hashes_and_file_sets_identical": True,
                          "execution_directory": str(execution_dir), "archive_directory": str(RUN / "strict"),
                          "archive_index_sha256": sha(RUN / "strict.archive.json"),
                          "raw_report_paths_preserved": True, "path_preflight_max_units_with_reserve": workspace["path_preflight"]["max_units_with_reserve"]}
    require(result["mode"] == "All" and result["official_nine_run"] and result["complete"], "Not complete All")
    require(m["complete"] and m["all_measurements_valid"] and cap["complete"] and proof["complete"], "Incomplete child batch")
    require(result["stages"][0]["sha256"] == sha(RUN / "measure/report.json"), "Stage report binding")
    sys.path.insert(0, str(PACKAGE / "verification"))
    import fair_measure as fm
    import fair_measure_v3 as v3
    require(sha(PACKAGE / "verification/fair_measure_v3.py") == m["measurement_source_sha256"], "Measure implementation changed")
    require(sha(PACKAGE / "verification/fair_measure.py") == m["v2_helper_source_sha256"], "Measure helper changed")
    release = read(PACKAGE / "release.json")
    require(release["candidate"] == "r30_native_sload" and release["binaries"]["optimized"] == "efebff04cac47ccdfefc3317d4de14b7f727123a7499243ab9da35ba79f596d9", "Not frozen r30")
    for arm in ("baseline", "optimized"):
        under(m["binaries"][arm]["path"])
        require(sha(m["binaries"][arm]["path"]) == release["binaries"][arm] == m["binaries"][arm]["sha256"], "Executable binding")
        under(m["techlibs"][arm]["path"])
        require(fm.tree_fingerprint(m["techlibs"][arm]["path"]) == m["techlibs"][arm], "Techlib binding")
        require(sha(PACKAGE / arm / "bin/yosys-abc.exe") == release["abc_sha256"], "Packaged ABC mismatch")
    require(sha(m["abc"]["path"]) == m["abc"]["sha256"] == release["abc_sha256"], "Requested ABC binding")
    for p in m["runtime_dirs"]:
        under(p)
    expected_cases = m["manifest"]["cases"]
    expected_ids = [c["id"] for c in expected_cases]
    require(len(expected_ids) == 9 and len(set(expected_ids)) == 9, "Not nine unique cases")
    for batch in [m, cap, proof]:
        require([c["id"] for c in batch["cases"]] == expected_ids, "Case order/set differs")
    require(sha(PACKAGE / "cases/official_manifest.json") == m["manifest_sha256"] == proof["manifest_sha256"], "Manifest binding")
    require(len(m["warmup"]) == 10 and all(r["arm"] == "baseline" and r["case_id"] == expected_ids[0] for r in m["warmup"]), "Warmup protocol")
    events = m["events"]
    finished = [e for e in events if e["phase"] == "timed_batch_finished"]
    first_capture = min(e["sequence"] for e in events if e["phase"] == "untimed_capture_start")
    require(len(finished) == 1 and finished[0]["sequence"] < first_capture, "Capture interleaved into timed batch")
    require(all(e["sequence"] < finished[0]["sequence"] for e in events if e["phase"].startswith("timed_" ) and e["phase"] != "timed_batch_finished"), "Timed process after capture boundary")
    rows = []
    total_timed = 0
    for source, case, capture, pcase in zip(expected_cases, m["cases"], cap["cases"], proof["cases"]):
        cid = case["id"]
        sources = {s["path"]: s["sha256"] for s in source["resolved_sources"]}
        for path, digest in sources.items():
            under(path, PACKAGE / "cases")
            require(sha(path) == digest, "Source changed")
        require(fm.recheck_required(case["initial_ratios"], case["initial"]) == case["recheck_triggered"], "Trigger changed")
        require(len(case["rechecks"]) == (5 if case["recheck_triggered"] else 0), "Wrong repeat count")
        require(all([r["arm"] for r in pair] == ["baseline", "optimized"] for pair in [case["initial"]] + case["rechecks"]), "Expected baseline then optimized in every pair")
        require(v3.aggregate_timing(case["initial"], case["rechecks"]) == case["timing_aggregate"], "Aggregate not reproducible")
        timed = [r for r in m["warmup"] if r["case_id"] == cid] + case["initial"] + [r for pair in case["rechecks"] for r in pair]
        total_timed += len(timed)
        expected_text = "read_verilog " + " ".join(fm.yosys_quote(s["path"]) for s in source["resolved_sources"]) + "\n" + f"synth_intel -family cycloneiv -top {source['top']}\n"
        script = RUN / "measure/scripts" / (cid + ".timed.ys")
        require(script.read_text(encoding="utf-8") == expected_text, "Timed script is not exact two-command default flow")
        require(sha(script) == m["script_hashes"][cid]["timed"], "Timed script hash")
        for arm in ("baseline", "optimized"):
            area = case["untimed_area_captures"][arm]
            samples = [r for r in timed if r["arm"] == arm]
            require(v3.validate_capture(source, area, samples) == area["timed_inventory_validation"], "Inventory capture binding")
            require(fm.cell_counts(area["netlist"], source["top"]) == area["cells"] == case["aggregate"][arm + "_cells"], "Actual JSON area mismatch")
            for rec in samples + [area]:
                require(rec["source_sha256"] == sources and rec["binary_sha256"] == release["binaries"][arm], "Record source/binary binding")
                require(sha(rec["command"][0]) == rec["binary_sha256"] and sha(rec["command"][-1]) == rec["script_sha256"], "Actual executable/script mismatch")
                inv_key = "timed_stat_inventory" if rec in samples else "capture_stat_inventory"
                require(v3.parse_top_inventory(rec["log"], source["top"], timed=rec in samples) == rec[inv_key], "Actual log inventory differs")
                require(fm.abc_execution_evidence(rec["log"], Path(rec["command"][0]), release["abc_sha256"]) == rec["actual_abc_invocations"], "Actual ABC path evidence differs")
                for abc in rec["actual_abc_invocations"]:
                    under(abc["resolved_path"])
                    require(sha(abc["resolved_path"]) == release["abc_sha256"], "Executed ABC bytes differ")
        folder = RUN / "capture" / cid
        require(capture["success"] and not capture["execution"]["timed_out"] and capture["execution"]["returncode"] == 0, "Capture execution failed")
        require(sha(capture["execution"]["command"][0]) == release["binaries"]["optimized"], "PostABC exe mismatch")
        require(fm.abc_execution_evidence(capture["execution"]["log"], Path(capture["execution"]["command"][0]), release["abc_sha256"]) == capture["actual_abc_invocations"], "Capture ABC mismatch")
        require(sha(folder / "mapped.netlist.json") == capture["full_json_sha256"], "Capture final JSON hash")
        require(read(folder / "mapped.netlist.json") == read(case["untimed_area_captures"]["optimized"]["netlist"]), "Split/full flow differs")
        require(sha(folder / "mapped.netlist.json") == sha(case["untimed_area_captures"]["optimized"]["netlist"]), "Split/full JSON bytes differ")
        endpoint_sha = sha(folder / "post_abc9.il")
        pfolder = execution_dir / cid
        require(endpoint_sha == capture["postabc_sha256"] == pcase["baseline"]["source_sha256"] == sha(pfolder / "baseline.source.il"), "Proof does not use actual candidate endpoint")
        require({s["path"]: s["sha256"] for s in pcase["rtl"]["source_sha256"]} == sources, "Proof original source differs")
        require(not re.search(r"^\s*(zinit|setundef)\b", (pfolder / "baseline.normalize.ys").read_text(), re.M), "Candidate initialization repaired")
        require(len(pcase["checks"]) == 1, "Unexpected proof count")
        check = pcase["checks"][0]
        require(check["config_sha256"] == sha(pfolder / "rtl_to_baseline.eqy"), "Proof config changed")
        require(check["partition_count"] == 1 and check["only_original_inputs_used_as_partition_inputs"], "Proof cut assumptions")
        require(check["proven_output_bit_counts"] == check["expected_output_bit_counts"], "Proof output coverage")
        require(pcase["baseline"]["ports"] == pcase["rtl"]["ports"], "Candidate/source port interface changed")
        paths = []
        for runys in (pfolder / "rb/strategies").glob("*/induction/run.ys"):
            line = next(x for x in runys.read_text().splitlines() if x.startswith("read_verilog "))
            raw = runys.parent / line.split()[-1]
            resolved = raw.resolve()
            paths.append({"raw_path": str(raw), "raw_path_length": len(str(raw)), "raw_exists": raw.exists(),
                          "resolved_path": str(resolved), "resolved_path_length": len(str(resolved)),
                          "resolved_exists": resolved.is_file(), "resolved_sha256": sha(resolved)})
        passed = pcase["status"] == "PASS" and check["eqy_raw_status"] == "PASS" and check["execution"]["returncode"] == 0 and not check["execution"]["timed_out"] and (pfolder / "rb/PASS").is_file()
        rows.append({"id": cid, "source_sha256": sources, "timed_process_count_including_warmups": len(timed),
                     "recheck_triggered": case["recheck_triggered"], "aggregate": case["aggregate"],
                     "split_full_json_matches_area_capture": True, "actual_endpoint_sha256": endpoint_sha,
                     "proof_endpoint_binding": True, "formal_status": pcase["status"], "formal_pass": passed,
                     "formal_execution": check["execution"], "proof_outcome": check.get("proof_outcome"),
                     "path_diagnostics": paths})
    tc = proof["toolchain"]
    for path, key in [(Path(tc["suite"]) / "bin/yosys.exe", "yosys_sha256"),
                      (Path(tc["suite"]) / "bin/eqy-script.py", "eqy_script_sha256"),
                      (PACKAGE / "verification/endpoint_nozinit_runner.py", "runner_sha256"),
                      (PACKAGE / "verification/dffeas_formal.v", "formal_model_sha256")]:
        require(sha(path) == tc[key], "Formal tool/model mismatch")
    runtime = identity["restricted_model_runtime_sha256"]
    for name, digest in runtime.items():
        require(sha(PACKAGE / "verification" / name) == digest, "Five-file runtime changed")
    for key, name in [("simulation_counterpart_sha256", "dffeas_sload_sim.v"), ("legacy_runner_sha256", "legacy_endpoint_runner.py"), ("legacy_model_sha256", "legacy_dffeas_formal.v")]:
        require(proof["restricted_sclr_contract"][key] == runtime[name], "Actual proof sidecar identity mismatch")
    for pcase in proof["cases"]:
        require(sha(execution_dir / pcase["id"] / "dffeas_formal.v") == runtime["dffeas_formal.v"], "Actual copied formal model differs")
    require(all(c["formal_pass"] for c in rows), "Not every actual proof passed")
    require(all(p["raw_exists"] for c in rows for p in c["path_diagnostics"]), "Actual strategy path unreadable")
    output = {"schema": 1, "audited_utc": datetime.now(timezone.utc).isoformat(), "run": str(RUN),
              "batch_scope": prepared["predeclared_scope"], "prior_r30_main_report_replaced": False,
              "process_returncode_observed": execution_record["returncode"], "all_entrypoint_success": result["success"],
              "all_local_objective_checks_pass": result.get("all_local_objective_checks_pass") is True,
              "all_local_objective_field_present": "all_local_objective_checks_pass" in result,
              "overall_delivery_reproduction_status": "PASS", "original_error": result.get("error"),
              "package_indexed_bytes_unchanged": True, "indexed_file_count": len(index["files"]),
              "package_archive_mapping_count": len(read(PACKAGE / "ARCHIVE_PATH_INDEX.json")["files"]),
              "proof_archive_file_count": len(archive["files"]), "generated_cache_files": extra,
              "package_snapshot_identity": identity, "measurement_and_capture_binding_pass": True,
              "formal_endpoint_binding_all_nine_pass": True, "formal_proof_all_nine_pass": all(c["formal_pass"] for c in rows),
              "formal_pass_count": sum(c["formal_pass"] for c in rows), "timed_process_count": total_timed,
              "restricted_model_runtime_sha256": runtime, "all_copied_models_and_sidecars_bound": True,
              "primitive_model_domains": {c["id"]: c["baseline"]["restricted_primitive_model_domains"] for c in proof["cases"]},
              "all_split_full_json_byte_identical": True, "prior_r23_failed_and_successful_material_preserved": True, "prior_r29_material_preserved": True,
              "proof_work_root":prepared["proof_work_root"], "child_only_environment_overrides":execution_record["child_only_environment_overrides"],
              "system_environment_modified":False,
              "storage_failure_history_sha256":prepared["historical_evidence_sha256"]["RELOCATION_R29_STORAGE_EVENT.json"],
              "timed_finished_sequence": finished[0]["sequence"], "first_untimed_capture_sequence": first_capture,
              "area_and_efficiency_pass": result["area_and_efficiency_pass"], "mean_area_reduction": m["mean_area_reduction"],
              "diagnosis": "Frozen r30 source/binary delivery validated by one actual full All batch through the unchanged pathfix2 entrypoint in a new space-containing directory. All nine proofs pass and every proof file is identically archived. Historical r23 shell/preparation records and earlier r20 results remain preserved; no prior run is relabelled. This does not replace r30_run01.",
              "proof_archive_validation": archive_validation, "prior_failed_run_preserved": True,
              "reference_contract": result["reference_contract"],
              "historical_r23_preparation_error_preserved": str(HISTORY / "RELOCATION_R23_PREPARATION_ALIAS_ERROR.json"),
              "historical_r23_windows_powershell_launch_error_preserved": str(HISTORY / "RELOCATION_R23_ALL_EXECUTION.json"),
              "actual_pwsh7_execution_record_sha256": sha(HERE / "RELOCATION_R30_ALL_EXECUTION_PWSH7.json"),
              "actual_single_all_results_directory_count": 1,
              "source_overlay_count_unchanged": identity["overlay_file_count"],
              "native_full_source_count_unchanged": identity["native_source_after_files_verified"],
              "prior_successful_r20_run_preserved": True,
              "limitations": ["This audits the prepared snapshot listed in package_snapshot_identity; final book archive QA is separately required when books were pending at copy.",
                              "External OSS CAD suite and shell tools are explicit supplied prerequisites, not included portable package tools.",
                              "Timed inventory and untimed JSON agree; no bit-for-bit identity is claimed for an unexported timed netlist."],
              "report_hashes": {p: sha(RUN / p) for p in ["report.json", "measure/report.json", "capture/report.json", "strict/report.json", "strict.archive.json"]},
              "cases": rows}
    DEST.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: output[k] for k in ["overall_delivery_reproduction_status", "package_indexed_bytes_unchanged", "measurement_and_capture_binding_pass", "formal_endpoint_binding_all_nine_pass", "formal_pass_count", "timed_process_count", "mean_area_reduction", "proof_archive_validation"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
