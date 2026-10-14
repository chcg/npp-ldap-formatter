"""Benchmark production gateway/core in six fresh, isolated Notepad++ processes."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

from host_evidence import inspect_pe

ROOT = Path(__file__).resolve().parents[1]
SIZES = (262144, 1048576, 5242880)
MODES = ("Pretty", "Compact")
ARCHIVE_SHA256 = "beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414"
SDK_REVISION = "dd40fe45598791e9aa38f3aeb200e10fb3d6338a"
PHASES = ("capture_ms", "format_ms", "commit_ms", "total_ms")
RUN_KEYS = set(PHASES) | {"output_bytes", "core_peak_bytes", "ui_ticks", "ui_max_gap_ms"}
NATIVE_KEYS = {"schema", "complete", "input_bytes", "mode", "host_version_code", "compiler_version", "ui_thread", "worker_scintilla_calls", "timer_period_ms", "setup_settle_ms", "failure_stage", "failure_code", "working_set_base_bytes", "working_set_final_bytes", "working_set_peak_bytes", "executions", "runs"}
CONFIG = '''<?xml version="1.0" encoding="UTF-8"?>
<NotepadPlus><GUIConfigs>
<GUIConfig name="RememberLastSession">no</GUIConfig>
<GUIConfig name="Backup" action="0" useCustumDir="no" dir="" isSnapshotMode="no" snapshotBackupTiming="7000" />
<GUIConfig name="noUpdate" autoUpdateMode="0">yes</GUIConfig>
<GUIConfig name="NewDocDefaultSettings" format="0" encoding="4" lang="0" codepage="-1" openAnsiAsUTF8="yes" addNewDocumentOnStartup="no" useContentAsTabName="no" />
</GUIConfigs></NotepadPlus>
'''


class NativeResultError(ValueError):
    def __init__(self, native: object):
        super().__init__("Invalid/incomplete native result")
        self.numeric = {key: -1 for key in ("executions", "failure_stage", "failure_code")}
        if isinstance(native, dict):
            for key in self.numeric:
                value = native.get(key)
                maximum = {"executions": 35, "failure_stage": 6, "failure_code": 100}[key]
                if type(value) is int and 0 <= value <= maximum:
                    self.numeric[key] = value


def exact_keys(value: object, keys: set) -> None:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("Unexpected fields; document data forbidden")


def integer(value: object, low: int = 0, high: int = 2**63 - 1) -> None:
    if type(value) is not int or not low <= value <= high:
        raise ValueError("Invalid integer metadata")


def number(value: object) -> None:
    if type(value) not in (int, float) or value < 0 or value > sys.float_info.max or not math.isfinite(value):
        raise ValueError("Invalid timing")


def load_json(path: Path) -> dict:
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ValueError("Duplicate JSON field")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8", errors="strict"), object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))


def validate_native(value: dict, size: int, mode: str) -> None:
    exact_keys(value, NATIVE_KEYS)
    if type(value["schema"]) is not int or value["schema"] != 1 or value["complete"] is not True:
        raise ValueError("Native scenario incomplete")
    if type(value["input_bytes"]) is not int or value["input_bytes"] != size or size not in SIZES or value["mode"] != mode or mode not in MODES:
        raise ValueError("Incorrect scenario")
    if type(value["host_version_code"]) is not int or value["host_version_code"] != (8 << 16) + 981:
        raise ValueError("Incorrect host")
    integer(value["compiler_version"], 194400000, 194499999)
    if type(value["setup_settle_ms"]) is not int or value["setup_settle_ms"] != 100 or type(value["failure_stage"]) is not int or value["failure_stage"] != 0 or type(value["failure_code"]) is not int or value["failure_code"] != 0:
        raise ValueError("Invalid setup/failure metadata")
    integer(value["ui_thread"], 1)
    if type(value["worker_scintilla_calls"]) is not int or value["worker_scintilla_calls"] != 0 or type(value["timer_period_ms"]) is not int or value["timer_period_ms"] != 10:
        raise ValueError("Invalid UI protocol")
    if type(value["executions"]) is not int or value["executions"] != 35 or not isinstance(value["runs"], list) or len(value["runs"]) != 35:
        raise ValueError("Missing warmups/samples")
    for key in ("working_set_base_bytes", "working_set_final_bytes", "working_set_peak_bytes"):
        integer(value[key], 1)
    if value["working_set_peak_bytes"] < max(value["working_set_base_bytes"], value["working_set_final_bytes"]):
        raise ValueError("Inconsistent Working Set")
    output = None
    for run in value["runs"]:
        exact_keys(run, RUN_KEYS)
        for key in (*PHASES, "ui_max_gap_ms"):
            number(run[key])
        if run["total_ms"] + 1e-6 < sum(run[key] for key in PHASES[:3]):
            raise ValueError("Inconsistent phase durations")
        integer(run["output_bytes"], 1, 5242880)
        integer(run["core_peak_bytes"], size + 1, 33554432)
        integer(run["ui_ticks"])
        if output is not None and run["output_bytes"] != output:
            raise ValueError("Output size changed between runs")
        output = run["output_bytes"]


def p95(values: list) -> float:
    if len(values) != 30:
        raise ValueError("p95 requires exactly thirty measured samples")
    for value in values:
        number(value)
    return sorted(values)[math.ceil(0.95 * len(values)) - 1]


def summarize(native: dict) -> dict:
    exact_keys(native, NATIVE_KEYS)
    validate_native(native, native["input_bytes"], native["mode"])
    measured = native["runs"][5:]
    phases = {key: p95([run[key] for run in measured]) for key in PHASES}
    return {"native": native, "warmups": 5, "samples": 30, "output_bytes": measured[0]["output_bytes"],
            "p95_ms": phases, "core_peak_bytes": max(run["core_peak_bytes"] for run in native["runs"]),
            "working_set_peak_delta_bytes": native["working_set_peak_bytes"] - native["working_set_base_bytes"],
            "ui_ticks": sum(run["ui_ticks"] for run in measured),
            "ui_max_gap_ms": max(run["ui_max_gap_ms"] for run in measured),
            "capture_gate": "passed" if phases["capture_ms"] < 50 else "failed",
            "commit_gate": "passed" if phases["commit_ms"] < 50 else "failed"}


def validate_build(build: dict) -> None:
    exact_keys(build, {"configuration", "architecture", "runtime", "compiler", "compiler_full_version", "toolset", "windows_sdk", "sdk_revision", "scintilla_version", "dll_sha256", "source_revision", "source_modified", "source_digest"})
    expected = {"configuration": "Release", "architecture": "x64", "runtime": "MultiThreaded", "toolset": "14.44.35207", "windows_sdk": "10.0.26100.0", "sdk_revision": SDK_REVISION, "scintilla_version": "5.6.6"}
    if any(build[key] != val for key, val in expected.items()) or not isinstance(build["compiler"], str) or not re.fullmatch(r"19\.44\.\d+(?:\.\d+)?", build["compiler"]):
        raise ValueError("Incorrect build/toolchain")
    major, minor, patch = map(int, build["compiler"].split(".")[:3])
    integer(build["compiler_full_version"], 194400000, 194499999)
    if build["compiler_full_version"] != major * 10000000 + minor * 100000 + patch:
        raise ValueError("Compiler versions disagree")
    for key in ("dll_sha256", "source_digest"):
        if not isinstance(build[key], str) or not re.fullmatch(r"[0-9a-f]{64}", build[key]):
            raise ValueError("Invalid build hash")
    if not isinstance(build["source_revision"], str) or not re.fullmatch(r"[0-9a-f]{40}", build["source_revision"]) or type(build["source_modified"]) is not bool:
        raise ValueError("Invalid build source provenance")


def validate_binding(build: dict, dll: dict, compiler: int | None = None) -> None:
    validate_build(build); validate_identity(dll)
    if build["dll_sha256"] != dll["sha256"]:
        raise ValueError("Manifest is not bound to the measured DLL")
    if compiler is not None and compiler != build["compiler_full_version"]:
        raise ValueError("Native compiler does not match the build manifest")


def validate_identity(identity: dict) -> None:
    exact_keys(identity, {"size", "sha256", "machine", "pe_valid"})
    integer(identity["size"], 64)
    if identity["machine"] != "x64" or identity["pe_valid"] is not True or not isinstance(identity["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", identity["sha256"]):
        raise ValueError("Incorrect binary identity")


def validate_report(report: dict) -> None:
    exact_keys(report, {"schema", "complete", "environment", "provenance", "protocol", "scenarios", "errors", "discrepancies", "reference_nfr002", "other_release_gates", "release_blocked"})
    if type(report["schema"]) is not int or report["schema"] != 2 or type(report["complete"]) is not bool or report["release_blocked"] is not True or report["reference_nfr002"] != "not-promoted" or report["other_release_gates"] != "pending":
        raise ValueError("Benchmark cannot promote release")
    env = report["environment"]
    exact_keys(env, {"os", "windows_major", "windows_minor", "windows_build", "cpu_model", "physical_cores", "logical_processors", "ram_bytes", "storage_reference", "architecture"})
    if env["os"] not in ("Windows 10", "Windows 11") or env["architecture"] != "x64" or env["storage_reference"] != "not-verified":
        raise ValueError("Incorrect environment")
    for key in ("windows_major", "windows_minor", "windows_build", "physical_cores", "logical_processors", "ram_bytes"):
        integer(env[key], 0 if key == "windows_minor" else 1)
    if env["windows_major"] != 10 or env["windows_minor"] != 0 or env["os"] != ("Windows 11" if env["windows_build"] >= 22000 else "Windows 10") or env["physical_cores"] > env["logical_processors"]:
        raise ValueError("Inconsistent Windows/CPU metadata")
    if not isinstance(env["cpu_model"], str) or not re.fullmatch(r"[A-Za-z0-9 @().+_/\-]{1,200}", env["cpu_model"]):
        raise ValueError("Invalid CPU metadata")
    provenance = report["provenance"]
    exact_keys(provenance, {"host", "benchmark_dll", "archive_sha256", "host_version", "scintilla_version_source", "build", "source_revision", "source_modified"})
    validate_identity(provenance["host"]); validate_identity(provenance["benchmark_dll"])
    validate_binding(provenance["build"], provenance["benchmark_dll"])
    if provenance["archive_sha256"] != ARCHIVE_SHA256 or provenance["host_version"] != "8.9.8.1" or provenance["scintilla_version_source"] != "pinned-sdk-and-official-host-archive":
        raise ValueError("Unverified host provenance")
    if not isinstance(provenance["source_revision"], str) or not re.fullmatch(r"[0-9a-f]{40}", provenance["source_revision"]) or type(provenance["source_modified"]) is not bool:
        raise ValueError("Invalid source provenance")
    if any(provenance[key] != provenance["build"][key] for key in ("source_revision", "source_modified")):
        raise ValueError("Source provenance must come from the build")
    if report["protocol"] != {"warmups": 5, "samples": 30, "p95": "nearest-rank", "fresh_process_per_scenario": True, "ui_timer_period_ms": 10, "working_set_peak": "process-lifetime-os-high-water", "core_peak": "shared-budget-exact-acquisition-high-water"}:
        raise ValueError("Incorrect protocol")
    for key in ("warmups", "samples", "ui_timer_period_ms"):
        integer(report["protocol"][key], 1)
    if report["protocol"]["fresh_process_per_scenario"] is not True:
        raise ValueError("Fresh-process flag must be boolean")
    if not isinstance(report["scenarios"], list) or len(report["scenarios"]) > 6 or not isinstance(report["errors"], list):
        raise ValueError("Invalid matrix")
    seen = set()
    for scenario in report["scenarios"]:
        exact_keys(scenario, {"native", "warmups", "samples", "output_bytes", "p95_ms", "core_peak_bytes", "working_set_peak_delta_bytes", "ui_ticks", "ui_max_gap_ms", "capture_gate", "commit_gate"})
        native = scenario["native"]
        exact_keys(native, NATIVE_KEYS)
        validate_native(native, native["input_bytes"], native["mode"])
        validate_binding(provenance["build"], provenance["benchmark_dll"], native["compiler_version"])
        exact_keys(scenario["p95_ms"], set(PHASES))
        for timing in scenario["p95_ms"].values(): number(timing)
        for key in ("warmups", "samples", "output_bytes", "core_peak_bytes", "working_set_peak_delta_bytes", "ui_ticks"):
            integer(scenario[key])
        number(scenario["ui_max_gap_ms"])
        key = (native["input_bytes"], native["mode"])
        if key in seen or scenario != summarize(native):
            raise ValueError("Duplicate/incorrect summary")
        seen.add(key)
    for error in report["errors"]:
        exact_keys(error, {"input_bytes", "mode", "reason", "executions", "failure_stage", "failure_code"})
        if type(error["input_bytes"]) is not int or error["input_bytes"] not in SIZES or error["mode"] not in MODES or error["reason"] not in ("timeout", "host-exited", "invalid-native-result", "runner-io"):
            raise ValueError("Invalid incomplete scenario")
        integer(error["executions"], -1, 35)
        integer(error["failure_stage"], -1, 6)
        integer(error["failure_code"], -1, 100)
        key = (error["input_bytes"], error["mode"])
        if key in seen:
            raise ValueError("Duplicate scenario")
        seen.add(key)
    if seen != {(size, mode) for size in SIZES for mode in MODES} or report["complete"] != (len(report["scenarios"]) == 6 and not report["errors"]):
        raise ValueError("Matrix completeness mismatch")
    if report["discrepancies"] != discrepancies(report):
        raise ValueError("Hidden/incorrect discrepancies")


def discrepancies(report: dict) -> list:
    result = ["reference-storage-not-verified", "other-release-gates-pending"]
    env = report["environment"]
    if env["os"] != "Windows 11": result.append("windows10-differs-from-windows11-reference")
    if env["physical_cores"] < 4: result.append("reference-cpu-below-four-physical-cores")
    if env["ram_bytes"] < 16 * 1024**3: result.append("reference-ram-below-16GiB")
    for scenario in report["scenarios"]:
        native = scenario["native"]
        label = f'{native["input_bytes"]}-{native["mode"]}'
        for gate in ("capture_gate", "commit_gate"):
            if scenario[gate] == "failed": result.append(f"{label}-{gate}-p95-at-least-50ms")
        if scenario["ui_ticks"] == 0: result.append(f"{label}-no-ui-probe-observed")
    if report["errors"]: result.append("native-matrix-incomplete")
    return result


def file_version(path: Path) -> str:
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID)
    version.VerQueryValueW.argtypes = (wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(wintypes.LPVOID), ctypes.POINTER(wintypes.UINT))
    ignored = wintypes.DWORD()
    length = version.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
    if not length: raise ValueError("Missing file version")
    data = ctypes.create_string_buffer(length)
    if not version.GetFileVersionInfoW(str(path), 0, length, data): raise ValueError("Invalid file version")
    pointer, amount = wintypes.LPVOID(), wintypes.UINT()
    if not version.VerQueryValueW(data, "\\", ctypes.byref(pointer), ctypes.byref(amount)) or amount.value < 52:
        raise ValueError("Invalid fixed file info")
    info = ctypes.cast(pointer, ctypes.POINTER(wintypes.DWORD))
    if info[0] != 0xFEEF04BD: raise ValueError("Invalid version signature")
    return f"{info[2] >> 16}.{info[2] & 65535}.{info[3] >> 16}.{info[3] & 65535}"


def native_architecture() -> str:
    # GetNativeSystemInfo reports the OS architecture even for an emulated process.
    class SystemInfo(ctypes.Structure):
        _fields_ = [("architecture", wintypes.WORD), ("reserved", wintypes.WORD),
                    ("page_size", wintypes.DWORD), ("minimum_address", wintypes.LPVOID),
                    ("maximum_address", wintypes.LPVOID), ("active_mask", ctypes.c_size_t),
                    ("processors", wintypes.DWORD), ("processor_type", wintypes.DWORD),
                    ("allocation_granularity", wintypes.DWORD), ("level", wintypes.WORD), ("revision", wintypes.WORD)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetNativeSystemInfo.argtypes = (ctypes.POINTER(SystemInfo),)
    kernel.GetNativeSystemInfo.restype = None
    info = SystemInfo(); kernel.GetNativeSystemInfo(ctypes.byref(info))
    if info.architecture != 9: # PROCESSOR_ARCHITECTURE_AMD64
        raise ValueError("Native AMD64 Windows required; emulation is outside this protocol")
    return "x64"


def environment() -> dict:
    architecture = native_architecture()
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
        cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    class Memory(ctypes.Structure):
        _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [(key, ctypes.c_ulonglong) for key in ("total", "available", "total_page", "available_page", "total_virtual", "available_virtual", "extended")]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory = Memory(); memory.length = ctypes.sizeof(memory)
    if not kernel.GlobalMemoryStatusEx(ctypes.byref(memory)): raise OSError("RAM unavailable")
    length = wintypes.DWORD()
    kernel.GetLogicalProcessorInformationEx(0, None, ctypes.byref(length))
    if not length.value: raise OSError("Processor topology unavailable")
    data = ctypes.create_string_buffer(length.value)
    if not kernel.GetLogicalProcessorInformationEx(0, data, ctypes.byref(length)): raise OSError("Processor topology unavailable")
    cores, offset = 0, 0
    while offset < length.value:
        relationship, size = struct.unpack_from("<II", data.raw, offset)
        if size < 8 or offset + size > length.value or relationship != 0: raise ValueError("Invalid topology")
        cores += 1; offset += size
    windows = sys.getwindowsversion()
    return {"os": "Windows 11" if windows.build >= 22000 else "Windows 10", "windows_major": windows.major,
            "windows_minor": windows.minor, "windows_build": windows.build, "cpu_model": cpu,
            "physical_cores": cores, "logical_processors": os.cpu_count(), "ram_bytes": memory.total,
            "storage_reference": "not-verified", "architecture": architecture}


def reject_output(output: Path, inputs: tuple, root: Path = ROOT) -> None:
    resolved = output.resolve()
    allowed = root / ".build" / "benchmark-results"
    if not resolved.is_relative_to(root.resolve()):
        raise ValueError("Output escapes the workspace")
    if resolved != (root / "docs" / "benchmarks-ui-results.json").resolve() and not resolved.is_relative_to(allowed.resolve()):
        raise ValueError("Output must be the documented report or under .build/benchmark-results")
    if resolved.suffix != ".json" or any(resolved == path.resolve() or (output.exists() and output.samefile(path)) for path in inputs):
        raise ValueError("Output aliases an input/protected source")
    if output.exists():
        validate_report(load_json(output))


def run_scenario(portable: Path, dll: Path, size: int, mode: str, timeout: float, runs_root: Path, build: dict) -> dict:
    validate_binding(build, inspect_pe(dll))
    # mkdtemp owns a unique absolute subtree; never copy user config or other plugins.
    with tempfile.TemporaryDirectory(prefix="native-ui-", dir=runs_root) as temporary:
        isolated = Path(temporary).resolve()
        executable = isolated / "notepad++.exe"
        shutil.copyfile(portable / "notepad++.exe", executable)
        plugin = isolated / "plugins" / "NativeUiBenchmark" / "NativeUiBenchmark.dll"
        plugin.parent.mkdir(parents=True); shutil.copyfile(dll, plugin)
        validate_binding(build, inspect_pe(plugin))
        for name in ("langs.model.xml", "stylers.model.xml"):
            shutil.copyfile(portable / name, isolated / name)
        for name in ("doLocalConf.xml", "disableNppAutoUpdate.xml"):
            (isolated / name).write_bytes(b"")
        (isolated / "config.xml").write_text(CONFIG, encoding="utf-8")
        native_output = isolated / "numeric-result.json"
        env = os.environ.copy()
        env.update(NPP_LDAP_BENCHMARK_OUTPUT=str(native_output), NPP_LDAP_BENCHMARK_SIZE=str(size), NPP_LDAP_BENCHMARK_MODE=mode)
        startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
        process = subprocess.Popen([str(executable), "-multiInst", "-nosession", "-notabbar", f"-settingsDir={isolated}"],
                                   cwd=isolated, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, startupinfo=startup)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if native_output.exists():
                    try:
                        native = load_json(native_output)
                    except json.JSONDecodeError:
                        time.sleep(0.05); continue # A flush can be in progress.
                    try:
                        validate_native(native, size, mode)
                        validate_binding(build, inspect_pe(plugin), native["compiler_version"])
                    except ValueError:
                        error = NativeResultError(native)
                        print(f"Native rejection {size} {mode}: {error.numeric}", flush=True)
                        raise error from None
                    if (isolated / "backup").exists() and any((isolated / "backup").iterdir()):
                        raise ValueError("Snapshot persistence unexpectedly enabled")
                    return summarize(native)
                if process.poll() is not None:
                    raise ChildProcessError("Owned host exited before results")
                time.sleep(0.05)
            raise TimeoutError("Owned host timed out")
        finally:
            # The only termination target is the exact Popen handle we created.
            if process.poll() is None: process.terminate()
            process.wait(timeout=10)


def run_matrix(portable: Path, archive: Path, dll: Path, build_path: Path, timeout: float) -> dict:
    if sys.platform != "win32" or not math.isfinite(timeout) or not 1 <= timeout <= 600:
        raise ValueError("Native Windows and timeout 1..600 seconds required")
    host = inspect_pe(portable / "notepad++.exe"); identity = inspect_pe(dll)
    validate_identity(host); validate_identity(identity)
    if file_version(portable / "notepad++.exe") != "8.9.8.1": raise ValueError("Incorrect portable host version")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256: raise ValueError("Official ZIP hash mismatch")
    with zipfile.ZipFile(archive) as package:
        if hashlib.sha256(package.read("notepad++.exe")).hexdigest() != host["sha256"]:
            raise ValueError("Portable executable differs from verified official ZIP")
    build = load_json(build_path); validate_binding(build, identity)
    report = {"schema": 2, "complete": False, "environment": environment(),
              "provenance": {"host": host, "benchmark_dll": identity, "archive_sha256": ARCHIVE_SHA256,
                             "host_version": "8.9.8.1", "scintilla_version_source": "pinned-sdk-and-official-host-archive",
                             "build": build, "source_revision": build["source_revision"], "source_modified": build["source_modified"]},
              "protocol": {"warmups": 5, "samples": 30, "p95": "nearest-rank", "fresh_process_per_scenario": True,
                           "ui_timer_period_ms": 10, "working_set_peak": "process-lifetime-os-high-water",
                           "core_peak": "shared-budget-exact-acquisition-high-water"},
              "scenarios": [], "errors": [], "discrepancies": [], "reference_nfr002": "not-promoted",
              "other_release_gates": "pending", "release_blocked": True}
    runs_root = ROOT / ".build" / "benchmark-runs"; runs_root.mkdir(exist_ok=True)
    if not runs_root.resolve().is_relative_to(ROOT.resolve()):
        raise ValueError("Isolated-run directory escapes the workspace")
    for size in SIZES:
        for mode in MODES:
            try:
                report["scenarios"].append(run_scenario(portable, dll, size, mode, timeout, runs_root, build))
            except (ValueError, UnicodeError, OSError, subprocess.TimeoutExpired) as error:
                reason = "timeout" if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)) else "host-exited" if isinstance(error, ChildProcessError) else "runner-io" if isinstance(error, OSError) else "invalid-native-result"
                numeric = error.numeric if isinstance(error, NativeResultError) else {key: -1 for key in ("executions", "failure_stage", "failure_code")}
                report["errors"].append({"input_bytes": size, "mode": mode, "reason": reason, **numeric})
            print(f"Native scenario {size} {mode}: {'incomplete' if report['errors'] and report['errors'][-1]['input_bytes'] == size and report['errors'][-1]['mode'] == mode else 'complete'}", flush=True)
    report["complete"] = len(report["scenarios"]) == 6 and not report["errors"]
    report["discrepancies"] = discrepancies(report)
    validate_report(report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", type=Path, default=ROOT / ".build/benchmark-host-8.9.8.1/portable")
    parser.add_argument("--archive", type=Path, default=ROOT / ".build/benchmark-host-8.9.8.1/npp.8.9.8.1.portable.x64.zip")
    parser.add_argument("--dll", type=Path, default=ROOT / ".build/x64/Release/NativeUiBenchmark.dll")
    parser.add_argument("--build-metadata", type=Path, default=ROOT / ".build/x64/Release/native-benchmark-build.json")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/benchmarks-ui-results.json")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--validate", type=Path)
    args = parser.parse_args()
    try:
        if args.validate:
            report = load_json(args.validate); validate_report(report)
        else:
            inputs = (args.dll, args.archive, args.portable / "notepad++.exe", args.build_metadata)
            reject_output(args.output, inputs)
            report = run_matrix(args.portable.resolve(), args.archive.resolve(), args.dll.resolve(), args.build_metadata.resolve(), args.timeout)
            reject_output(args.output, inputs)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            # Atomic replacement also avoids modifying a hardlinked binary at the write boundary.
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=args.output.parent, suffix=".json", delete=False) as stream:
                    temporary = Path(stream.name)
                    json.dump(report, stream, indent=2, ensure_ascii=True, allow_nan=False); stream.write("\n")
                validate_report(load_json(temporary))
                temporary.replace(args.output)
            finally:
                if temporary is not None: temporary.unlink(missing_ok=True)
            report = load_json(args.output); validate_report(report)
        print(f"Native matrix {'complete' if report['complete'] else 'incomplete'}; reference NFR-002 not promoted; release blocked")
        return 0 if report["complete"] else 1
    except (ValueError, UnicodeError, OSError, subprocess.SubprocessError, zipfile.BadZipFile):
        print("Benchmark rejected before certification; no release promotion")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
