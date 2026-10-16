"""Run the DLL E2E and consolidate content-free, explicitly simulated evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

GATES = ("native_callbacks", "native_undo_selection", "native_failure_preservation", "native_arm64")
SUMMARY_KEYS = {"schema", "suite", "cases", "failures", "containment_cases", "integrity_loss_cases", "preservation_gate", "native_gates", "release_blocked", "fault_observations"}
REPORT_KEYS = {"schema", "environment", "revision", "dll", "simulated", "gates", "release_blocked", "native_certified"}


def validate_summary(value: dict) -> None:
    if not isinstance(value, dict) or set(value) != SUMMARY_KEYS:
        raise ValueError("Unexpected suite fields; document data is forbidden")
    if type(value["schema"]) is not int or value["schema"] != 1 or value["suite"] != "simulated-host":
        raise ValueError("Not a simulated-host schema 1 report")
    for key in ("cases", "failures", "containment_cases", "integrity_loss_cases"):
        if type(value[key]) is not int or value[key] < 0:
            raise ValueError("Invalid counter")
    if value["cases"] == 0 or value["containment_cases"] == 0 or value["containment_cases"] > value["cases"] or value["integrity_loss_cases"] > value["cases"]:
        raise ValueError("Inconsistent coverage counters")
    expected = "failed" if value["integrity_loss_cases"] else "passed"
    if value["preservation_gate"] != expected or value["native_gates"] != "pending" or value["release_blocked"] is not True:
        raise ValueError("Simulation cannot certify native gates or waive integrity loss")
    faults = value["fault_observations"]
    if not isinstance(faults, list) or len(faults) != value["containment_cases"] or len(faults) != 6:
        raise ValueError("Missing fault observations")
    losses = 0
    for index, fault in enumerate(faults):
        if not isinstance(fault, dict) or set(fault) != {"case", "text_equal", "selection_equal", "history_equal", "replacements", "undo_calls"}:
            raise ValueError("Unexpected fault observation fields")
        if type(fault["case"]) is not int or fault["case"] != index or any(type(fault[key]) is not bool for key in ("text_equal", "selection_equal", "history_equal")):
            raise ValueError("Invalid fault observation")
        if any(type(fault[key]) is not int or fault[key] < 0 for key in ("replacements", "undo_calls")) or fault["replacements"] > 1 or fault["undo_calls"] > 1:
            raise ValueError("Unsafe recovery observed")
        if fault["undo_calls"] and (index not in (1, 2) or fault["replacements"] != 1
                                    or not fault["text_equal"] or not fault["selection_equal"]):
            raise ValueError("Unsafe recovery observed")
        if index in (1, 2) and fault["text_equal"] and fault["selection_equal"] and fault["undo_calls"] != 1:
            raise ValueError("Restored replacement requires one compensating Undo")
        losses += not all(fault[key] for key in ("text_equal", "selection_equal", "history_equal"))
    if losses != value["integrity_loss_cases"]:
        raise ValueError("Integrity counter disagrees with measured equalities")


def validate_report(value: dict) -> None:
    if not isinstance(value, dict) or set(value) != REPORT_KEYS or type(value["schema"]) is not int or value["schema"] != 1:
        raise ValueError("Unexpected evidence fields")
    if not isinstance(value["environment"], dict) or set(value["environment"]) != {"os", "architecture", "host_version", "scintilla_version"}:
        raise ValueError("Unexpected environment fields")
    environment = value["environment"]
    if environment["os"] != "Windows" or environment["architecture"] not in ("x86", "x64", "ARM64") or environment["host_version"] != "8.9.8.1" or environment["scintilla_version"] != "5.6.6":
        raise ValueError("Unrecognized baseline")
    if not isinstance(value["revision"], str) or not re.fullmatch(r"[0-9a-f]{40}", value["revision"]):
        raise ValueError("Invalid revision")
    dll = value["dll"]
    if not isinstance(dll, dict) or set(dll) != {"size", "sha256", "machine", "pe_valid"}:
        raise ValueError("Unexpected DLL fields")
    if type(dll["size"]) is not int or dll["size"] < 64 or not isinstance(dll["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", dll["sha256"]) or dll["pe_valid"] is not True:
        raise ValueError("Invalid DLL identity")
    if dll["machine"] != environment["architecture"]:
        raise ValueError("DLL architecture mismatch")
    validate_summary(value["simulated"])
    if not isinstance(value["gates"], dict) or set(value["gates"]) != set(GATES) | {"simulated_containment", "simulated_preservation"}:
        raise ValueError("Missing or unexpected gates")
    gates = value["gates"]
    if any(gates[key] != "pending" for key in GATES):
        raise ValueError("Native evidence requires a separate native protocol")
    if gates["simulated_preservation"] != value["simulated"]["preservation_gate"] or gates["simulated_containment"] != ("passed" if value["simulated"]["failures"] == 0 else "failed"):
        raise ValueError("Gate disagrees with observed suite")
    if value["release_blocked"] is not True or value["native_certified"] is not False:
        raise ValueError("Simulation cannot authorize release or native certification")


def inspect_pe(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Missing MZ header")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset > len(data) - 24 or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Missing PE header")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    names = {0x14C: "x86", 0x8664: "x64", 0xAA64: "ARM64"}
    if machine not in names:
        raise ValueError("Unsupported PE machine")
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "machine": names[machine], "pe_valid": True}


def consolidate(summary: dict, dll: dict, revision: str) -> dict:
    validate_summary(summary)
    report = {"schema": 1, "environment": {"os": "Windows", "architecture": dll["machine"], "host_version": "8.9.8.1", "scintilla_version": "5.6.6"},
              "revision": revision, "dll": dll, "simulated": summary,
              "gates": {**{key: "pending" for key in GATES}, "simulated_containment": "passed" if summary["failures"] == 0 else "failed", "simulated_preservation": summary["preservation_gate"]},
              "release_blocked": True, "native_certified": False}
    validate_report(report)
    return report


def run(harness: Path, dll: Path, revision: str) -> dict:
    harness_identity = inspect_pe(harness)
    dll_identity = inspect_pe(dll)
    if harness_identity["machine"] != dll_identity["machine"]:
        raise ValueError("Harness and DLL must share architecture")
    # shortcut: approved local harness only; bound pipes before supporting arbitrary runners.
    completed = subprocess.run([str(harness.resolve()), str(dll.resolve())], capture_output=True, timeout=60, check=False)
    # Never relay raw stdout/stderr; only strict numeric/enumerated metadata may
    # cross the persistence boundary. Malformed output fails closed.
    try:
        summary = json.loads(completed.stdout.decode("utf-8", errors="strict"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("Harness did not return valid metadata") from error
    validate_summary(summary)
    if completed.returncode != 0 or completed.stderr or summary["failures"]:
        raise ValueError("DLL E2E failed; certification refused")
    return consolidate(summary, dll_identity, revision)


def _reject_output_alias(output: Path, harness: Path, dll: Path) -> None:
    if output.exists() and any(output.samefile(path) for path in (harness, dll)):
        raise ValueError("Evidence output aliases an input binary")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate", type=Path)
    args = parser.parse_args()
    try:
        if args.validate:
            validate_report(json.loads(args.validate.read_text(encoding="utf-8", errors="strict")))
        else:
            if not all((args.harness, args.dll, args.revision, args.output)):
                parser.error("--harness, --dll, --revision and --output are required")
            _reject_output_alias(args.output, args.harness, args.dll)
            report = run(args.harness, args.dll, args.revision)
            _reject_output_alias(args.output, args.harness, args.dll)
            args.output.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
            validate_report(json.loads(args.output.read_text(encoding="utf-8", errors="strict")))
        print("Host evidence valid; native gates pending; release blocked")
        return 0
    except (ValueError, OSError, subprocess.TimeoutExpired):
        print("Host evidence rejected; no native certification")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
