#!/usr/bin/env python3
"""FAQ-aligned paired Yosys measurements; Windows, standard library only.

This tool produces evidence, never a contest score or an EQY PASS.
See README.md for deliberately disclosed interpretations of ambiguous FAQ details.
"""
from __future__ import annotations

import argparse
from collections import Counter
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone


SCHEMA = 2
LUT_TYPES = {"$lut", "cycloneiv_lcell_comb"}
NOT_TYPES = {"$_NOT_", "$not"}
IO_TYPES = {"cycloneiv_io_ibuf", "cycloneiv_io_obuf", "$__inpad", "$__outpad"}
# Explicit nonlogic resources are transparent, not silently counted as LUTs.
OTHER_TYPES = {"altsyncram", "altpll", "cycloneiv_pll", "cycloneiv_ram_block"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def abc_execution_evidence(logfile, yosys_exe, expected_sha256):
    """Bind the ABC actually named by Yosys to the requested binary bytes.

    An ABC environment variable alone does not override every Yosys build.
    The pinned native build logs its sibling executable using a path token.
    Missing or unrecognized commands are unresolved evidence, never a match.
    Hashing happens outside the measured Yosys lifetime.
    """
    commands = re.findall(r'^Running ABC command: (.+)$',
                          Path(logfile).read_text(encoding='utf-8', errors='replace'), re.M)
    result = []
    for command in commands:
        match = re.match(r'"([^"]+)"(?:\s|$)', command)
        if not match:
            raise ValueError('Cannot resolve actual ABC executable from Yosys log')
        token = match.group(1)
        prefix = '<yosys-exe-dir>/'
        if token.startswith(prefix):
            path = Path(yosys_exe).resolve().parent / token[len(prefix):]
        else:
            path = Path(token)
            if not path.is_absolute():
                raise ValueError('Actual ABC executable path is not absolute: ' + token)
        actual = sha256(path)
        if actual != expected_sha256:
            raise ValueError('Actual ABC bytes differ from requested frozen ABC: ' + str(path))
        result.append({'logged_command': command.strip(), 'resolved_path': str(path.resolve()),
                       'sha256': actual, 'matches_requested_binary_bytes': True})
    if not result:
        raise ValueError('No actual ABC invocation found; provenance remains unverified')
    return result


def tree_fingerprint(path):
    root = Path(path).resolve()
    if not root.is_dir():
        raise ValueError(f"Missing technology library: {root}")
    files = {p.relative_to(root).as_posix(): sha256(p)
             for p in sorted(root.rglob('*')) if p.is_file()}
    if not files:
        raise ValueError(f"Empty technology library: {root}")
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {"path": str(root), "sha256": digest, "file_count": len(files), "files": files}


def write_json(path, obj):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def parse_int(value):
    if isinstance(value, int):
        return value
    if isinstance(value, str) and re.fullmatch("[01]+", value):
        return int(value, 2)
    raise ValueError(f"Undefined/non-binary integer parameter: {value!r}")


def cell_counts(path, top):
    """Count actual leaf instances in top hierarchy, without library-definition inflation."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    modules = data["modules"]
    if top not in modules:
        raise ValueError(f"Top {top!r} absent from output netlist")
    counts = Counter(lut=0, inverter=0, dff=0, io=0, other=0, unknown=0, total_leaf_cells=0)
    by_type = Counter()
    unknown = Counter()

    def visit(name, ancestry):
        if name in ancestry:
            raise ValueError("Recursive netlist hierarchy")
        for cell in modules[name].get("cells", {}).values():
            typ = cell["type"]
            child = modules.get(typ)
            attrs = child.get("attributes", {}) if child else {}
            is_primitive = any(parse_int(attrs.get(a, 0)) for a in ("blackbox", "whitebox", "lib_whitebox"))
            if child is not None and not is_primitive and typ not in LUT_TYPES | NOT_TYPES | IO_TYPES | OTHER_TYPES and typ != "dffeas":
                visit(typ, ancestry | {name})
                continue
            by_type[typ] += 1
            counts["total_leaf_cells"] += 1
            if typ in LUT_TYPES:
                counts["lut"] += 1
            elif typ in NOT_TYPES:
                width = parse_int(cell.get("parameters", {}).get("Y_WIDTH", 1)) if typ == "$not" else 1
                if width < 1:
                    raise ValueError("Invalid inverter width")
                counts["inverter"] += width
            elif typ == "dffeas" or typ.startswith(("$_DFF", "$_SDFF", "$_ADFF")):
                counts["dff"] += 1
            elif typ in IO_TYPES:
                counts["io"] += 1
            elif typ in OTHER_TYPES:
                counts["other"] += 1
            else:
                counts["unknown"] += 1
                unknown[typ] += 1

    visit(top, set())
    return {**counts, "logic_units": counts["lut"] + counts["inverter"],
            "cell_types": dict(sorted(by_type.items())), "unknown_types": dict(sorted(unknown.items())),
            "top_parameter_defaults_raw": modules[top].get("parameter_default_values", {}),
            "classification_complete": not unknown, "netlist_sha256": sha256(path)}


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def filetime_int(t):
    return (t.dwHighDateTime << 32) | t.dwLowDateTime


def measure_process(command, cwd, env, timeout, logfile):
    """Same native process handle supplies creation/exit time and main-process peak WS.

    Child (ABC) time is naturally included in Yosys wall time, child memory is not.
    PeakWorkingSetSize is an OS-maintained lifetime high-water mark, not sampled RSS.
    """
    if os.name != "nt":
        raise RuntimeError("Windows required for the specified PeakWorkingSetSize measurement")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    host_start = time.perf_counter()
    peak = 0
    successful_reads = 0
    timed_out = False
    with open(logfile, "wb") as log:
        proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        handle = wintypes.HANDLE(int(proc._handle))
        while True:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                peak = max(peak, counters.PeakWorkingSetSize)
                successful_reads += 1
            returncode = proc.poll()
            if returncode is not None:
                break
            if time.perf_counter() - host_start > timeout:
                timed_out = True
                # Only this owned process tree is terminated; never unrelated Yosys/GUI jobs.
                try:
                    subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                returncode = proc.returncode
                break
            time.sleep(0.005)
        times = [wintypes.FILETIME() for _ in range(4)]
        ok = bool(kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)))
        # One final handle query after termination also captures peaks of very short jobs.
        final_counters = ProcessMemoryCounters()
        final_counters.cb = ctypes.sizeof(final_counters)
        final_peak_read_ok = bool(psapi.GetProcessMemoryInfo(handle, ctypes.byref(final_counters), final_counters.cb))
        final_peak_read_error = 0 if final_peak_read_ok else ctypes.get_last_error()
        if final_peak_read_ok:
            peak = max(peak, final_counters.PeakWorkingSetSize)
            successful_reads += 1
        wall = (filetime_int(times[1]) - filetime_int(times[0])) / 1e7 if ok else None
        if wall is not None and wall <= 0:
            wall = None
    return {"pid": proc.pid, "exit_code": returncode, "timed_out": timed_out,
            "wall_seconds": wall, "peak_working_set_bytes": peak if successful_reads and peak > 0 else None,
            "host_observed_seconds_diagnostic": time.perf_counter() - host_start,
            "successful_peak_counter_reads": successful_reads,
            "final_peak_counter_read_ok": final_peak_read_ok,
            "final_peak_counter_read_error": final_peak_read_error,
            "measurement_valid": wall is not None and peak > 0 and successful_reads > 0 and final_peak_read_ok,
            "command": [str(x) for x in command]}


def yosys_quote(path):
    s = str(path).replace("\\", "/")
    if any(c in s for c in ('"', "\n", "\r")):
        raise ValueError("Unsupported quote/newline in path")
    return '"' + s + '"'


def load_manifest(path, development):
    path = Path(path).resolve()
    manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    cases = manifest.get("cases", [])
    if not cases:
        raise ValueError("No cases supplied; no measurements or formal scores can be fabricated")
    if not development and (len(cases) != 9 or manifest.get("official_cases_confirmed") is not True):
        raise ValueError("Formal mode requires exactly 9 source-confirmed official cases; use --development for self-built inputs")
    ids = set()
    for case in cases:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", case["id"]) or case["id"] in ids:
            raise ValueError("Case IDs must be unique safe ASCII identifiers")
        ids.add(case["id"])
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", case["top"]):
            raise ValueError("Unsupported top identifier")
        if case.get("parameter_overrides"):
            raise ValueError("Parameter overrides prohibited: use RTL default parameters")
        sources = case.get("sources") or [case["path"]]
        case["resolved_sources"] = []
        for source in sources:
            p = (path.parent / source).resolve()
            if not p.is_file():
                raise FileNotFoundError(p)
            case["resolved_sources"].append({"path": str(p), "sha256": sha256(p)})
        declared = case.get("source_sha256", {})
        for item in case["resolved_sources"]:
            expected = declared.get(Path(item["path"]).name)
            if expected and expected.lower() != item["sha256"]:
                raise ValueError(f"Source hash mismatch: {item['path']}")
        source_basenames = {Path(item["path"]).name for item in case["resolved_sources"]}
        if len(source_basenames) != len(case["resolved_sources"]):
            raise ValueError("Source basenames must be unique within each case")
        if not development and (not case.get("provenance") or set(declared) != source_basenames
                                or "default_parameters" not in case):
            raise ValueError("Formal cases require provenance and SHA-256 for every source basename")
    return manifest


def prepare_script(case, root):
    # Identical absolute script path and content for both executables, all runs.
    scriptdir = root / "scripts"
    scriptdir.mkdir(exist_ok=True)
    lines = []
    for source in case["resolved_sources"]:
        sv = " -sv" if source["path"].lower().endswith(".sv") else ""
        lines.append(f"read_verilog{sv} {yosys_quote(source['path'])}")
    lines += [f"synth_intel -family cycloneiv -top {case['top']}",
              "stat", "write_json mapped.netlist.json", ""]
    path = scriptdir / (case["id"] + ".ys")
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


def summarize_pair(runs):
    if len(runs) != 2 or any(not r["valid"] for r in runs):
        return {"valid": False, "reason": "failed_or_incomplete_run"}
    baseline, optimized = runs
    b, o = baseline["cells"], optimized["cells"]
    return {"valid": True,
            "time_ratio": optimized["wall_seconds"] / baseline["wall_seconds"],
            "memory_ratio": optimized["peak_working_set_bytes"] / baseline["peak_working_set_bytes"],
            "area_reduction": (b["logic_units"] - o["logic_units"]) / b["logic_units"] if b["logic_units"] else None}


def recheck_required(pair, runs):
    return (pair["valid"] and max(r["wall_seconds"] for r in runs) <= 10.0
            and (pair["time_ratio"] > 1.05 or pair["memory_ratio"] > 1.05))


def aggregate_case(initial, rechecks):
    chosen = rechecks if rechecks else [initial]
    flat = [r for pair in chosen for r in pair]
    if any(not r["valid"] for r in flat):
        return {"valid": False, "reason": "any_selected_sample_failed"}
    # Never choose a favorable run or the best observed area.
    for arm_index in (0, 1):
        area_signatures = {json.dumps(pair[arm_index]["cells"]["cell_types"], sort_keys=True) for pair in [initial] + rechecks}
        if len(area_signatures) != 1:
            return {"valid": False, "reason": "nondeterministic_resource_counts"}
    btime = statistics.mean(pair[0]["wall_seconds"] for pair in chosen)
    otime = statistics.mean(pair[1]["wall_seconds"] for pair in chosen)
    bmem = statistics.mean(pair[0]["peak_working_set_bytes"] for pair in chosen)
    omem = statistics.mean(pair[1]["peak_working_set_bytes"] for pair in chosen)
    b, o = initial[0]["cells"], initial[1]["cells"]
    tr, mr = otime / btime, omem / bmem
    reduction = (b["logic_units"] - o["logic_units"]) / b["logic_units"] if b["logic_units"] else None
    return {"valid": True, "basis": "five_recheck_pairs_mean_excluding_initial" if rechecks else "initial_pair",
            "pairs_in_aggregate": len(chosen), "baseline_mean_wall_seconds": btime,
            "optimized_mean_wall_seconds": otime, "baseline_mean_peak_ws_bytes": bmem,
            "optimized_mean_peak_ws_bytes": omem, "time_ratio": tr, "memory_ratio": mr,
            "baseline_cells": b, "optimized_cells": o, "area_reduction": reduction,
            "logic_not_increased": o["logic_units"] <= b["logic_units"],
            "time_within_5_percent": tr <= 1.05, "memory_within_5_percent": mr <= 1.05,
            "time_above_15_percent": tr > 1.15, "memory_above_15_percent": mr > 1.15,
            "io_increase_needs_review": o["io"] > b["io"],
            "optimized_within_60_seconds": otime <= 60,
            "optimized_peak_ws_within_2_decimal_GB": omem <= 2_000_000_000,
            "eqy_status": "NOT_RUN_BY_THIS_TOOL"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--optimized", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--runtime-dir", type=Path, action="append", default=[])
    parser.add_argument("--abc", type=Path)
    parser.add_argument("--baseline-techlib", type=Path)
    parser.add_argument("--optimized-techlib", type=Path)
    parser.add_argument("--development", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    manifest = load_manifest(args.manifest, args.development)
    binaries = {"baseline": args.baseline.resolve(), "optimized": args.optimized.resolve()}
    for exe in binaries.values():
        if not exe.is_file():
            raise FileNotFoundError(exe)
    root = args.out.resolve()
    if root.exists() and any(root.iterdir()):
        raise ValueError("Output directory must be empty; previous measurements are never overwritten")
    root.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(str(p.resolve()) for p in args.runtime_dir) + os.pathsep + env.get("PATH", "")
    if not args.development and not all((args.abc, args.baseline_techlib, args.optimized_techlib)):
        raise ValueError("Official measurements require --abc and both --*-techlib paths for frozen provenance")
    abc_info = None
    if args.abc:
        env["ABC"] = str(args.abc.resolve())
        abc_info = {"path": env["ABC"], "sha256": sha256(args.abc)}
    techlibs = {}
    for arm, path in (("baseline", args.baseline_techlib), ("optimized", args.optimized_techlib)):
        if path:
            techlibs[arm] = tree_fingerprint(path)
    if len(techlibs) == 2 and techlibs['baseline']['sha256'] != techlibs['optimized']['sha256']:
        raise ValueError("Technology library contents differ between arms")
    report = {"schema": SCHEMA, "created_utc": datetime.now(timezone.utc).isoformat(),
              "mode": "DEVELOPMENT_NOT_OFFICIAL" if args.development else "OFFICIAL_INPUT_MEASUREMENTS_PENDING_EQY_REVIEW",
              "contest_score": None, "manifest_sha256": sha256(args.manifest), "manifest": manifest,
              "measurement_source_sha256": sha256(__file__), "timeout_seconds": args.timeout,
              "runtime_dirs": [str(p.resolve()) for p in args.runtime_dir],
              "policy": {"order": "baseline_then_optimized", "warmup": "first_case_baseline_10_once_at_test_start",
                         "short_case": "max(initial_baseline_wall,initial_optimized_wall)<=10s",
                         "recheck_trigger": "initial_time_ratio>1.05 OR initial_memory_ratio>1.05",
                         "recheck": "exactly_5_complete_pairs_no_early_stop; arithmetic_means_per_arm; exclude_initial",
                         "metric": "GetProcessTimes exit-creation; GetProcessMemoryInfo PeakWorkingSetSize main Yosys only",
                         "logic": "LUT+inverter scalar output units; resource inventory from actual write_json",
                         "security_software": "UNCHANGED", "eqy": "separate_not_included"},
              "binaries": {}, "abc": abc_info, "techlibs": techlibs,
              "abc_binding_policy": "Actual quoted ABC command in every Yosys log must resolve to identical requested binary bytes; executable paths may differ",
              "warmup": [], "cases": [], "complete": False}
    for arm, exe in binaries.items():
        version = subprocess.run([str(exe), "-V"], env=env, capture_output=True, timeout=20,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        if version.returncode != 0:
            raise RuntimeError(f"{arm} -V failed: {version.returncode}; runtime DLL paths may be missing")
        report["binaries"][arm] = {"path": str(exe), "sha256": sha256(exe),
                                    "version": version.stdout.decode("utf-8", errors="replace").strip()}
    scripts = {c["id"]: prepare_script(c, root) for c in manifest["cases"]}
    script_hashes = {case_id: sha256(script) for case_id, script in scripts.items()}
    report['script_hashes'] = script_hashes
    write_json(root / "report.json", report)

    def run(case, arm, label):
        folder = root / label / case["id"] / arm
        folder.mkdir(parents=True)
        script = scripts[case["id"]]
        r = {"arm": arm, "case_id": case["id"], "label": label,
             "script_sha256": sha256(script), "binary_sha256": report["binaries"][arm]["sha256"],
             "log": str(folder / "yosys.log"), "valid": False}
        try:
            # Input immutability checked outside measured process duration.
            for source in case["resolved_sources"]:
                if sha256(source["path"]) != source["sha256"]:
                    raise RuntimeError("Source file changed during experiment")
            if sha256(binaries[arm]) != report["binaries"][arm]["sha256"]:
                raise RuntimeError("Binary changed during experiment")
            if sha256(script) != script_hashes[case['id']]:
                raise RuntimeError("Frozen synthesis script changed")
            if abc_info and sha256(abc_info['path']) != abc_info['sha256']:
                raise RuntimeError("Frozen shared ABC changed")
            r.update(measure_process([str(binaries[arm]), "-s", str(script)], folder, env, args.timeout, folder / "yosys.log"))
            if r["timed_out"]:
                r["error"] = "timeout"
            elif r["exit_code"] != 0:
                r["error"] = "nonzero_exit"
            elif not r["measurement_valid"]:
                r["error"] = "missing_native_metrics"
            else:
                if abc_info:
                    r['actual_abc_invocations'] = abc_execution_evidence(
                        folder / 'yosys.log', binaries[arm], abc_info['sha256'])
                r["cells"] = cell_counts(folder / "mapped.netlist.json", case["top"])
                defaults = r["cells"]["top_parameter_defaults_raw"]
                for parameter, expected in case.get("default_parameters", {}).items():
                    if not isinstance(expected, int):
                        raise ValueError(f"Default parameter {parameter} must be a verified integer")
                    if parameter not in defaults or parse_int(defaults[parameter]) != expected:
                        raise ValueError(f"RTL default parameter mismatch: {parameter}")
                r["valid"] = r["cells"]["classification_complete"]
                if not r["valid"]:
                    r["error"] = "unclassified_cells_area_unresolved"
        except Exception as exc:
            r["error"] = f"{type(exc).__name__}: {exc}"
        write_json(folder / "measurement.json", r)
        print(f"{label} {case['id']} {arm}: {'OK' if r['valid'] else r['error']}", flush=True)
        return r

    for index in range(10):
        r = run(manifest["cases"][0], "baseline", f"warmup_{index + 1:02}")
        report["warmup"].append(r)
        write_json(root / "report.json", report)
        if not r["valid"]:
            report["error"] = "warmup_failed_no_formal_measurement"
            write_json(root / "report.json", report)
            return 2
    for case in manifest["cases"]:
        item = {"id": case["id"], "initial": [], "rechecks": []}
        report["cases"].append(item)
        item["initial"] = [run(case, arm, "initial") for arm in ("baseline", "optimized")]
        pair = summarize_pair(item["initial"])
        item["initial_ratios"] = pair
        item["recheck_triggered"] = recheck_required(pair, item["initial"])
        if item["recheck_triggered"]:
            for index in range(5):
                item["rechecks"].append([run(case, arm, f"recheck_{index + 1:02}") for arm in ("baseline", "optimized")])
                write_json(root / "report.json", report)
        item["aggregate"] = aggregate_case(item["initial"], item["rechecks"])
        write_json(root / "report.json", report)
    aggregates = [c["aggregate"] for c in report["cases"]]
    report["complete"] = True
    report["all_measurements_valid"] = all(a["valid"] for a in aggregates)
    for arm, original in techlibs.items():
        if tree_fingerprint(original['path'])['sha256'] != original['sha256']:
            report['all_measurements_valid'] = False
            report['error'] = 'Technology library changed during experiment: ' + arm
    if report["all_measurements_valid"]:
        reductions = [a["area_reduction"] for a in aggregates]
        report["mean_area_reduction"] = statistics.mean(reductions) if all(r is not None for r in reductions) else None
        report["all_logic_not_increased"] = all(a["logic_not_increased"] for a in aggregates)
        report["all_time_and_memory_within_5_percent"] = all(a["time_within_5_percent"] and a["memory_within_5_percent"] for a in aggregates)
    report["remaining_validation"] = ["official nine-case provenance and default RTL parameters",
                                      "baseline/optimized toolchain identity incl. ABC and dirty patches",
                                      "post-ABC $lut endpoint EQY final PASS with documented initialization",
                                      "official interpretation of disclosed warmup/recheck details"]
    write_json(root / "report.json", report)
    return 0 if report["all_measurements_valid"] else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ERROR: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(2)
