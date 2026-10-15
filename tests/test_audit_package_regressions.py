import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

from scripts import audit_package_regressions as audit


class PackageAuditTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.archive = self.root / "package.zip"
        self.harness = self.root / "harness.exe"
        self.manifest = self.root / "approved.json"
        self.dll = self.pe("x64")
        self.harness.write_bytes(self.dll)
        self.package(self.dll)
        self.summary = {"schema": 1, "suite": "simulated-host", "cases": 100, "failures": 0,
                        "containment_cases": 6, "integrity_loss_cases": 4, "preservation_gate": "failed",
                        "native_gates": "pending", "release_blocked": True,
                        "fault_observations": [{"case": i, "text_equal": i >= 4, "selection_equal": True,
                                                "history_equal": True, "replacements": 1, "undo_calls": 0} for i in range(6)]}
        self.approve()
        for name, value in (("system", "Windows"), ("machine", "AMD64")):
            mocked = patch.object(audit.platform, name, return_value=value)
            mocked.start()
            self.addCleanup(mocked.stop)

    @staticmethod
    def pe(arch):
        data = bytearray(512)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<HH", data, 68, audit.MACHINES[arch], 1)
        struct.pack_into("<HHH", data, 84, 240, 0x2000, 0x10B if arch == "x86" else 0x20B)
        return bytes(data)

    def approve(self, arch="x64"):
        value = {"schema": 1, "revision": "b" * 40, "architecture": arch,
                 "archive_sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest(),
                 "dll_sha256": hashlib.sha256(self.dll).hexdigest(),
                 "harness_sha256": hashlib.sha256(self.harness.read_bytes()).hexdigest(), "suite_cases": 100}
        self.manifest.write_text(json.dumps(value), encoding="utf-8")
        return value

    def package(self, dll):
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as zipped:
            zipped.writestr(audit.DLL_NAME, dll)
        self.checksum()

    def checksum(self):
        self.archive.with_suffix(".zip.sha256").write_text(
            hashlib.sha256(self.archive.read_bytes()).hexdigest() + "  " + self.archive.name + "\n", encoding="utf-8")

    def run_audit(self):
        return audit.run(self.archive, "x64", self.harness, "b" * 40, self.manifest)

    def test_success_runs_exact_extracted_dll_and_preserves_gates_and_inputs(self):
        originals = {path: path.read_bytes() for path in (self.archive, self.archive.with_suffix(".zip.sha256"), self.harness, self.manifest)}
        paths = []
        def execute(command, **kwargs):
            paths.append(Path(command[1]))
            self.assertEqual(paths[-1].read_bytes(), self.dll)
            self.assertEqual(Path(command[0]), self.harness.resolve())
            return SimpleNamespace(stdout=json.dumps(self.summary).encode(), stderr=b"", returncode=0)
        with patch.object(audit.host_evidence.subprocess, "run", side_effect=execute) as harness:
            result = self.run_audit()
        harness.assert_called_once()
        self.assertEqual(set(result), {"schema", "archive_sha256", "harness", "report"})
        self.assertEqual(result["harness"], audit.host_evidence.inspect_pe(self.harness))
        self.assertEqual(result["archive_sha256"], hashlib.sha256(originals[self.archive]).hexdigest())
        self.assertEqual(result["report"]["dll"]["sha256"], hashlib.sha256(self.dll).hexdigest())
        self.assertEqual(result["report"]["gates"]["simulated_preservation"], "failed")
        self.assertTrue(result["report"]["release_blocked"])
        self.assertFalse(result["report"]["native_certified"])
        self.assertFalse(paths[0].parent.exists())
        for path, data in originals.items(): self.assertEqual(path.read_bytes(), data)

    def test_invalid_inputs_do_not_execute(self):
        archive_bytes = self.archive.read_bytes()
        checksum_bytes = self.archive.with_suffix(".zip.sha256").read_bytes()
        def bad_sha(): self.archive.with_suffix(".zip.sha256").write_text("0" * 64 + "  package.zip\n", encoding="utf-8")
        def bad_zip(): self.archive.write_bytes(b"invalid ZIP"); self.checksum()
        def bad_pe(): self.package(b"x" * 512)
        def bad_arch():
            data = bytearray(self.dll); struct.pack_into("<H", data, 68, 0x14C); self.package(data)
        def bad_size(): self.package(b"x" * (8 * 1024**2 + 1))
        def long_checksum(): self.archive.with_suffix(".zip.sha256").write_text("x" * 1025, encoding="utf-8")
        for corrupt in (bad_sha, bad_zip, bad_pe, bad_arch, bad_size, long_checksum):
            with self.subTest(case=corrupt.__name__):
                self.archive.write_bytes(archive_bytes)
                self.archive.with_suffix(".zip.sha256").write_bytes(checksum_bytes)
                corrupt()
                with patch.object(audit.host_evidence.subprocess, "run") as execute:
                    with self.assertRaises((ValueError, zipfile.BadZipFile)): self.run_audit()
                    execute.assert_not_called()
        self.archive.write_bytes(b"x" * (audit.MAX_ARCHIVE_BYTES + 1))
        self.checksum()
        with patch.object(audit.host_evidence.subprocess, "run") as execute:
            with self.assertRaises((ValueError, zipfile.BadZipFile)): self.run_audit()
            execute.assert_not_called()

    def test_snapshot_cannot_be_replaced_between_validation_and_execution(self):
        snapshot = self.archive.read_bytes()
        validate = audit.validate_package_data
        def replace_after_read(data, *args):
            self.assertEqual(data, snapshot)
            self.archive.write_bytes(b"replacement")
            return validate(data, *args)
        def execute(command, **kwargs):
            self.assertEqual(Path(command[1]).read_bytes(), self.dll)
            return SimpleNamespace(stdout=json.dumps(self.summary).encode(), stderr=b"", returncode=0)
        with patch.object(audit, "validate_package_data", side_effect=replace_after_read), patch.object(audit.host_evidence.subprocess, "run", side_effect=execute):
            self.assertEqual(self.run_audit()["archive_sha256"], hashlib.sha256(snapshot).hexdigest())
        self.assertEqual(self.archive.read_bytes(), b"replacement")

    def test_architecture_revision_and_platform_rejected_before_execution(self):
        with patch.object(audit.host_evidence.subprocess, "run") as execute:
            for arch, revision in (("invalid", "b" * 40), ("x64", "invalid")):
                with self.assertRaises(ValueError): audit.run(self.archive, arch, self.harness, revision, self.manifest)
            for system, machine in (("Linux", "AMD64"), ("Windows", "ARM64"), ("Windows", "unknown")):
                with patch.object(audit.platform, "system", return_value=system), patch.object(audit.platform, "machine", return_value=machine):
                    with self.assertRaisesRegex(ValueError, "Plataforma"): self.run_audit()
            data = bytearray(self.dll); struct.pack_into("<H", data, 68, 0xAA64)
            self.harness.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Harness"): self.run_audit()
            self.dll = bytes(data)
            self.package(data)
            self.approve("ARM64")
            with self.assertRaisesRegex(ValueError, "Plataforma"): audit.run(self.archive, "ARM64", self.harness, "b" * 40, self.manifest)
            execute.assert_not_called()

    def test_cli_sanitizes_harness_failure_or_sensitive_stdout_and_cleans_temporary(self):
        command = ["audit_package_regressions.py", "--archive", str(self.archive), "--arch", "x64",
                   "--harness", str(self.harness), "--revision", "b" * 40, "--manifest", str(self.manifest)]
        paths = []
        for stdout, code in ((b"private-marker", 0), (b"\xff", 0),
                             (json.dumps({**self.summary, "document": "private-marker"}).encode(), 0),
                             (json.dumps(self.summary).encode(), 0),
                             (json.dumps(self.summary).encode(), 7)):
            def execute(args, **kwargs):
                paths.append(Path(args[1]))
                return SimpleNamespace(stdout=stdout, stderr=b"private-marker", returncode=code)
            output = io.StringIO()
            with patch.object(sys, "argv", command), patch.object(audit.host_evidence.subprocess, "run", side_effect=execute), contextlib.redirect_stdout(output):
                self.assertEqual(audit.main(), 1)
            self.assertNotIn("private-marker", output.getvalue())
            self.assertNotIn(str(self.root), output.getvalue())
            self.assertFalse(paths[-1].parent.exists())

    def test_cli_prints_only_strict_report(self):
        command = ["audit_package_regressions.py", "--archive", str(self.archive), "--arch", "x64",
                   "--harness", str(self.harness), "--revision", "b" * 40, "--manifest", str(self.manifest)]
        output = io.StringIO()
        with patch.object(sys, "argv", command), patch.object(audit.host_evidence.subprocess, "run", return_value=SimpleNamespace(stdout=json.dumps(self.summary).encode(), stderr=b"", returncode=0)), contextlib.redirect_stdout(output):
            self.assertEqual(audit.main(), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(set(result), {"schema", "archive_sha256", "harness", "report"})
        audit.host_evidence.validate_report(result["report"])

    def test_report_must_identify_snapshot_dll(self):
        report = audit.host_evidence.consolidate(self.summary, audit.host_evidence.inspect_pe(self.harness), "b" * 40)
        report["dll"]["sha256"] = "a" * 64
        with patch.object(audit.host_evidence, "run", return_value=report):
            with self.assertRaisesRegex(ValueError, "snapshot"): self.run_audit()

    def test_cli_sanitizes_timeout(self):
        command = ["audit_package_regressions.py", "--archive", str(self.archive), "--arch", "x64",
                   "--harness", str(self.harness), "--revision", "b" * 40, "--manifest", str(self.manifest)]
        output = io.StringIO()
        with patch.object(sys, "argv", command), patch.object(audit.host_evidence.subprocess, "run", side_effect=subprocess.TimeoutExpired("private-marker", 60, output=b"private-marker")), contextlib.redirect_stdout(output):
            self.assertEqual(audit.main(), 1)
        self.assertNotIn("private-marker", output.getvalue())

    def test_positive_architecture_dispatch_uses_synthetic_harness(self):
        for arch, machine in (("x64", "AMD64"), ("x86", "AMD64"), ("ARM64", "ARM64")):
            with self.subTest(architecture=arch):
                self.dll = self.pe(arch)
                self.harness.write_bytes(self.dll)
                self.package(self.dll)
                self.approve(arch)
                completed = SimpleNamespace(stdout=json.dumps(self.summary).encode(), stderr=b"", returncode=0)
                with patch.object(audit.platform, "machine", return_value=machine), patch.object(audit.host_evidence.subprocess, "run", return_value=completed) as execute:
                    result = audit.run(self.archive, arch, self.harness, "b" * 40, self.manifest)
                execute.assert_called_once()
                self.assertEqual(result["report"]["environment"]["architecture"], arch)
                self.assertEqual(result["harness"]["machine"], arch)

    def test_wellformed_manifest_identity_mismatches_do_not_execute(self):
        original = self.approve()
        for key, value in (("revision", "c" * 40), ("architecture", "x86"),
                           ("archive_sha256", "a" * 64), ("dll_sha256", "a" * 64), ("harness_sha256", "a" * 64)):
            with self.subTest(field=key), patch.object(audit.host_evidence.subprocess, "run") as execute:
                self.manifest.write_text(json.dumps({**original, key: value}), encoding="utf-8")
                with self.assertRaises(ValueError): self.run_audit()
                execute.assert_not_called()

    def test_manifest_requires_exact_expected_suite_count(self):
        value = self.approve()
        self.manifest.write_text(json.dumps({**value, "suite_cases": 99}), encoding="utf-8")
        completed = SimpleNamespace(stdout=json.dumps(self.summary).encode(), stderr=b"", returncode=0)
        with patch.object(audit.host_evidence.subprocess, "run", return_value=completed) as execute:
            with self.assertRaisesRegex(ValueError, "Cobertura"): self.run_audit()
        execute.assert_called_once()

    def test_malformed_manifest_is_rejected_before_execution(self):
        value = self.approve()
        encoded = json.dumps(value).encode()
        bad_fields = [("schema", True), ("schema", 1.0), ("schema", 2), ("revision", False),
                      ("architecture", []), ("architecture", "unknown"), ("archive_sha256", "A" * 64),
                      ("dll_sha256", "a" * 63), ("harness_sha256", 123), ("suite_cases", True),
                      ("suite_cases", 100.0), ("suite_cases", 0), ("suite_cases", -1), ("suite_cases", "100")]
        malformed = [b"", b"\xff", b"{private-marker", b"[]", b"null", b"x" * 4097,
                     json.dumps({key: item for key, item in value.items() if key != "schema"}).encode(),
                     json.dumps({**value, "document": "private-marker"}).encode(),
                     encoded.replace(b'"schema": 1', b'"schema": 1, "schema": 1')]
        malformed.extend(json.dumps({**value, key: item}).encode() for key, item in bad_fields)
        with patch.object(audit.host_evidence.subprocess, "run") as execute:
            for index, raw in enumerate(malformed):
                with self.subTest(case=index):
                    self.manifest.write_bytes(raw)
                    with self.assertRaises(ValueError): self.run_audit()
            execute.assert_not_called()
        self.manifest.write_bytes(encoded + b" " * (4096 - len(encoded)))
        self.assertEqual(audit.load_manifest(self.manifest), value)

    def test_cli_argument_errors_never_echo_sensitive_values(self):
        original = ["audit_package_regressions.py", "--archive", str(self.archive), "--arch", "x64",
                    "--harness", str(self.harness), "--revision", "b" * 40, "--manifest", str(self.manifest)]
        invalid_arch = original.copy()
        invalid_arch[invalid_arch.index("--arch") + 1] = "private-marker"
        for command in (original + ["--private-marker", "private-marker"], invalid_arch):
            output, errors = io.StringIO(), io.StringIO()
            with patch.object(sys, "argv", command), contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors), patch.object(audit.host_evidence.subprocess, "run") as execute:
                with self.assertRaises(SystemExit) as rejected: audit.main()
                self.assertEqual(rejected.exception.code, 2)
                execute.assert_not_called()
            self.assertEqual(output.getvalue(), "")
            self.assertNotIn("private-marker", errors.getvalue())
            self.assertNotIn(str(self.root), errors.getvalue())

    def test_cli_does_not_output_manifest_content(self):
        self.manifest.write_text('{"private-marker": true}', encoding="utf-8")
        command = ["audit_package_regressions.py", "--archive", str(self.archive), "--arch", "x64",
                   "--harness", str(self.harness), "--revision", "b" * 40, "--manifest", str(self.manifest)]
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", command), contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            self.assertEqual(audit.main(), 1)
        self.assertNotIn("private-marker", output.getvalue() + errors.getvalue())


if __name__ == "__main__":
    unittest.main()
