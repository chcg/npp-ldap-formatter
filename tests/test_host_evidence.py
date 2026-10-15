import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

_spec = importlib.util.spec_from_file_location("host_evidence", Path(__file__).resolve().parents[1] / "scripts" / "host_evidence.py")
evidence = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(evidence)


class HostEvidenceTests(unittest.TestCase):
    def setUp(self):
        summary = {"schema": 1, "suite": "simulated-host", "cases": 100, "failures": 0, "containment_cases": 6, "integrity_loss_cases": 4,
                   "preservation_gate": "failed", "native_gates": "pending", "release_blocked": True,
                   "fault_observations": [{"case": index, "text_equal": index >= 4, "selection_equal": True, "history_equal": True, "replacements": 1, "undo_calls": 0} for index in range(6)]}
        dll = {"size": 1024, "sha256": "a" * 64, "machine": "x64", "pe_valid": True}
        self.report = evidence.consolidate(summary, dll, "b" * 40)

    def test_valid_simulation_keeps_native_pending(self):
        evidence.validate_report(self.report)
        self.assertTrue(self.report["release_blocked"])
        self.assertFalse(self.report["native_certified"])

    def test_loss_cannot_pass_preservation(self):
        self.report["simulated"]["preservation_gate"] = "passed"
        with self.assertRaises(ValueError): evidence.validate_report(self.report)

    def test_containment_is_not_preservation(self):
        self.assertEqual(self.report["gates"]["simulated_containment"], "passed")
        self.assertEqual(self.report["gates"]["simulated_preservation"], "failed")

    def test_missing_gates_rejected(self):
        del self.report["gates"]["native_callbacks"]
        with self.assertRaises(ValueError): evidence.validate_report(self.report)

    def test_every_native_gate_cannot_be_promoted_by_simulation(self):
        for gate in evidence.GATES:
            report = copy.deepcopy(self.report); report["gates"][gate] = "passed"
            with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_sensitive_fields_rejected_at_every_boundary(self):
        for key in ("document", "filter", "dn", "credentials", "document_sha256", "text"):
            for section in (None, "environment", "dll", "simulated", "gates"):
                report = copy.deepcopy(self.report)
                (report if section is None else report[section])[key] = "sensitive"
                with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_release_or_native_certification_rejected(self):
        for key, value in (("release_blocked", False), ("native_certified", True)):
            report = copy.deepcopy(self.report); report[key] = value
            with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_metadata_values_cannot_carry_content(self):
        for section, key in (("environment", "os"), ("environment", "architecture"), ("dll", "sha256"), ("dll", "machine")):
            report = copy.deepcopy(self.report); report[section][key] = "(cn=sensitive)"
            with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_boolean_counter_rejected(self):
        self.report["simulated"]["cases"] = True
        with self.assertRaises(ValueError): evidence.validate_report(self.report)

    def test_schema_requires_exact_integer(self):
        for version in (True, 1.0):
            for section in (None, "simulated"):
                report = copy.deepcopy(self.report)
                (report if section is None else report[section])["schema"] = version
                with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_empty_or_inconsistent_coverage_rejected(self):
        for key, value in (("cases", 0), ("integrity_loss_cases", 101), ("containment_cases", 101)):
            report = copy.deepcopy(self.report); report["simulated"][key] = value
            with self.assertRaises(ValueError): evidence.validate_report(report)

    def test_architecture_mismatch_rejected(self):
        self.report["dll"]["machine"] = "ARM64"
        with self.assertRaises(ValueError): evidence.validate_report(self.report)

    def test_pe_inspection_checks_headers_and_machine(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.dll"
            data = bytearray(128); data[:2] = b"MZ"; struct.pack_into("<I", data, 60, 64); data[64:68] = b"PE\0\0"; struct.pack_into("<H", data, 68, 0x8664)
            path.write_bytes(data)
            identity = evidence.inspect_pe(path)
            self.assertEqual(identity, {"size": 128, "sha256": "d1a163b6d2e1ca2d24467cc99302770cca68a3bcc2e610f724fca69b410dd19a", "machine": "x64", "pe_valid": True})
            data[-1] = 1; path.write_bytes(data)
            changed = evidence.inspect_pe(path)
            self.assertEqual(changed["size"], 128)
            self.assertEqual(changed["sha256"], "a1b842c8972bd220fd3d42d99181fd9b64cdad128c1ef7d7cbb25b17ca340967")
            self.assertNotEqual(changed["sha256"], identity["sha256"])
            for broken in (b"", b"MZ", bytes(128)):
                path.write_bytes(broken)
                with self.assertRaises(ValueError): evidence.inspect_pe(path)

    def test_runner_rejects_unstructured_or_sensitive_stdout(self):
        for stdout in (b"not metadata", b"\xff", json.dumps({**self.report["simulated"], "document": "sensitive"}).encode()):
            with patch.object(evidence, "inspect_pe", return_value=self.report["dll"]), patch.object(evidence.subprocess, "run", return_value=SimpleNamespace(stdout=stdout, stderr=b"sensitive", returncode=0)):
                with self.assertRaises(ValueError): evidence.run(Path("harness.exe"), Path("plugin.dll"), "b" * 40)

    def test_runner_rejects_failure_exit_even_with_green_summary(self):
        with patch.object(evidence, "inspect_pe", return_value=self.report["dll"]), patch.object(evidence.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(self.report["simulated"]).encode(), stderr=b"", returncode=3)):
            with self.assertRaises(ValueError): evidence.run(Path("harness.exe"), Path("plugin.dll"), "b" * 40)

    def test_runner_rejects_stderr_even_with_valid_stdout(self):
        with patch.object(evidence, "inspect_pe", return_value=self.report["dll"]), patch.object(evidence.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(self.report["simulated"]).encode(), stderr=b"private-marker", returncode=0)):
            with self.assertRaises(ValueError) as rejected:
                evidence.run(Path("harness.exe"), Path("plugin.dll"), "b" * 40)
            self.assertNotIn("private-marker", str(rejected.exception))

    def test_runner_success_returns_complete_report(self):
        expected = {"schema": 1, "environment": {"os": "Windows", "architecture": "x64", "host_version": "8.9.8.1", "scintilla_version": "5.6.6"},
                    "revision": "b" * 40, "dll": self.report["dll"], "simulated": self.report["simulated"],
                    "gates": {"native_callbacks": "pending", "native_undo_selection": "pending", "native_failure_preservation": "pending", "native_arm64": "pending", "simulated_containment": "passed", "simulated_preservation": "failed"},
                    "release_blocked": True, "native_certified": False}
        harness, dll = Path("harness.exe"), Path("plugin.dll")
        with patch.object(evidence, "inspect_pe", return_value=self.report["dll"]), patch.object(evidence.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(self.report["simulated"]).encode(), stderr=b"", returncode=0)) as execute:
            self.assertEqual(evidence.run(harness, dll, "b" * 40), expected)
            execute.assert_called_once_with([str(harness.resolve()), str(dll.resolve())], capture_output=True, timeout=60, check=False)

    def test_output_cannot_alias_either_input(self):
        with tempfile.TemporaryDirectory() as directory:
            harness, dll = Path(directory) / "harness.exe", Path(directory) / "plugin.dll"
            originals = {harness: b"synthetic harness", dll: b"synthetic DLL"}
            for path, data in originals.items(): path.write_bytes(data)
            for source in (harness, dll):
                link = Path(directory) / (source.name + ".link"); link.hardlink_to(source)
                for output in (source, link):
                    with patch.object(sys, "argv", ["host_evidence.py", "--harness", str(harness), "--dll", str(dll), "--revision", "b" * 40, "--output", str(output)]), patch.object(evidence.subprocess, "run") as execute, patch("builtins.print"):
                        self.assertEqual(evidence.main(), 1)
                        execute.assert_not_called()
                    for path, data in originals.items(): self.assertEqual(path.read_bytes(), data)

    def test_cli_writes_missing_and_unrelated_existing_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            harness, dll = Path(directory) / "harness.exe", Path(directory) / "plugin.dll"
            originals = {harness: b"synthetic harness", dll: b"synthetic DLL"}
            for path, data in originals.items(): path.write_bytes(data)
            for existing in (False, True):
                output = Path(directory) / ("existing.json" if existing else "missing.json")
                if existing: output.write_text('{"previous":true}', encoding="utf-8")
                with patch.object(sys, "argv", ["host_evidence.py", "--harness", str(harness), "--dll", str(dll), "--revision", "b" * 40, "--output", str(output)]), patch.object(evidence, "run", return_value=self.report) as execute, patch("builtins.print"):
                    self.assertEqual(evidence.main(), 0)
                    execute.assert_called_once_with(harness, dll, "b" * 40)
                self.assertEqual(json.loads(output.read_text(encoding="utf-8", errors="strict")), self.report)
                for path, data in originals.items(): self.assertEqual(path.read_bytes(), data)

    def test_cli_rechecks_output_alias_after_run(self):
        with tempfile.TemporaryDirectory() as directory:
            harness, dll = Path(directory) / "harness.exe", Path(directory) / "plugin.dll"
            originals = {harness: b"synthetic harness", dll: b"synthetic DLL"}
            for path, data in originals.items(): path.write_bytes(data)
            for source in (harness, dll):
                output = Path(directory) / (source.name + ".json")
                def create_alias(*args):
                    output.hardlink_to(source)
                    return self.report
                with patch.object(sys, "argv", ["host_evidence.py", "--harness", str(harness), "--dll", str(dll), "--revision", "b" * 40, "--output", str(output)]), patch.object(evidence, "run", side_effect=create_alias) as execute, patch("builtins.print"):
                    self.assertEqual(evidence.main(), 1)
                    execute.assert_called_once_with(harness, dll, "b" * 40)
                self.assertTrue(output.samefile(source))
                self.assertEqual(output.read_bytes(), originals[source])
                for path, data in originals.items(): self.assertEqual(path.read_bytes(), data)

    def test_fault_equalities_are_required_and_consistent(self):
        self.report["simulated"]["fault_observations"][0]["text_equal"] = True
        with self.assertRaises(ValueError): evidence.validate_report(self.report)

    def test_faults_cannot_hide_unsafe_recovery(self):
        for key, value in (("replacements", 2), ("undo_calls", 1)):
            report = copy.deepcopy(self.report); report["simulated"]["fault_observations"][0][key] = value
            with self.assertRaises(ValueError): evidence.validate_report(report)


if __name__ == "__main__": unittest.main()
