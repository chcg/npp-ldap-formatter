import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import benchmark_ui as benchmark


def native(size=262144, mode="Pretty"):
    return {"schema": 1, "complete": True, "input_bytes": size, "mode": mode, "host_version_code": (8 << 16) + 981,
            "compiler_version": 194435229, "ui_thread": 123, "worker_scintilla_calls": 0, "timer_period_ms": 10,
            "setup_settle_ms": 100, "failure_stage": 0, "failure_code": 0,
            "working_set_base_bytes": 10000000, "working_set_final_bytes": 11000000, "working_set_peak_bytes": 12000000,
            "executions": 35, "runs": [{"capture_ms": 1.0, "format_ms": 2.0, "commit_ms": 3.0, "total_ms": 7.0,
                                        "output_bytes": size - 100, "core_peak_bytes": size * 3, "ui_ticks": 1, "ui_max_gap_ms": 2.0} for _ in range(35)]}


def report():
    identity = {"size": 1024, "sha256": "a" * 64, "machine": "x64", "pe_valid": True}
    build = {"configuration": "Release", "architecture": "x64", "runtime": "MultiThreaded", "compiler": "19.44.35229.0",
             "compiler_full_version": 194435229, "toolset": "14.44.35207", "windows_sdk": "10.0.26100.0", "sdk_revision": benchmark.SDK_REVISION, "scintilla_version": "5.6.6",
             "dll_sha256": identity["sha256"], "source_revision": "b" * 40, "source_modified": True, "source_digest": "c" * 64}
    value = {"schema": 2, "complete": True, "environment": {"os": "Windows 10", "windows_major": 10, "windows_minor": 0,
             "windows_build": 19045, "cpu_model": "Synthetic CPU", "physical_cores": 4, "logical_processors": 8,
             "ram_bytes": 16 * 1024**3, "storage_reference": "not-verified", "architecture": "x64"},
             "provenance": {"host": identity, "benchmark_dll": copy.deepcopy(identity), "archive_sha256": benchmark.ARCHIVE_SHA256,
              "host_version": "8.9.8.1", "scintilla_version_source": "pinned-sdk-and-official-host-archive", "build": build,
              "source_revision": "b" * 40, "source_modified": True},
             "protocol": {"warmups": 5, "samples": 30, "p95": "nearest-rank", "fresh_process_per_scenario": True,
              "ui_timer_period_ms": 10, "working_set_peak": "process-lifetime-os-high-water", "core_peak": "shared-budget-exact-acquisition-high-water"},
             "scenarios": [benchmark.summarize(native(size, mode)) for size in benchmark.SIZES for mode in benchmark.MODES],
             "errors": [], "discrepancies": [], "reference_nfr002": "not-promoted", "other_release_gates": "pending", "release_blocked": True}
    value["discrepancies"] = benchmark.discrepancies(value)
    return value


