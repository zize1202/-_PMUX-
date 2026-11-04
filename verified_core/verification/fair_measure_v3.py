#!/usr/bin/env python3
"""C145 command-scope lifetime measurements; area export is a later untimed batch.

v2 evidence is never rewritten. This is a measurement-scope revision, not an
algorithm improvement. C145 lists read_verilog and synth_intel; C92 does not
explicitly resolve whether export is part of every possible official workflow.
"""
from __future__ import annotations
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

import fair_measure as v2

SCHEMA = 3
ARMS = ("baseline", "optimized")


def command_hash(command):
    return hashlib.sha256(json.dumps([str(x) for x in command], ensure_ascii=False,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def prepare_scripts(case, root):
    """One frontend command for all same-language sources, then synth_intel.

    Mixed .v/.sv is rejected instead of changing the language of any source.
    Both arms use the same path and bytes. No parameter overrides are accepted.
    """
    languages = {Path(s["path"]).suffix.lower() == ".sv" for s in case["resolved_sources"]}
    if len(languages) != 1:
        raise ValueError("Mixed Verilog/SystemVerilog sources require an explicitly reviewed frontend script")
    read = "read_verilog" + (" -sv" if True in languages else "")
    read += " " + " ".join(v2.yosys_quote(s["path"]) for s in case["resolved_sources"])
    text = read + "\n" + f"synth_intel -family cycloneiv -top {case['top']}\n"
    folder = Path(root) / "scripts"
    folder.mkdir(exist_ok=True)
    timed = folder / (case["id"] + ".timed.ys")
    capture = folder / (case["id"] + ".untimed_area.ys")
    timed.write_text(text, encoding="utf-8")
    capture.write_text(text + "write_json mapped.netlist.json\n", encoding="utf-8")
    return {"timed": timed, "capture": capture}


def parse_top_inventory(logfile, top, timed=True):
    """Fail closed on missing/ambiguous/incomplete internal synth statistics.

    Counts are direct top cell *instances*. They are never called scalar logic
    area: e.g. $not requires Y_WIDTH from the separate JSON for that calculation.
    """
    text = Path(logfile).read_text(encoding="utf-8", errors="strict")
    if "End of script." not in text or "Found and reported 0 problems." not in text:
        raise ValueError("Missing clean synthesis completion/check evidence")
    events = list(re.finditer(r"^(\d+(?:\.\d+)*)\. Printing statistics\.\s*$", text, re.M))
    if len(events) != 1 or not events[0].group(1).startswith("2."):
        raise ValueError("Expected exactly one synth_intel-internal statistics event")
    if timed and re.search(r"Executing (?:JSON|RTLIL|Verilog) backend", text):
        raise ValueError("Export backend found inside timed scope")
    tail = text[events[0].end():]
    boundary = re.search(r"^\d+(?:\.\d+)*\. (?:Executing|Printing)\b|^End of script\.", tail, re.M)
    if not boundary:
        raise ValueError("Cannot delimit statistics event")
    stats = tail[:boundary.start()]
    headings = list(re.finditer(r"^=== (.+) ===\s*$", stats, re.M))
    # These official cases are flat at the mapping endpoint. Do not silently
    # equate local counts with a recursive leaf count for an unfamiliar design.
    if len(headings) != 1 or headings[0].group(1) != top:
        raise ValueError("Expected one flat top statistics table; hierarchy unresolved")
    table = stats[headings[0].end():]
    totals = list(re.finditer(r"^\s+(\d+) cells\s*$", table, re.M))
    if len(totals) != 1:
        raise ValueError("Missing or duplicate total cells row")
    total = int(totals[0].group(1))
    types = {}
    for line in table[totals[0].end():].splitlines():
        if not line.strip():
            continue
        row = re.fullmatch(r"\s+(\d+)\s+(\S+)\s*", line)
        if not row:
            raise ValueError("Unrecognized cell inventory row: " + line)
        number, typ = int(row.group(1)), row.group(2)
        if number <= 0 or typ in types:
            raise ValueError("Duplicate or nonpositive cell inventory row")
        types[typ] = number
    if sum(types.values()) != total:
        raise ValueError("Cell inventory does not sum to total")
    payload = {"top": top, "total_direct_cells": total, "cell_types": dict(sorted(types.items()))}
    return {**payload, "inventory_sha256": command_hash([json.dumps(payload, sort_keys=True)]),
            "basis": "synth_intel_builtin_stat_direct_top_instances", "log_sha256": v2.sha256(logfile)}


def summarize_pair(runs):
    if len(runs) != 2 or [r["arm"] for r in runs] != list(ARMS) or any(not r["valid"] for r in runs):
        return {"valid": False, "reason": "failed_or_incomplete_timed_pair"}
    return {"valid": True, "time_ratio": runs[1]["wall_seconds"] / runs[0]["wall_seconds"],
            "memory_ratio": runs[1]["peak_working_set_bytes"] / runs[0]["peak_working_set_bytes"]}


def aggregate_timing(initial, rechecks):
    if rechecks and len(rechecks) != 5:
        return {"valid": False, "reason": "recheck_requires_exactly_five_complete_pairs"}
    if any(not summarize_pair(p)["valid"] for p in [initial] + rechecks):
        return {"valid": False, "reason": "failed_timed_sample"}
    for arm_index in (0, 1):
        signatures = {p[arm_index]["timed_stat_inventory"]["inventory_sha256"] for p in [initial] + rechecks}
        if len(signatures) != 1:
            return {"valid": False, "reason": "nondeterministic_timed_inventory"}
    chosen = rechecks or [initial]
    bt, ot = [statistics.mean(p[i]["wall_seconds"] for p in chosen) for i in (0, 1)]
    bm, om = [statistics.mean(p[i]["peak_working_set_bytes"] for p in chosen) for i in (0, 1)]
    tr, mr = ot / bt, om / bm
    return {"valid": True, "basis": "five_recheck_pairs_mean_excluding_initial" if rechecks else "initial_pair",
            "pairs_in_aggregate": len(chosen), "baseline_mean_wall_seconds": bt,
            "optimized_mean_wall_seconds": ot, "baseline_mean_peak_ws_bytes": bm,
            "optimized_mean_peak_ws_bytes": om, "time_ratio": tr, "memory_ratio": mr,
            "time_within_5_percent": tr <= 1.05, "memory_within_5_percent": mr <= 1.05,
            "time_above_15_percent": tr > 1.15, "memory_above_15_percent": mr > 1.15,
            "optimized_within_60_seconds": ot <= 60, "optimized_peak_ws_within_2_decimal_GB": om <= 2_000_000_000}


def execute_batches(report, cases, run_timed, run_capture, checkpoint, event):
    """Fixed schedule shared by production and orchestration regression tests."""
    for i in range(10):
        r = run_timed(cases[0], "baseline", f"warmup_{i+1:02}")
        report["warmup"].append(r)
        checkpoint()
        if not r["valid"]:
            raise RuntimeError("Warmup failed; no official measurement or area capture")
    for case in cases:
        item = {"id": case["id"], "initial": [], "rechecks": []}
        report["cases"].append(item)
        item["initial"] = [run_timed(case, arm, "initial") for arm in ARMS]
        item["initial_ratios"] = summarize_pair(item["initial"])
        item["recheck_triggered"] = v2.recheck_required(item["initial_ratios"], item["initial"])
        if item["recheck_triggered"]:
            for i in range(5):
                item["rechecks"].append([run_timed(case, arm, f"recheck_{i+1:02}") for arm in ARMS])
                checkpoint()
        item["timing_aggregate"] = aggregate_timing(item["initial"], item["rechecks"])
        checkpoint()
    event("timed_batch_finished")
    report["timed_batch_finished"] = True
    checkpoint()
    # No exporter process can run until every case's timed/recheck batch ends.
    for case, item in zip(cases, report["cases"]):
        item["untimed_area_captures"] = {arm: run_capture(case, arm) for arm in ARMS}
        checkpoint()
    event("untimed_area_batch_finished")


def validate_capture(case, capture, timed_runs):
    if not capture["valid"] or not timed_runs or any(not r["valid"] for r in timed_runs):
        raise ValueError("Incomplete capture or timed evidence")
    data = json.loads(Path(capture["netlist"]).read_text(encoding="utf-8"))
    direct = dict(sorted(Counter(c["type"] for c in data["modules"][case["top"]].get("cells", {}).values()).items()))
    if direct != capture["cells"]["cell_types"]:
        raise ValueError("Direct top and leaf inventory differ; hierarchy unresolved")
    for record in [capture] + timed_runs:
        if command_hash(record["command"]) != record["command_sha256"]:
            raise ValueError("Command hash mismatch")
        actual_abc = record.get("actual_abc_invocations", [])
        if not actual_abc or any(x["sha256"] != record["abc_sha256"] for x in actual_abc):
            raise ValueError("Actual ABC evidence mismatch")
        inventory = record.get("timed_stat_inventory", record.get("capture_stat_inventory"))
        if inventory["cell_types"] != direct or inventory["total_direct_cells"] != sum(direct.values()):
            raise ValueError("Timed/capture/JSON inventory mismatch")
        for key in ("binary_sha256", "source_sha256", "abc_sha256", "techlib_sha256", "timed_script_sha256"):
            if record[key] != capture[key]:
                raise ValueError("Provenance mismatch: " + key)
    return {"valid": True, "matched_timed_runs_including_warmup": len(timed_runs),
            "matched_inventory": direct,
            "scope": "Separate execution inventory agreement, not an identical-netlist or equivalence proof"}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    for flag in ("manifest", "baseline", "optimized", "out", "abc", "baseline-techlib", "optimized-techlib"):
        p.add_argument("--" + flag, type=Path, required=True)
    p.add_argument("--runtime-dir", type=Path, action="append", default=[])
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--development", action="store_true")
    args = p.parse_args(argv)
    if args.timeout <= 0:
        p.error("--timeout must be positive")
    manifest = v2.load_manifest(args.manifest, args.development)
    root = args.out.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("Output must be new or empty; previous v2/v3 evidence is never overwritten")
    root.mkdir(parents=True, exist_ok=True)
    binaries = {a: getattr(args, a).resolve() for a in ARMS}
    abc = {"path": str(args.abc.resolve()), "sha256": v2.sha256(args.abc)}
    techlibs = {a: v2.tree_fingerprint(getattr(args, a + "_techlib")) for a in ARMS}
    if techlibs["baseline"]["sha256"] != techlibs["optimized"]["sha256"]:
        raise ValueError("Technology library contents differ")
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(str(x.resolve()) for x in args.runtime_dir) + os.pathsep + env.get("PATH", "")
    env["ABC"] = abc["path"]
    report = {"schema": SCHEMA, "created_utc": datetime.now(timezone.utc).isoformat(),
              "mode": "DEVELOPMENT_NOT_OFFICIAL" if args.development else "OFFICIAL_INPUT_MEASUREMENTS_PENDING_EQY_REVIEW",
              "measurement_scope": "C145_read_verilog_then_synth_intel_process_lifetime_no_added_stat_or_export",
              "contest_score": None, "manifest": manifest, "manifest_sha256": v2.sha256(args.manifest),
              "measurement_source_sha256": v2.sha256(__file__), "v2_helper_source_sha256": v2.sha256(v2.__file__),
              "abc": abc, "techlibs": techlibs, "binaries": {}, "runtime_dirs": [str(x.resolve()) for x in args.runtime_dir],
              "timeout_seconds": args.timeout, "events": [], "warmup": [], "cases": [], "complete": False,
              "timed_batch_finished": False, "policy": {
                  "order": "baseline_then_optimized", "warmup": "first_case_baseline_10_once_at_test_start",
                  "recheck": "initial_ratio>1.05; max_initial_wall<=10s; exactly5_complete_pairs; per_arm_means_exclude_initial",
                  "metric": "Same run GetProcessTimes exit-creation and main Yosys PeakWorkingSetSize; ABC wall included",
                  "capture": "All timed cases/rechecks finish first, then one untimed JSON capture per case/arm",
                  "area": "Separate JSON LUT+scalar inverter classification; matched to every timed builtin-stat inventory",
                  "interpretation": "C145 displayed commands; C92 broad full-flow wording does not explicitly prohibit export",
                  "v2_comparison": "v2 was export-inclusive; keep separate tables, never attribute scope differences to algorithm gain",
                  "security_software": "UNCHANGED", "eqy": "NOT_RUN_BY_THIS_TOOL"}}
    for arm, exe in binaries.items():
        version = subprocess.run([str(exe), "-V"], env=env, capture_output=True, timeout=20,
                                 creationflags=subprocess.CREATE_NO_WINDOW, check=True)
        report["binaries"][arm] = {"path": str(exe), "sha256": v2.sha256(exe),
                                    "version": version.stdout.decode("utf-8", errors="replace").strip()}
    scripts = {c["id"]: prepare_scripts(c, root) for c in manifest["cases"]}
    hashes = {cid: {kind: v2.sha256(path) for kind, path in paths.items()} for cid, paths in scripts.items()}
    report["script_hashes"] = hashes
    def checkpoint():
        v2.write_json(root / "report.json", report)
    def event(phase, **fields):
        report["events"].append({"sequence": len(report["events"]), "phase": phase,
                                 "utc": datetime.now(timezone.utc).isoformat(), **fields})
    def frozen(case, arm, kind):
        for source in case["resolved_sources"]:
            if v2.sha256(source["path"]) != source["sha256"]:
                raise RuntimeError("Source changed")
        if v2.sha256(binaries[arm]) != report["binaries"][arm]["sha256"]:
            raise RuntimeError("Yosys executable changed")
        if v2.sha256(abc["path"]) != abc["sha256"]:
            raise RuntimeError("Shared ABC changed")
        for which in ("timed", kind):
            if v2.sha256(scripts[case["id"]][which]) != hashes[case["id"]][which]:
                raise RuntimeError("Frozen script changed")
    def check_libraries():
        for lib in techlibs.values():
            if v2.tree_fingerprint(lib["path"])["sha256"] != lib["sha256"]:
                raise RuntimeError("Technology library changed")
    def run(case, arm, label, capture=False):
        if capture and not report["timed_batch_finished"]:
            raise RuntimeError("Area exporter attempted before timed batch completed")
        kind = "capture" if capture else "timed"
        folder = root / label / case["id"] / arm
        folder.mkdir(parents=True)
        script = scripts[case["id"]][kind]
        command = [str(binaries[arm]), "-s", str(script)]
        record = {"arm": arm, "case_id": case["id"], "label": label, "valid": False,
                  "script_sha256": hashes[case["id"]][kind], "timed_script_sha256": hashes[case["id"]]["timed"],
                  "binary_sha256": report["binaries"][arm]["sha256"], "abc_sha256": abc["sha256"],
                  "source_sha256": {s["path"]: s["sha256"] for s in case["resolved_sources"]},
                  "techlib_sha256": techlibs[arm]["sha256"], "command": command, "command_sha256": command_hash(command),
                  "log": str(folder / "yosys.log"), "included_in_efficiency": not capture and not label.startswith("warmup"),
                  "execution_scope": "untimed_area_reexecution" if capture else "two_command_synthesis_lifetime"}
        event("untimed_capture_start" if capture else "timed_start", case=case["id"], arm=arm, label=label)
        try:
            frozen(case, arm, kind)
            if capture:
                with (folder / "yosys.log").open("wb") as log:
                    result = subprocess.run(command, cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT,
                                            timeout=args.timeout, creationflags=subprocess.CREATE_NO_WINDOW)
                record["exit_code"] = result.returncode
                if result.returncode:
                    raise RuntimeError("Untimed capture nonzero exit")
            else:
                record.update(v2.measure_process(command, folder, env, args.timeout, folder / "yosys.log"))
                if record["timed_out"] or record["exit_code"] or not record["measurement_valid"]:
                    raise RuntimeError("Timed process failed or native metrics missing")
            frozen(case, arm, kind)
            record["actual_abc_invocations"] = v2.abc_execution_evidence(folder/"yosys.log", binaries[arm], abc["sha256"])
            inventory = parse_top_inventory(folder/"yosys.log", case["top"], timed=not capture)
            record["capture_stat_inventory" if capture else "timed_stat_inventory"] = inventory
            if capture:
                record["netlist"] = str(folder/"mapped.netlist.json")
                record["cells"] = v2.cell_counts(record["netlist"], case["top"])
                if not record["cells"]["classification_complete"]:
                    raise ValueError("Unclassified cells; area unresolved")
                defaults = record["cells"]["top_parameter_defaults_raw"]
                for name, expected in case.get("default_parameters", {}).items():
                    if not isinstance(expected, int) or name not in defaults or v2.parse_int(defaults[name]) != expected:
                        raise ValueError("RTL default parameter mismatch: " + name)
            record["valid"] = True
        except Exception as error:
            record["error"] = f"{type(error).__name__}: {error}"
        event("untimed_capture_end" if capture else "timed_end", case=case["id"], arm=arm, label=label, valid=record["valid"])
        v2.write_json(folder/("capture.json" if capture else "measurement.json"), record)
        print(f"{label} {case['id']} {arm}: {'OK' if record['valid'] else record['error']}", flush=True)
        return record
    checkpoint()
    try:
        def capture(case, arm):
            # Once between all timed processes and all capture processes.
            if not any(e["phase"] == "untimed_capture_start" for e in report["events"]):
                check_libraries()
            return run(case, arm, "untimed_area", capture=True)
        execute_batches(report, manifest["cases"], run, capture, checkpoint, event)
        for case, item in zip(manifest["cases"], report["cases"]):
            item["area_state"] = "UNVERIFIED"
            try:
                for arm in ARMS:
                    timed = [r for pair in [item["initial"]]+item["rechecks"] for r in pair if r["arm"] == arm]
                    timed += [r for r in report["warmup"] if r["case_id"] == case["id"] and r["arm"] == arm]
                    cap = item["untimed_area_captures"][arm]
                    cap["timed_inventory_validation"] = validate_capture(case, cap, timed)
                b, o = [item["untimed_area_captures"][arm]["cells"] for arm in ARMS]
                item["aggregate"] = {**item["timing_aggregate"], "area_valid": True,
                    "area_basis": "separate_untimed_JSON_matched_to_all_timed_top_inventories",
                    "baseline_cells": b, "optimized_cells": o,
                    "area_reduction": (b["logic_units"]-o["logic_units"])/b["logic_units"] if b["logic_units"] else None,
                    "logic_not_increased": o["logic_units"] <= b["logic_units"],
                    "io_increase_needs_review": o["io"] > b["io"], "eqy_status": "NOT_RUN_BY_THIS_TOOL"}
                item["area_state"] = "CAPTURED_AND_INVENTORY_MATCHED"
            except Exception as error:
                item["aggregate"] = {**item["timing_aggregate"], "valid": False, "area_valid": False,
                                     "area_error": f"{type(error).__name__}: {error}"}
            checkpoint()
        check_libraries()
        for case in manifest["cases"]:
            for arm in ARMS:
                frozen(case, arm, "capture")
        report["all_timing_valid"] = all(x["timing_aggregate"]["valid"] for x in report["cases"])
        report["all_area_valid"] = all(x["aggregate"]["area_valid"] for x in report["cases"])
        report["all_measurements_valid"] = report["all_timing_valid"] and report["all_area_valid"]
        report["complete"] = True
        if report["all_measurements_valid"]:
            aggregates = [x["aggregate"] for x in report["cases"]]
            reductions = [a["area_reduction"] for a in aggregates]
            report["mean_area_reduction"] = statistics.mean(reductions) if all(r is not None for r in reductions) else None
            report["all_logic_not_increased"] = all(a["logic_not_increased"] for a in aggregates)
            report["all_time_and_memory_within_5_percent"] = all(a["time_within_5_percent"] and a["memory_within_5_percent"] for a in aggregates)
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        report["all_measurements_valid"] = False
    checkpoint()
    return 0 if report.get("all_measurements_valid") else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ERROR: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(2)
