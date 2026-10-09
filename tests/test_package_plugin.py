import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from scripts.package_plugin import DLL_NAME, MACHINES, package, project_version, validate_dll


def fake_dll(machine: int, characteristics: int = 0x2000) -> bytes:
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    struct.pack_into("<H", data, 70, 1)
    struct.pack_into("<H", data, 84, 240)
    struct.pack_into("<H", data, 86, characteristics)
    struct.pack_into("<H", data, 88, 0x10B if machine == MACHINES["x86"] else 0x20B)
    struct.pack_into("<II", data, 328 + 16, 512, 512)
    return bytes(data)


class PackageTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": ""})
        environment.start()
        self.addCleanup(environment.stop)

    def test_packages_all_machines_and_checks_exact_zip(self):
        for arch, machine in MACHINES.items():
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                dll = root / DLL_NAME
                dll.write_bytes(fake_dll(machine))
                archive = package(dll, root / "out", arch, "0.1.0")
                with zipfile.ZipFile(archive) as zipped:
                    self.assertEqual(zipped.namelist(), [DLL_NAME])
                    self.assertEqual(zipped.read(DLL_NAME), dll.read_bytes())
                expected = hashlib.sha256(archive.read_bytes()).hexdigest()
                self.assertEqual(archive.with_suffix(".zip.sha256").read_text(), f"{expected}  {archive.name}\n")
                with self.assertRaises(FileExistsError):
                    package(dll, root / "out", arch, "0.1.0")

    def test_rejects_invalid_pe_wrong_machine_and_executable(self):
        for data in (b"", b"MZ", fake_dll(MACHINES["x86"]), fake_dll(MACHINES["x64"], 0)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                validate_dll(data, "x64")
        malformed = bytearray(fake_dll(MACHINES["x64"]))
        struct.pack_into("<I", malformed, 0x3C, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            validate_dll(malformed, "x64")

    def test_missing_dll_does_not_create_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                package(root / DLL_NAME, root / "out", "x64", "0.1.0")
            self.assertFalse((root / "out").exists())

    def test_truncated_optional_header_and_sections(self):
        valid = fake_dll(MACHINES["x64"])
        for size in (88, 200, 340, 800):
            with self.subTest(size=size), self.assertRaises(ValueError):
                validate_dll(valid[:size], "x64")

    def test_checksum_only_collision_preserves_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dll = root / DLL_NAME
            dll.write_bytes(fake_dll(MACHINES["x64"]))
            output = root / "out"
            output.mkdir()
            checksum = output / "NppLdapFormatter-0.1.0-x64.zip.sha256"
            checksum.write_text("previous evidence", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                package(dll, output, "x64", "0.1.0")
            self.assertEqual(checksum.read_text(), "previous evidence")
            self.assertEqual(list(output.glob("*.zip")), [])

    def test_summary_isolated_from_real_job(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dll = root / DLL_NAME
            dll.write_bytes(fake_dll(MACHINES["x64"]))
            summary = root / "summary.md"
            with patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary)}):
                archive = package(dll, root / "out", "x64", "0.1.0")
            self.assertIn(hashlib.sha256(archive.read_bytes()).hexdigest(), summary.read_text())

    def test_cli_success_and_missing_input(self):
        script = Path(__file__).resolve().parents[1] / "scripts/package_plugin.py"
        version = project_version(script.parent.parent / "CMakeLists.txt")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dll = root / DLL_NAME
            dll.write_bytes(fake_dll(MACHINES["x64"]))
            command = [sys.executable, str(script), "--arch", "x64", "--dll", str(dll), "--output-dir", str(root / "out")]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / "out" / f"NppLdapFormatter-{version}-x64.zip").is_file())
            command[command.index("--dll") + 1] = str(root / "missing" / DLL_NAME)
            command[-1] = str(root / "missing-out")
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "missing-out").exists())

    def test_project_version_rejects_malformed_or_missing_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "CMakeLists.txt"
            with self.assertRaises(FileNotFoundError):
                project_version(manifest)
            for text in ("", "project(NppLdapFormatter VERSION 0.1 LANGUAGES NONE)"):
                manifest.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    project_version(manifest)


if __name__ == "__main__":
    unittest.main()
