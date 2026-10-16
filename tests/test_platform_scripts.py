import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

from scripts.build_matrix import ROOT, main, run_matrix
from scripts.package_plugin import DLL_NAME, MACHINES, package, project_version
from scripts.validate_packages import validate_package
from test_package_plugin import fake_dll


class PlatformScriptsTests(unittest.TestCase):
    def test_default_tool_cache_is_repository_local(self):
        with patch.object(sys, 'argv', ['build_matrix.py', '--arch', 'x64', '--test']), patch('scripts.build_matrix.run_matrix') as run:
            main()
        run.assert_called_once_with(['x64'], ['uvx', '--cache-dir', str(ROOT / '.uv-cache'), '--from', 'cmake==3.31.6', 'cmake'], True)

    def test_matrix_matches_ci_flags_and_presets(self):
        with patch('scripts.build_matrix.subprocess.run') as run:
            run_matrix(['x86', 'x64', 'ARM64'], ['cmake'], tests=False)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(commands), 6)
        for index, preset in enumerate(['win32', 'x64', 'arm64']):
            self.assertIn(preset, commands[index * 2])
            self.assertIn('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', commands[index * 2])
            self.assertIn('-DCMAKE_CXX_FLAGS=/analyze', commands[index * 2])
            self.assertEqual(commands[index * 2 + 1], ['cmake', '--build', '--preset', preset + '-release'])

    def test_build_failure_is_not_hidden(self):
        with patch('scripts.build_matrix.subprocess.run', side_effect=subprocess.CalledProcessError(7, 'cmake')) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                run_matrix(['x86', 'x64'], ['cmake'])
        self.assertEqual(run.call_count, 1)

    def test_test_mode_runs_x64_but_not_arm64_executable(self):
        with patch('scripts.build_matrix.subprocess.run') as run:
            run_matrix(['x64', 'ARM64'], ['cmake'], tests=True)
        commands = [call.args[0] for call in run.call_args_list]
        test_commands = [command for command in commands if '--test-dir' in command]
        self.assertEqual(len(test_commands), 1)
        self.assertIn('.build/x64', test_commands[0])
        self.assertIn('--no-tests=error', test_commands[0])

    def test_package_validation_rejects_tampering_and_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dll = root / DLL_NAME
            dll.write_bytes(fake_dll(0x8664))
            with patch.dict('os.environ', {'GITHUB_STEP_SUMMARY': ''}):
                archive = package(dll, root / 'out', 'x64', '0.1.0')
            self.assertEqual(validate_package(archive, 'x64', dll), hashlib.sha256(archive.read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                validate_package(archive, 'ARM64')
            checksum = archive.with_suffix('.zip.sha256')
            checksum.write_text('0' * 64 + '  ' + archive.name + '\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                validate_package(archive, 'x64')

    def test_uvx_prefix_and_explicit_cmake_path_with_x86_tests(self):
        for command in (['uvx', '--cache-dir', str(ROOT / '.uv-cache'), '--from', 'cmake==3.31.6', 'cmake'], ['C:/Tools/bin/cmake.exe']):
            with self.subTest(command=command), patch('scripts.build_matrix.subprocess.run') as run:
                run_matrix(['x86'], command, tests=True)
                ctest = run.call_args_list[-1].args[0]
                self.assertEqual(ctest[:len(command) - 1], command[:-1])
                self.assertTrue(ctest[len(command) - 1].endswith('ctest.exe' if command[-1].endswith('.exe') else 'ctest'))
                self.assertIn('.build/win32', ctest)

    def test_invalid_zip_layout_wrong_name_and_original_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'example.zip'
            data = fake_dll(0x8664)
            for members in ({'folder/' + DLL_NAME: data}, {DLL_NAME: data, 'extra.txt': b'extra'}):
                with zipfile.ZipFile(archive, 'w') as zipped:
                    for name, content in members.items():
                        zipped.writestr(name, content)
                archive.with_suffix('.zip.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name, encoding='utf-8')
                with self.assertRaises(ValueError):
                    validate_package(archive, 'x64')
            with zipfile.ZipFile(archive, 'w') as zipped:
                zipped.writestr(DLL_NAME, data)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            checksum = archive.with_suffix('.zip.sha256')
            checksum.write_text(digest + '  wrong.zip', encoding='utf-8')
            with self.assertRaises(ValueError):
                validate_package(archive, 'x64')
            checksum.write_text(digest + '  ' + archive.name, encoding='utf-8')
            original = root / DLL_NAME
            original.write_bytes(data + b'changed')
            with self.assertRaises(ValueError):
                validate_package(archive, 'x64', original)

    def test_validator_cli_single_all_and_invalid_dll_option(self):
        script = Path(__file__).resolve().parents[1] / 'scripts/validate_packages.py'
        version = project_version(script.parent.parent / 'CMakeLists.txt')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.dict('os.environ', {'GITHUB_STEP_SUMMARY': ''}):
                for arch, machine in MACHINES.items():
                    dll = root / DLL_NAME
                    dll.write_bytes(fake_dll(machine))
                    package(dll, root / arch, arch, version)
            command = [sys.executable, str(script), '--packages-dir', str(root)]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('ARM64:', result.stdout)
            result = subprocess.run([sys.executable, str(script), '--packages-dir', str(root / 'x64'), '--arch', 'x64'], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(command + ['--dll', str(root / DLL_NAME)], capture_output=True, text=True, check=False)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
