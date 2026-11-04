"""Isolated r30 restricted-SCLR/SLOAD EQY runner; r29 is unchanged.
Legacy dynamic CLRN is accepted only with SCLR/ASDATA/SLOAD zero.
The extended domain requires CLRN/PRN=1 and ALOAD=0; normal device globals
are implicit inactive (one), and power_up must be high/low.
Binary priority: ENA, then SCLR to zero, then SLOAD selects ASDATA, else D.

Prepare/execute EQY proofs with no mapped-endpoint initialization repair.
Original RTL alone receives the disclosed logical-zero zinit contract; mapped
endpoints use their required explicit dffeas power_up parameters. The argument
slot named baseline may contain a caller-supplied candidate. Default is prepare
only, never infer PASS.

Only writes --out. Originals, compiler sources and supplied tools are read-only.
Requires OSS CAD Suite and a Windows POSIX shell/make tool directory.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, obj):
    Path(path).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def launch(argv, cwd, env, logfile, timeout=60):
    started = time.monotonic()
    with Path(logfile).open("wb") as log:
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, creationflags=FLAGS)
        try:
            code = process.wait(timeout=timeout)
            expired = False
        except subprocess.TimeoutExpired:
            expired = True
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               creationflags=FLAGS, check=False)
            else:
                process.kill()
            code = process.wait()
    return {"returncode": code, "timed_out": expired,
            "diagnostic_seconds_not_contest_time": time.monotonic() - started}


def yosys(argv, cwd, env, log):
    result = launch(argv, cwd, env, log)
    if result["returncode"] or result["timed_out"]:
        raise RuntimeError("Yosys preparation failed; see " + str(log))
    return result


def validate_raw(module):
    counts = Counter(c["type"] for c in module.get("cells", {}).values())
    for name, cell in module.get("cells", {}).items():
        kind = cell["type"]
        if kind == "dffeas":
            connections = cell["connections"]
            expected_ports = {"q", "d", "clk", "clrn", "prn", "ena", "asdata", "aload", "sclr", "sload"}
            if set(connections) != expected_ports or any(len(bits) != 1 for bits in connections.values()):
                raise ValueError("Unsupported dffeas port set/width: " + name)
            for pin, bits in connections.items():
                bit = bits[0]
                if type(bit) is not int and bit not in ("0", "1"):
                    raise ValueError(f"Undefined dffeas pin {name}.{pin}")
            if type(connections["q"][0]) is not int or type(connections["clk"][0]) is not int:
                raise ValueError("dffeas requires real Q and clock wires: " + name)
            for pin, value in {"prn": "1", "aload": "0"}.items():
                if cell["connections"].get(pin) != [value]:
                    raise ValueError(f"Unsupported dffeas control {name}.{pin}; do not blackbox it")
            # Dynamic asynchronous clear is accepted in the original domain only.
            legacy = all(connections[p] == ["0"] for p in ("sclr", "sload", "asdata"))
            if not legacy and connections["clrn"] != ["1"]:
                raise ValueError("Extended SCLR/SLOAD domain requires clrn tied to1: " + name)
            if cell["parameters"].get("power_up") not in ("high", "low"):
                raise ValueError("Unsupported dffeas power_up: " + name)
            if cell["parameters"].get("is_wysiwyg") != "TRUE":
                raise ValueError("Unexpected dffeas mapping provenance: " + name)
        elif kind not in ("$lut", "$_NOT_", "$not"):
            raise ValueError("Unsupported cell at mapped proof endpoint: " + kind)
        if kind.startswith("$spec"):
            raise ValueError("Timing primitive remains in top: " + kind)
    return dict(counts)


def model_domains(module):
    def domain(cell):
        c=cell["connections"]
        if all(c[p] == ["0"] for p in ("sclr", "sload", "asdata")):
            return "legacy_sclr_zero"
        if c["sload"] == ["0"] and c["asdata"] == ["0"]:
            return "sclr_with_async_controls_inactive"
        return "sclr_sload_asdata_with_async_controls_inactive"
    return dict(Counter(domain(cell) for cell in module.get("cells", {}).values()
                        if cell["type"] == "dffeas"))


def validate_defaults(module, case):
    defaults = module.get("parameter_default_values", {})
    for name, expected in case.get("default_parameters", {}).items():
        actual = defaults.get(name)
        if actual is None or re.search(r"[^01]", actual) or int(actual, 2) != expected:
            raise ValueError("Default parameter mismatch: " + name)


def normalize_il(raw, label, case, folder, exe, env):
    shutil.copy2(raw, folder / f"{label}.source.il")
    script = f'read_rtlil {label}.source.il\nhierarchy -check -top {case["top"]}\nwrite_json {label}.raw.json\n'
    (folder / f"{label}.inspect.ys").write_text(script, encoding="utf-8")
    yosys([str(exe), "-s", f"{label}.inspect.ys"], folder, env, folder / f"{label}.inspect.log")
    module = json.loads((folder / f"{label}.raw.json").read_text())["modules"][case["top"]]
    validate_defaults(module, case)
    raw_counts = validate_raw(module)
    script = f'read_rtlil {label}.source.il\nhierarchy -check -top {case["top"]}\n'
    script += 'techmap -autoproc -map dffeas_formal.v\n'
    script += f'prep -top {case["top"]} -flatten\ncheck -assert\ndelete =A:blackbox =A:whitebox\n'
    script += f'write_rtlil {label}.normalized.il\nwrite_json {label}.normalized.json\n'
    (folder / f"{label}.normalize.ys").write_text(script, encoding="utf-8")
    yosys([str(exe), "-s", f"{label}.normalize.ys"], folder, env, folder / f"{label}.normalize.log")
    result = inspect_normalized(folder, label, case, raw_counts, sha(raw))
    result["restricted_primitive_model_domains"] = model_domains(module)
    return result


def inspect_normalized(folder, label, case, raw_counts, raw_hash):
    module = json.loads((folder / f"{label}.normalized.json").read_text())["modules"][case["top"]]
    validate_defaults(module, case)
    counts = Counter(c["type"] for c in module.get("cells", {}).values())
    if any(not c.startswith("$") for c in counts):
        raise ValueError("Unresolved primitive/blackbox after normalization")
    if any(c.startswith(("$any", "$assume", "$assert")) for c in counts):
        raise ValueError("Unexpected formal cell in design under test")
    sequential = [c for c in module.get("cells", {}).values() if "ff" in c["type"].lower()]
    clocks = [c["connections"].get("CLK", c["connections"].get("C", [])) for c in sequential]
    if case.get("sequential") and not sequential:
        raise ValueError("Sequential case lost all flip-flops; refusing vacuous proof")
    if any(not clk or all(isinstance(bit, str) for bit in clk) for clk in clocks):
        raise ValueError("Flip-flop has missing/constant clock; requires investigation")
    return {"source_sha256": raw_hash, "raw_cell_types": raw_counts,
            "normalized_sha256": sha(folder / f"{label}.normalized.il"),
            "normalized_cell_types": dict(counts), "state_cells": len(sequential),
            "clock_bits_before_clk2fflogic": clocks,
            "ports": {name: {"direction": port["direction"], "width": len(port["bits"])}
                      for name, port in module["ports"].items()}}


def normalize_rtl(sources, case, folder, exe, env, preserve_unspecified_init=False):
    lines = []
    source_hashes = []
    for i, path in enumerate(sources):
        suffix = ".sv" if path.suffix.lower() == ".sv" else ".v"
        local = f"rtl_source_{i}{suffix}"
        shutil.copy2(path, folder / local)
        lines.append(f"read_verilog -formal {'-sv ' if suffix == '.sv' else ''}{local}")
        source_hashes.append({"path": str(path), "sha256": sha(path)})
    lines += [f'prep -top {case["top"]} -flatten', "memory_map", "opt_clean"]
    if not preserve_unspecified_init:
        lines.append("zinit -all")
    lines += ["check -assert",
              "write_rtlil rtl.normalized.il", "write_json rtl.normalized.json"]
    (folder / "rtl.normalize.ys").write_text("\n".join(lines) + "\n", encoding="utf-8")
    yosys([str(exe), "-s", "rtl.normalize.ys"], folder, env, folder / "rtl.normalize.log")
    return inspect_normalized(folder, "rtl", case, {}, source_hashes)


def state_relations(folder, top, left, right):
    gold = json.loads((folder / f"{left}.normalized.json").read_text())["modules"][top]
    gate = json.loads((folder / f"{right}.normalized.json").read_text())["modules"][top]
    state_bits = {bit for cell in gold["cells"].values() if "ff" in cell["type"].lower()
                  for bit in cell["connections"].get("Q", []) if isinstance(bit, int)}
    output_bits = {bit for port in gold["ports"].values() if port["direction"] == "output" for bit in port["bits"]}
    candidates = {}
    seen, covered_bits = set(), set()
    for name in sorted(gold["netnames"], key=lambda n: (-len(gold["netnames"][n]["bits"]), len(n), n)):
        if name in gold["ports"] or "dffeas" in name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$.\[\]]*", name):
            continue
        bits = gold["netnames"][name]["bits"]
        signature = tuple(bits)
        if signature in seen or not bits or any(bit not in state_bits for bit in bits) or any(bit in output_bits or bit in covered_bits for bit in bits):
            continue
        if name not in gate["netnames"] or len(gate["netnames"][name]["bits"]) != len(bits):
            continue
        candidates[name] = len(bits)
        seen.add(signature)
        covered_bits.update(bits)
    return candidates


def config(top, ports, left, right, depth, timeout, invariants=None):
    # Only port matches: internal cuts cannot hide a changed register/logic cone.
    invariants = invariants or {}
    for name in [top, *ports]:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", name):
            raise ValueError("Unsupported identifier for automatic config: " + name)
    return (f"[gold]\nread_rtlil {left}.normalized.il\n"
            f"[gate]\nread_rtlil {right}.normalized.il\n"
            f"[script]\nhierarchy -check -top {top}\nclk2fflogic\ncheck -assert\n"
            f"[match {top}]\nnodefault\n" + "".join(f"gold-match {p}\n" for p in ports) +
            "".join(f"gold-match /{re.escape(p)}/\n" for p in invariants) +
            f"[collect {top}]\ngroup *\n"
            f"[strategy induction]\nuse sat\ndepth {depth}\ntimeout {timeout}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--suite", type=Path, required=True)
    ap.add_argument("--shell-bin", type=Path, required=True)
    ap.add_argument("--baseline-dir", type=Path, required=True)
    ap.add_argument("--optimized-dir", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--case", action="append")
    ap.add_argument("--execute", action="store_true")
    ap.add_argument("--smoke-self", action="store_true", help="Only test2 self-comparison, never official acceptance")
    ap.add_argument("--baseline-only", action="store_true", help="Prove original RTL versus baseline IL only; no optimization acceptance")
    ap.add_argument("--mapped-only", action="store_true", help="Prove baseline versus optimized mapped IL only; keep original-RTL policy evidence separate")
    ap.add_argument("--state-invariants", action="store_true", help="Prove selected same-name state relations as additional outputs in the same full partition; never assume them")
    ap.add_argument("--diagnostic-preserve-rtl-init", action="store_true", help="DIAGNOSTIC ONLY: leave unspecified original RTL initial state undefined; never FAQ zero-init acceptance")
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--timeout", type=int, default=90)
    args = ap.parse_args()
    if sum((args.smoke_self, args.baseline_only, args.mapped_only)) > 1:
        ap.error("--smoke-self, --baseline-only and --mapped-only are mutually exclusive")
    suite, shell = args.suite.resolve(), args.shell_bin.resolve()
    exe = suite / "bin/yosys.exe"
    required = [exe, suite / "lib/python3.exe", suite / "bin/eqy-script.py",
                shell / "sh.exe", shell / "make.exe", shell / "grep.exe"]
    for path in required:
        if not path.is_file():
            ap.error("Missing tool: " + str(path))
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    adapter = out / "shell_adapter"
    adapter.mkdir()
    # EQY emits POSIX shell commands but names bash. w64devkit BusyBox sh
    # supports the generated source/if/case scripts. No supplied tool is patched.
    (adapter / "bash.cmd").write_text('@echo off\n"' + str(shell / "sh.exe") + '" %*\n', encoding="utf-8")
    env = os.environ.copy()
    local_temp = out / "child_temp"
    local_temp.mkdir()
    env["TEMP"] = env["TMP"] = str(local_temp)
    env["PATH"] = os.pathsep.join(map(str, [adapter, suite / "bin", suite / "lib", shell])) + os.pathsep + env.get("PATH", "") + os.pathsep + "."
    env["SHELL"] = str(shell / "sh.exe")
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    cases = [c for c in manifest["cases"] if not args.case or c["id"] in args.case]
    if args.smoke_self:
        cases = [c for c in cases if c["id"] == "test2"]
    if not cases:
        ap.error("No matching cases")
    if not args.smoke_self and not args.baseline_only and not args.optimized_dir:
        ap.error("--optimized-dir is required outside self smoke")
    report = {"schema": 1, "mode": "TOOLCHAIN_SELF_SMOKE" if args.smoke_self else ("BASELINE_ONLY_FULL_OUTPUT_PROOF" if args.baseline_only else "EQY_FULL_OUTPUT_PROOF"),
              "manifest_sha256": sha(manifest_path), "complete": False,
              "init_policy": "Original RTL only: zinit -all defines undefined source FF state as logical zero while preserving explicit 0/1 semantics. Mapped endpoints: require explicit dffeas power_up high/low, inverse-map it without zinit or setundef. Free inputs remain free.",
              "clock_policy": "clk2fflogic: actual input-clock edges retained as explicit transition logic; clock never tied low.",
              "partition_policy": "All top outputs and optional state equality invariants grouped into one jointly proved partition. Partition inputs must be original top inputs only; no internal cut assumptions.",
              "proof_endpoint": "ABC9-after/map_cells-before IL; isolated restricted SCLR/SLOAD dffeas model in proof copies only",
              "restricted_sclr_contract": {"legacy": "sclr=0, prn=1, asdata/aload/sload=0; clrn may be dynamic",
                                           "new": "clrn/prn=1, aload=0, normal device globals implicit1; ena > sclr(0) > sload(asdata) > d",
                                           "r29_degenerate_domain": "sload/asdata=0 reproduces the independently frozen r29 model",
                                           "vendor_probe_scope": "Existing vendor library, 642 finite binary observations, zero behavior mismatches; original probe report ERROR retained for copied-cache refresh. Not manufacturer formal proof.",
                                           "semantics": "binary controls; arbitrary source data/current state and actual clock edges; no new external assumptions",
                                           "vendor_reference": "https://docs.altera.com/r/docs/683082/25.1/quartus-prime-pro-edition-user-guide/secondary-register-control-signals-such-as-clear-and-clock-enable",
                                           "legacy_runner_sha256": sha(HERE / "legacy_endpoint_runner.py"),
                                           "legacy_model_sha256": sha(HERE / "legacy_dffeas_formal.v"),
                                           "simulation_counterpart_sha256": sha(HERE / "dffeas_sload_sim.v")},
              "toolchain": {"suite": str(suite), "shell_bin": str(shell), "yosys_sha256": sha(exe),
                            "eqy_script_sha256": sha(suite / "bin/eqy-script.py"),
                            "runner_sha256": sha(Path(__file__)),
                            "formal_model_sha256": sha(HERE / "dffeas_formal.v")},
              "cases": [], "all_nine_official_proven": False, "contest_score": None}
    if args.diagnostic_preserve_rtl_init:
        report["mode"] = "DIAGNOSTIC_UNSPECIFIED_RTL_INIT_NOT_FAQ_ZERO_INIT_ACCEPTANCE"
        report["init_policy"] = "Original RTL unspecified state remains undefined; mapped endpoints retain required explicit synthesis power_up parameters and receive no zinit/setundef. Supplementary refinement diagnostic only."
    elif args.mapped_only:
        report["mode"] = "MAPPED_PAIR_FULL_OUTPUT_INITIAL_STATE_PROOF"
        report["init_policy"] = "Both mapped endpoints use the same inverse model and retain required explicit dffeas power_up high/low; neither receives zinit or setundef. This does not replace the original-RTL logical-zero initialization audit."
    for case in cases:
        entry = {"id": case["id"], "status": "NOT_EXECUTED", "checks": []}
        report["cases"].append(entry)
        folder = out / case["id"]
        folder.mkdir()
        try:
            shutil.copy2(HERE / "dffeas_formal.v", folder / "dffeas_formal.v")
            left = args.baseline_dir.resolve() / case["id"] / "post_abc9.il"
            entry["baseline"] = normalize_il(left, "baseline", case, folder, exe, env)
            if args.smoke_self:
                pairs = [("baseline", "baseline")]
            else:
                if not args.baseline_only:
                    right = args.optimized_dir.resolve() / case["id"] / "post_abc9.il"
                    entry["optimized"] = normalize_il(right, "optimized", case, folder, exe, env)
                paths = case.get("sources", [case.get("path")])
                sources = [manifest_path.parent / (p["path"] if isinstance(p, dict) else p) for p in paths]
                for path in sources:
                    expected = case["source_sha256"].get(path.name)
                    if expected is None or sha(path) != expected:
                        raise ValueError("Official source SHA mismatch: " + str(path))
                if args.mapped_only:
                    pairs = [("baseline", "optimized")]
                else:
                    entry["rtl"] = normalize_rtl(sources, case, folder, exe, env, args.diagnostic_preserve_rtl_init)
                    pairs = [("rtl", "baseline")] if args.baseline_only else [("rtl", "baseline"), ("rtl", "optimized"), ("baseline", "optimized")]
            ports = entry["baseline"]["ports"]
            if not any(p["direction"] == "output" for p in ports.values()):
                raise ValueError("No output to prove")
            for label in ("optimized", "rtl"):
                if label in entry and entry[label]["ports"] != ports:
                    raise ValueError("Port signatures differ: " + label)
            for left_label, right_label in pairs:
                name = left_label + "_to_" + right_label
                job_name = {"rtl_to_baseline": "rb", "rtl_to_optimized": "ro", "baseline_to_optimized": "bo", "baseline_to_baseline": "self"}[name]
                cfg = folder / (name + ".eqy")
                invariants = state_relations(folder, case["top"], left_label, right_label) if args.state_invariants else {}
                cfg.write_text(config(case["top"], ports, left_label, right_label, args.depth, args.timeout, invariants), encoding="utf-8")
                check = {"name": name, "config_sha256": sha(cfg), "status": "NOT_EXECUTED",
                         "result_directory": job_name,
                         "additional_jointly_proved_state_relations": invariants}
                entry["checks"].append(check)
                if args.execute:
                    run = launch([str(suite / "lib/python3.exe"), str(suite / "bin/eqy-script.py"),
                                  "-j", "1", "-d", job_name, cfg.name], folder, env,
                                 folder / (name + ".console.log"), args.timeout + 30)
                    check["execution"] = run
                    result_dir = folder / job_name
                    markers = [s for s in ("PASS", "FAIL", "UNKNOWN", "ERROR", "TIMEOUT") if (result_dir / s).is_file()]
                    raw_status = markers[0] if len(markers) == 1 else "MISSING_OR_AMBIGUOUS"
                    check["eqy_raw_status"] = raw_status
                    check["status"] = "PASS" if run["returncode"] == 0 and raw_status == "PASS" else "NOT_PROVEN"
                    strategy_statuses = {str(p.relative_to(result_dir)): p.read_text().strip() for p in result_dir.glob("strategies/*/*/status")}
                    check["strategy_statuses"] = strategy_statuses
                    check["proof_outcome"] = ("TIMEOUT" if run["timed_out"] else "PASS" if check["status"] == "PASS" else
                                               "FAIL" if "FAIL" in strategy_statuses.values() else
                                               "UNKNOWN" if "UNKNOWN" in strategy_statuses.values() else "ERROR")
                    partition_list = result_dir / "partitions"
                    check["partition_count"] = len(list(partition_list.glob("*.json")))
                    coverage = {}
                    actual_inputs = set()
                    for part_file in partition_list.glob("*.json"):
                        part = json.loads(part_file.read_text())["partition"]
                        actual_inputs.update(part["inbits"])
                        for port, bits in part["outbits"].items():
                            coverage.setdefault(port, set()).update(bits)
                    expected = {p: meta["width"] for p, meta in ports.items() if meta["direction"] == "output"}
                    expected.update(invariants)
                    check["proven_output_bit_counts"] = {p: len(bits) for p, bits in coverage.items()}
                    check["expected_output_bit_counts"] = expected
                    check["partition_input_names"] = sorted(actual_inputs)
                    permitted_inputs = {p for p, meta in ports.items() if meta["direction"] == "input"}
                    check["only_original_inputs_used_as_partition_inputs"] = actual_inputs <= permitted_inputs
                    if not actual_inputs <= permitted_inputs or check["partition_count"] != 1:
                        check["status"] = "NOT_PROVEN"
                        check["reason"] = "Unexpected internal cut or multiple partitions; reject assumed state relation"
                    if check["proven_output_bit_counts"] != expected:
                        check["status"] = "NOT_PROVEN"
                        check["reason"] = "Partition output coverage is incomplete; manual investigation required"
                    if not list(partition_list.glob("*.il")):
                        check["status"] = "NOT_PROVEN"
                        check["reason"] = "No partition IL; reject empty proof"
                    if raw_status == "PASS" and check["status"] != "PASS":
                        check["proof_outcome"] = "REJECTED_PARTITION_VALIDATION"
            if args.execute:
                entry["status"] = "PASS" if all(c["status"] == "PASS" for c in entry["checks"]) else "NOT_PROVEN"
            else:
                entry["status"] = "PREPARED_NOT_EXECUTED"
        except Exception as exc:
            entry["status"] = "PREPARATION_ERROR"
            entry["error"] = str(exc)
        save(out / "report.json", report)
        print(case["id"] + ": " + entry["status"], flush=True)
    report["complete"] = True
    report["all_nine_official_proven"] = (not args.smoke_self and not args.baseline_only and not args.mapped_only and not args.diagnostic_preserve_rtl_init and manifest.get("official_cases_confirmed") is True
                                              and len(cases) == 9 and len(manifest["cases"]) == 9
                                              and all(c["status"] == "PASS" for c in report["cases"]))
    report["all_nine_baseline_proven"] = (args.baseline_only and not args.diagnostic_preserve_rtl_init and manifest.get("official_cases_confirmed") is True
                                               and len(cases) == 9 and len(manifest["cases"]) == 9
                                               and all(c["status"] == "PASS" for c in report["cases"]))
    report["all_nine_mapped_pair_proven"] = (args.mapped_only and manifest.get("official_cases_confirmed") is True
                                               and len(cases) == 9 and len(manifest["cases"]) == 9
                                               and all(c["status"] == "PASS" for c in report["cases"]))
    save(out / "report.json", report)
    expected_status = "PASS" if args.execute else "PREPARED_NOT_EXECUTED"
    return 0 if all(c["status"] == expected_status for c in report["cases"]) else 2


if __name__ == "__main__":
    raise SystemExit(main())