class BenchmarkUiTests(unittest.TestCase):
    def test_complete_six_scenarios_and_p95_excludes_warmups(self):
        value = native()
        for run in value["runs"][:5]:
            run.update(capture_ms=1000, total_ms=1007)
        for index, run in enumerate(value["runs"][5:]):
            run.update(capture_ms=index, total_ms=index + 6)
        self.assertEqual(benchmark.summarize(value)["p95_ms"]["capture_ms"], 28)
        benchmark.validate_report(report())

    def test_missing_nonfinite_wrong_size_or_versions_rejected(self):
        changes = (("executions", 34), ("complete", False), ("host_version_code", 1),
                   ("compiler_version", 193800000), ("input_bytes", True), ("worker_scintilla_calls", 1))
        for key, replacement in changes:
            value = native(); value[key] = replacement
            with self.assertRaises(ValueError): benchmark.validate_native(value, 262144, "Pretty")
        value = native(); value["runs"].pop()
        with self.assertRaises(ValueError): benchmark.summarize(value)
        for invalid in (float("nan"), float("inf"), -1, True, 10**400):
            value = native(); value["runs"][3]["capture_ms"] = invalid
            with self.assertRaises(ValueError): benchmark.summarize(value)

    def test_memory_and_duration_consistency(self):
        for key, replacement in (("output_bytes", 5242881), ("core_peak_bytes", 33554433), ("total_ms", 5), ("ui_ticks", True)):
            value = native(); value["runs"][0][key] = replacement
            with self.assertRaises(ValueError): benchmark.summarize(value)
        value = native(); value["working_set_peak_bytes"] = 1
        with self.assertRaises(ValueError): benchmark.summarize(value)

    def test_failures_published_without_relaxing_gate(self):
        value = report()
        for run in value["scenarios"][0]["native"]["runs"]:
            run.update(commit_ms=50, total_ms=54)
        value["scenarios"][0] = benchmark.summarize(value["scenarios"][0]["native"])
        value["discrepancies"] = benchmark.discrepancies(value)
        benchmark.validate_report(value)
        self.assertEqual(value["scenarios"][0]["commit_gate"], "failed")
        self.assertTrue(value["release_blocked"])
        self.assertTrue(any("50ms" in discrepancy for discrepancy in value["discrepancies"]))

    def test_incomplete_report_is_explicit_and_cannot_be_complete(self):
        value = report(); removed = value["scenarios"].pop()["native"]
        value["complete"] = False
        value["errors"] = [{"input_bytes": removed["input_bytes"], "mode": removed["mode"], "reason": "timeout", "executions": -1, "failure_stage": -1, "failure_code": -1}]
        value["discrepancies"] = benchmark.discrepancies(value)
        benchmark.validate_report(value)
        value["complete"] = True
        with self.assertRaises(ValueError): benchmark.validate_report(value)

    def test_sensitive_fields_duplicates_false_summaries_and_promotions_rejected(self):
        for section in (None, "environment", "provenance", "protocol"):
            value = report(); (value if section is None else value[section])["document"] = "forbidden"
            with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["scenarios"][0]["native"]["runs"][0]["document_sha256"] = "forbidden"
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        for key, replacement in (("release_blocked", False), ("reference_nfr002", "passed")):
            value = report(); value[key] = replacement
            with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["scenarios"][0]["p95_ms"]["capture_ms"] = 0
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["scenarios"][1] = copy.deepcopy(value["scenarios"][0])
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["scenarios"][0]["p95_ms"]["capture_ms"] = True
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["protocol"]["fresh_process_per_scenario"] = 1
        with self.assertRaises(ValueError): benchmark.validate_report(value)

    def test_protected_outputs_rejected_before_execution_or_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); (root / "docs").mkdir(); (root / ".build/benchmark-results").mkdir(parents=True)
            dll = root / "native.dll"; dll.write_bytes(b"synthetic PE")
            for output in (dll, root / "scripts/tool.py", root / "CMakePresets.json", root / "docs/notes.json"):
                with self.assertRaises(ValueError): benchmark.reject_output(output, (dll,), root)
            allowed = root / "docs/benchmarks-ui-results.json"
            benchmark.reject_output(allowed, (dll,), root)
            allowed.hardlink_to(dll)
            with self.assertRaises(ValueError): benchmark.reject_output(allowed, (dll,), root)
            self.assertEqual(dll.read_bytes(), b"synthetic PE")
            allowed.unlink(); allowed.write_text("{}", encoding="utf-8")
            with self.assertRaises(ValueError): benchmark.reject_output(allowed, (dll,), root)

    def test_strict_utf8_duplicates_and_nonfinite_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            for data in (b"\xff", b'{"schema":1,"schema":1}', b'{"a":NaN}'):
                path.write_bytes(data)
                with self.assertRaises((UnicodeError, ValueError)): benchmark.load_json(path)

    def test_wrong_toolchain_or_binary_rejected(self):
        value = report()
        for key, replacement in (("configuration", "Debug"), ("runtime", "MultiThreadedDLL"), ("scintilla_version", "0")):
            build = copy.deepcopy(value["provenance"]["build"]); build[key] = replacement
            with self.assertRaises(ValueError): benchmark.validate_build(build)
        identity = value["provenance"]["host"]; identity["machine"] = "x86"
        with self.assertRaises(ValueError): benchmark.validate_identity(identity)

    def test_native_runner_success_and_cleanup_only_its_own_process(self):
        metadata = report()["provenance"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); portable = root / "portable"; portable.mkdir()
            for name in ("notepad++.exe", "langs.model.xml", "stylers.model.xml"):
                (portable / name).write_bytes(b"synthetic fixture")
            dll = root / "benchmark.dll"; dll.write_bytes(b"synthetic fixture")
            process = unittest.mock.Mock(); process.poll.return_value = None
            def launch(*args, **kwargs):
                destination = Path(kwargs["env"]["NPP_LDAP_BENCHMARK_OUTPUT"])
                destination.write_text(json.dumps(native()), encoding="utf-8")
                self.assertIn("-multiInst", args[0]); self.assertIn("-nosession", args[0])
                self.assertIn('isSnapshotMode="no"', (destination.parent / "config.xml").read_text(encoding="utf-8"))
                self.assertFalse((destination.parent / "updater").exists())
                return process
            with patch.object(benchmark.subprocess, "Popen", side_effect=launch) as execute, patch.object(benchmark, "inspect_pe", return_value=metadata["benchmark_dll"]):
                result = benchmark.run_scenario(portable, dll, 262144, "Pretty", 10, root, metadata["build"])
            self.assertEqual(result["samples"], 30)
            execute.assert_called_once(); process.terminate.assert_called_once(); process.wait.assert_called_once_with(timeout=10)
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["benchmark.dll", "portable"])

    def test_native_timeout_and_premature_exit_cleanup(self):
        metadata = report()["provenance"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); portable = root / "portable"; portable.mkdir()
            for name in ("notepad++.exe", "langs.model.xml", "stylers.model.xml"):
                (portable / name).write_bytes(b"synthetic fixture")
            dll = root / "benchmark.dll"; dll.write_bytes(b"synthetic fixture")
            process = unittest.mock.Mock(); process.poll.return_value = None
            with patch.object(benchmark.subprocess, "Popen", return_value=process), patch.object(benchmark.time, "monotonic", side_effect=[0, 0, 100]), patch.object(benchmark.time, "sleep"), patch.object(benchmark, "inspect_pe", return_value=metadata["benchmark_dll"]):
                with self.assertRaises(TimeoutError): benchmark.run_scenario(portable, dll, 262144, "Pretty", 10, root, metadata["build"])
            process.terminate.assert_called_once(); process.wait.assert_called_once_with(timeout=10)
            process.reset_mock(); process.poll.return_value = 3
            with patch.object(benchmark.subprocess, "Popen", return_value=process), patch.object(benchmark, "inspect_pe", return_value=metadata["benchmark_dll"]):
                with self.assertRaises(ChildProcessError): benchmark.run_scenario(portable, dll, 262144, "Pretty", 10, root, metadata["build"])
            process.terminate.assert_not_called(); process.wait.assert_called_once_with(timeout=10)

    def test_failure_diagnostics_cannot_persist_document_fields(self):
        error = benchmark.NativeResultError({"executions": 3, "failure_stage": 4, "failure_code": 11, "document": "forbidden"})
        self.assertEqual(error.numeric, {"executions": 3, "failure_stage": 4, "failure_code": 11})
        error = benchmark.NativeResultError({"executions": "forbidden", "failure_stage": True, "failure_code": 1000})
        self.assertEqual(error.numeric, {"executions": -1, "failure_stage": -1, "failure_code": -1})

    def test_manifest_binds_hash_exact_compiler_and_build_source_provenance(self):
        metadata = report()["provenance"]
        benchmark.validate_binding(metadata["build"], metadata["benchmark_dll"], 194435229)
        for wrong in (194435219, 194435230):
            with self.assertRaises(ValueError): benchmark.validate_binding(metadata["build"], metadata["benchmark_dll"], wrong)
        for key in ("dll_sha256", "source_digest"):
            build = copy.deepcopy(metadata["build"]); build[key] = "invalid"
            with self.assertRaises(ValueError): benchmark.validate_build(build)
        dll = copy.deepcopy(metadata["benchmark_dll"]); dll["sha256"] = "d" * 64
        with self.assertRaises(ValueError): benchmark.validate_binding(metadata["build"], dll)
        value = report(); value["provenance"]["source_revision"] = "d" * 40
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        value = report(); value["scenarios"][0]["native"]["compiler_version"] = 194435219
        with self.assertRaises(ValueError): benchmark.validate_report(value)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(benchmark, "inspect_pe", return_value=dll), patch.object(benchmark.subprocess, "Popen") as execute:
                with self.assertRaises(ValueError): benchmark.run_scenario(root, root / "benchmark.dll", 262144, "Pretty", 10, root, metadata["build"])
            execute.assert_not_called()

    def test_native_api_rejects_arm64_os_even_for_x64_process(self):
        for architecture in (0, 12, 0xFFFF):
            kernel = unittest.mock.Mock()
            kernel.GetNativeSystemInfo.side_effect = lambda pointer: setattr(pointer._obj, "architecture", architecture)
            with patch.object(benchmark.ctypes, "WinDLL", return_value=kernel):
                with self.assertRaises(ValueError): benchmark.native_architecture()
        kernel.GetNativeSystemInfo.side_effect = lambda pointer: setattr(pointer._obj, "architecture", 9)
        with patch.object(benchmark.ctypes, "WinDLL", return_value=kernel):
            self.assertEqual(benchmark.native_architecture(), "x64")

    def test_failed_json_write_and_close_leave_no_temporary_file(self):
        for phase in ("write", "close"):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); output = root / "result.json"
                original_factory = benchmark.tempfile.NamedTemporaryFile
                class FailingFile:
                    def __enter__(self):
                        self.file = original_factory(mode="w", encoding="utf-8", dir=root, suffix=".json", delete=False)
                        return self.file
                    def __exit__(self, *args):
                        self.file.close()
                        if phase == "close": raise OSError("Synthetic flush failure")
                with patch.object(sys, "argv", ["benchmark_ui.py", "--output", str(output)]), patch.object(benchmark, "reject_output"), patch.object(benchmark, "run_matrix", return_value=report()), patch.object(benchmark.tempfile, "NamedTemporaryFile", side_effect=lambda **kwargs: FailingFile()), patch("builtins.print"):
                    if phase == "write":
                        with patch.object(benchmark.json, "dump", side_effect=OSError("Synthetic write failure")):
                            self.assertEqual(benchmark.main(), 1)
                    else:
                        self.assertEqual(benchmark.main(), 1)
                self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
