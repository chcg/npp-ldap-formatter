import copy
from contextlib import contextmanager
import ctypes
import hashlib
import json
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import validate_host_packages as host
from test_package_plugin import fake_dll
from package_plugin import DLL_NAME, MACHINES, package
from validate_packages import validate_package_data


def provenance(arch="x64"):
    identity = {"size": 1024, "sha256": "a" * 64, "machine": arch, "pe_valid": True}
    return {"host": identity.copy(), "dll": identity.copy(), "host_archive_sha256": host.HOST_HASHES[arch],
            "package_sha256": "b" * 64, "build": {"configuration": "Release", "architecture": arch,
            "runtime": "MultiThreaded", "dll_sha256": "a" * 64, "compiler": "19.44.35229.0",
            "source_revision": "c" * 40, "source_digest": "d" * 64}}


class FakeApi:
    def __init__(self):
        self.text = b""
        self.buffer = 7
        self.pointer = 9
        self.can_undo = 0
        self.calls = []
        self.changes = 0
        self.blocked = False
        self.view = 2

    def focused(self, window):
        return 2

    def remaining(self):
        return 1

    def block_input(self, window):
        self.blocked = True

    def view_owned(self, window, scintilla, blocked=True):
        if window != 1 or scintilla != self.view or blocked and not self.blocked:
            raise host.HostError("ownership")

    def send(self, window, message, w=0, l=0):
        self.calls.append((window, message, w, l))
        if message == 2084:
            return self.buffer
        if message == 2357:
            return self.pointer
        if message == 2006:
            return len(self.text)
        if message == 2007:
            return self.text[w]
        if message == 0x102:
            self.text += bytes([w])
            self.changes += 1
        if message == 0x111 and w == 41001:
            self.text = b""
            self.buffer += 1
            self.pointer += 1
        elif message == 0x111:
            self.text = host.PRETTY if w == 100 else host.COMPACT
            self.can_undo = 1
        if message == 2175:
            self.can_undo = 0
        if message == 2174:
            return self.can_undo
        if message == 2176:
            self.text, self.can_undo = host.SOURCE, 0
        return {2030: 2, 2137: 65001}.get(message, 0)


def locked_document(api):
    original = host.Document(api, 1)
    api.block_input(1)
    original.check()
    return original


@contextmanager
def cli_case(arch="x64"):
    with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": ""}):
        root = Path(temporary).resolve()
        dll = root / DLL_NAME
        dll.write_bytes(fake_dll(MACHINES[arch]))
        archive = package(dll, root / "packages", arch, "0.1.0")
        host_zip = root / "host.zip"
        with zipfile.ZipFile(host_zip, "w") as zipped:
            zipped.writestr("notepad++.exe", fake_dll(MACHINES[arch], 2))
            zipped.writestr("langs.model.xml", "<xml/>")
            zipped.writestr("stylers.model.xml", "<xml/>")
        host_hash = hashlib.sha256(host_zip.read_bytes()).hexdigest()
        build = provenance(arch)["build"]
        build["dll_sha256"] = hashlib.sha256(dll.read_bytes()).hexdigest()
        metadata = root / "metadata.json"
        metadata.write_text(json.dumps(build), encoding="utf-8")
        output = root / ".build/host-validation-4-2/result.json"
        args = ["validate_host_packages.py", "--arch", arch, "--package", str(archive), "--host-archive", str(host_zip),
                "--host-sha256", host_hash, "--build-metadata", str(metadata), "--output", str(output)]
        with patch.object(host, "ROOT", root), patch.dict(host.HOST_HASHES, {arch: host_hash}), patch.object(host, "file_version", return_value=host.VERSION), patch.object(host, "native_architecture", return_value="x64"):
            yield root, archive, host_zip, metadata, output, args


def completed(row, failed=None, error="none", process_cleanup="passed"):
    row["execution"] = "attempted"
    for stage in host.STAGES:
        row[stage] = "passed"
    if failed:
        index = host.STAGES.index(failed)
        row[failed] = "failed"
        for stage in host.STAGES[index + 1:]:
            row[stage] = "pending"
    if row["load"] == "passed":
        row["loaded_module"] = str(Path(row["portable_directory"]) / "plugins/NppLdapFormatter" / DLL_NAME)
    if row["menus"] == "passed":
        row["menu_ids"] = [100, 101]
    row["error"], row["process_cleanup"] = error, process_cleanup


class HostPackageTests(unittest.TestCase):
    def test_watchdog_start_failure_still_cleans_owned_process_and_persists_evidence(self):
        with cli_case() as (_, _, _, _, output, args):
            process = Mock(pid=42)
            process.poll.side_effect = [None, 0]
            timer = Mock()
            timer.start.side_effect = RuntimeError("cannot start thread")
            with patch.object(sys, "argv", args), patch.object(host.subprocess, "Popen", return_value=process), patch.object(host.threading, "Timer", return_value=timer), patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(host.main(), 1)
            process.terminate.assert_called_once()
            process.wait.assert_called_once_with(timeout=5)
            timer.cancel.assert_not_called()
            timer.join.assert_not_called()
            row = host.load_json(output)
            host.validate_report(row)
            self.assertEqual((row["error"], row["input_block"], row["process_cleanup"], row["directory_cleanup"]), ("api", "failed", "passed", "passed"))

    def test_watchdog_expiration_terminates_only_owned_handle_and_records_timeout(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        process = Mock(pid=42)
        process.poll.return_value = None
        process.terminate.side_effect = lambda: setattr(process.poll, "return_value", 0)
        timer = Mock()
        def watchdog(seconds, callback):
            self.assertEqual(seconds, 1)
            timer.start.side_effect = callback
            return timer
        api = Mock()
        api.window.side_effect = host.HostError("host-exited")
        with patch.object(host.subprocess, "Popen", return_value=process), patch.object(host.threading, "Timer", side_effect=watchdog):
            host.run_owned(Path(row["portable_directory"]), 1, row, api_type=lambda *args: api)
        process.terminate.assert_called_once()
        timer.cancel.assert_called_once()
        timer.join.assert_called_once()
        self.assertEqual((row["error"], row["input_block"], row["process_cleanup"]), ("timeout", "failed", "passed"))

    def test_watchdog_cancelled_and_joined_on_primary_failure_without_hiding_it(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        process = Mock(pid=42)
        process.poll.side_effect = [None, 0]
        timer = Mock()
        with patch.object(host.subprocess, "Popen", return_value=process), patch.object(host.threading, "Timer", return_value=timer), patch.object(host, "Win32"):
            host.run_owned(Path(row["portable_directory"]), 1, row, api_type=Mock(side_effect=host.HostError("ownership")))
        timer.start.assert_called_once()
        timer.cancel.assert_called_once()
        timer.join.assert_called_once()
        self.assertEqual(row["error"], "ownership")

    def test_schema_rejects_non_string_enums_and_contradictory_outcomes(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        for key in ("architecture", "native_architecture", "execution", "execution_mode", "error", *host.STAGES, "process_cleanup", "directory_cleanup"):
            for malformed in ([], {}):
                with self.subTest(key=key, malformed=malformed), self.assertRaises(ValueError):
                    host.validate_report({**row, key: malformed})
        for changes in ({"load": "failed"}, {"process_cleanup": "passed"}, {"execution": "attempted"}):
            with self.assertRaises(ValueError):
                host.validate_report({**row, **changes})
        completed(row)
        row["directory_cleanup"] = "passed"
        host.validate_report(row)
        for changes in ({"pretty_undo_api": "failed"}, {"error": "oracle"}, {"process_cleanup": "not-needed"}, {"directory_cleanup": "failed"}):
            with self.assertRaises(ValueError):
                host.validate_report({**row, **changes})
        cross = host.new_report("ARM64", "x64", provenance("ARM64"), Path.cwd() / "owned")
        with self.assertRaises(ValueError):
            host.validate_report({**cross, "process_cleanup": "passed"})

    def test_cli_malformed_json_metadata_is_rejected_without_traceback(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "invalid.json"
            row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
            for value in ("{", "[]", json.dumps({**row, "architecture": []}), json.dumps({**row, "error": {}})):
                source.write_text(value, encoding="utf-8")
                with patch.object(sys, "argv", ["validate_host_packages.py", "--validate", str(source)]), patch("sys.stdout", new_callable=io.StringIO) as stdout:
                    self.assertEqual(host.main(), 1)
                    self.assertNotIn("Traceback", stdout.getvalue())

    def test_real_toolhelp_method_rejects_module_provenance_and_checks_close(self):
        with tempfile.TemporaryDirectory() as temporary:
            expected = Path(temporary).resolve() / DLL_NAME
            expected.write_bytes(fake_dll(MACHINES["x64"]))
            for variant in ("valid", "foreign-path", "wrong-pid", "duplicate", "hash", "close", "enumeration"):
                with self.subTest(variant=variant):
                    api = host.Win32.__new__(host.Win32)
                    api.process = Mock(pid=42)
                    api.process.poll.return_value = None
                    api.deadline = float("inf")
                    api.dll_hash = "0" * 64 if variant == "hash" else hashlib.sha256(expected.read_bytes()).hexdigest()
                    api.kernel = Mock()
                    api.kernel.CreateToolhelp32Snapshot.return_value = 987
                    api.kernel.CloseHandle.return_value = variant != "close"
                    index = [0]
                    def emit(snapshot, pointer):
                        if index[0] >= (2 if variant == "duplicate" else 1):
                            ctypes.set_last_error(5 if variant == "enumeration" else 18)
                            return False
                        entry = pointer._obj
                        entry.pid = 43 if variant == "wrong-pid" else 42
                        entry.name = DLL_NAME
                        entry.path = str(expected.parent / "foreign" / DLL_NAME) if variant == "foreign-path" else str(expected)
                        index[0] += 1
                        return True
                    api.kernel.Module32FirstW.side_effect = emit
                    api.kernel.Module32NextW.side_effect = emit
                    if variant == "valid":
                        self.assertEqual(api.module(expected), str(expected))
                    else:
                        with self.assertRaises(host.HostError) as error:
                            api.module(expected)
                        self.assertEqual(error.exception.reason, "api" if variant in ("close", "enumeration") else "module")
                    api.kernel.CreateToolhelp32Snapshot.assert_called_once_with(0x18, 42)
                    api.kernel.CloseHandle.assert_called_once_with(987)

    def test_real_window_input_lock_and_retained_view_identity(self):
        api = host.Win32.__new__(host.Win32)
        api.process = Mock(pid=42)
        api.process.poll.return_value = None
        api.deadline = float("inf")
        api.blocked_window = None
        api.user = Mock()
        def owner(window, pointer):
            pointer._obj.value = 42
            return 8
        api.user.GetWindowThreadProcessId.side_effect = owner
        api.user.GetClassNameW.side_effect = lambda window, buffer, count: setattr(buffer, "value", "Scintilla") or 9
        api.user.IsChild.return_value = True
        enabled = [True]
        api.user.IsWindowEnabled.side_effect = lambda window: enabled[0]
        api.user.EnableWindow.side_effect = lambda window, state: enabled.__setitem__(0, state)
        api.block_input(1)
        self.assertFalse(enabled[0])
        api.user.EnableWindow.assert_called_once_with(1, False)
        data = FakeApi()
        api.send = data.send
        api.focused = Mock(side_effect=AssertionError("focus must not be queried while blocked"))
        document = host.Document(api, 1, 2)
        document.check()
        # Human tab selection is ignored by a disabled window; external changes still abort.
        if enabled[0]:
            data.buffer += 1
        document.send(0x102, 65)
        self.assertEqual(data.changes, 1)
        for failure in ("enabled", "parent", "pid"):
            enabled[0] = failure == "enabled"
            api.user.IsChild.return_value = failure != "parent"
            api.user.GetWindowThreadProcessId.side_effect = owner if failure != "pid" else lambda *args: 0
            with self.assertRaises(host.HostError):
                document.send(0x102, 66)
            self.assertEqual(data.changes, 1)
        api.user.GetWindowThreadProcessId.side_effect = owner
        api.user.EnableWindow.side_effect = lambda *args: None
        enabled[0] = True
        with self.assertRaises(host.HostError):
            api.block_input(1)

    def test_initial_owned_editor_readiness_waits_within_the_deadline(self):
        api = host.Win32.__new__(host.Win32)
        api.owned = Mock(return_value=8)
        api.remaining = Mock(return_value=1)
        api.user = Mock()
        api.user.IsChild.return_value = True
        api.klass = Mock(return_value="Scintilla")
        calls = [0]
        def ready(thread, pointer):
            calls[0] += 1
            pointer._obj.active = 1
            pointer._obj.focus = None if calls[0] == 1 else 2
            return True
        api.user.GetGUIThreadInfo.side_effect = ready
        with patch.object(host.time, "sleep") as sleep:
            self.assertEqual(api.focused(1), 2)
            sleep.assert_called_once_with(0.05)
        api.user.GetGUIThreadInfo.side_effect = lambda thread, pointer: True
        api.remaining.side_effect = host.HostError("timeout")
        with self.assertRaises(host.HostError) as error:
            api.focused(1)
        self.assertEqual(error.exception.reason, "timeout")

    def test_new_requires_a_distinct_document_pointer_even_with_new_buffer_id(self):
        api = FakeApi()
        original = locked_document(api)
        send = api.send
        def reuse_document(window, message, w=0, l=0):
            result = send(window, message, w, l)
            if message == 0x111 and w == 41001:
                api.pointer = original.pointer
            return result
        api.send = reuse_document
        with self.assertRaises(host.HostError):
            host.basic(api, 1, [100, 101], {}, original)
        self.assertEqual(api.changes, 0)

    def test_launch_failure_has_no_process_to_clean_up(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        with patch.object(host.subprocess, "Popen", side_effect=OSError("launch failed")):
            host.run_owned(Path(row["portable_directory"]), 1, row)
        self.assertEqual((row["launch"], row["input_block"], row["error"], row["process_cleanup"]), ("failed", "pending", "launch", "not-needed"))

    def test_failed_undo_is_recorded_before_compact_and_keeps_primary_error(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        process = Mock(pid=42)
        process.poll.return_value = None
        process.terminate.side_effect = OSError("cleanup failed")
        api = FakeApi()
        api.window = lambda: 1
        api.module = lambda expected: str(expected)
        api.menus = lambda window: [100, 101]
        def fail_undo(api, window, ids, result, original):
            result["pretty"] = "passed"
            raise host.HostError("oracle")
        with patch.object(host.subprocess, "Popen", return_value=process), patch.object(host, "basic", side_effect=fail_undo):
            host.run_owned(Path(row["portable_directory"]), 1, row, api_type=lambda *args: api)
        self.assertEqual((row["pretty_undo_api"], row["compact"], row["process_cleanup"], row["error"]), ("failed", "pending", "failed", "oracle"))

    def test_main_launch_and_directory_cleanup_failures_are_persisted(self):
        for primary in (None, "oracle"):
            with self.subTest(primary=primary), cli_case() as (_, _, _, _, output, args):
                def run(isolated, timeout, row):
                    completed(row, "pretty_undo_api" if primary else None, primary or "none")
                with patch.object(sys, "argv", args), patch.object(host, "run_owned", side_effect=run), patch.object(host.shutil, "rmtree", side_effect=OSError("cannot delete")) as remove, patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(host.main(), 1)
                    self.assertEqual(remove.call_count, 1)
                row = host.load_json(output)
                host.validate_report(row)
                self.assertEqual(row["directory_cleanup"], "failed")
                self.assertEqual(row["error"], primary or "cleanup")
                self.assertTrue(Path(row["portable_directory"]).is_dir())
        with cli_case() as (_, _, _, _, output, args), patch.object(sys, "argv", args), patch.object(host.subprocess, "Popen", side_effect=OSError()), patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(host.main(), 1)
            row = host.load_json(output)
            host.validate_report(row)
            self.assertEqual((row["error"], row["process_cleanup"], row["directory_cleanup"]), ("launch", "not-needed", "passed"))

    def test_main_retains_directory_if_owned_process_cleanup_fails(self):
        with cli_case() as (_, _, _, _, output, args):
            def run(isolated, timeout, row):
                completed(row, error="cleanup", process_cleanup="failed")
            with patch.object(sys, "argv", args), patch.object(host, "run_owned", side_effect=run), patch.object(host.shutil, "rmtree") as remove, patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(host.main(), 1)
                remove.assert_not_called()
            row = host.load_json(output)
            host.validate_report(row)
            self.assertEqual(row["directory_cleanup"], "pending")
            self.assertTrue(Path(row["portable_directory"]).is_dir())

    def test_main_hitl_and_cross_only_retain_pending_real_preparation(self):
        for arch, extra in (("x64", ["--prepare-hitl"]), ("ARM64", [])):
            with self.subTest(arch=arch), cli_case(arch) as (_, _, _, _, output, args):
                with patch.object(sys, "argv", args + extra), patch.object(host.subprocess, "Popen") as popen, patch.object(host.ctypes, "sizeof", return_value=4), patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(host.main(), 0)
                    popen.assert_not_called()
                row = host.load_json(output)
                host.validate_report(row)
                self.assertEqual(row["execution"], "prepared")
                self.assertTrue(all(row[stage] == "pending" for stage in host.STAGES))
                self.assertTrue((Path(row["portable_directory"]) / "plugins/NppLdapFormatter" / DLL_NAME).is_file())

    def test_32bit_interpreter_rejects_64bit_target_before_prepare_or_launch(self):
        for arch, native in (("x64", "x64"), ("ARM64", "ARM64")):
            with self.subTest(arch=arch), cli_case(arch) as (_, _, _, _, _, args), patch.object(sys, "argv", args), patch.object(host, "native_architecture", return_value=native), patch.object(host.ctypes, "sizeof", return_value=4), patch.object(host, "prepare") as prepare, patch.object(host.subprocess, "Popen") as popen, patch("sys.stdout", new_callable=io.StringIO) as stdout:
                self.assertEqual(host.main(), 1)
                prepare.assert_not_called()
                popen.assert_not_called()
                self.assertIn("64-bit Python", stdout.getvalue())

    def test_package_extraction_uses_the_same_immutable_snapshot_as_validation(self):
        with cli_case() as (root, archive, host_zip, metadata, _, _):
            original_bytes = archive.read_bytes()
            expected_dll = fake_dll(MACHINES["x64"])
            checksum = archive.with_suffix(".zip.sha256")
            original_read = Path.read_text
            def mutate_after_snapshot(path, *args, **kwargs):
                text = original_read(path, *args, **kwargs)
                if path == checksum:
                    archive.write_bytes(b"mutated-after-read")
                return text
            build = host.load_json(metadata)
            with patch.object(Path, "read_text", mutate_after_snapshot):
                owned, info = host.prepare(archive, host_zip, host.HOST_HASHES["x64"], build, "x64", root / ".build/host-validation-4-2/runs", root)
            self.assertEqual(info["package_sha256"], hashlib.sha256(original_bytes).hexdigest())
            self.assertEqual((owned / "plugins/NppLdapFormatter" / DLL_NAME).read_bytes(), expected_dll)
            self.assertEqual(archive.read_bytes(), b"mutated-after-read")

    def test_shared_package_validator_rejects_bounds_before_decompression(self):
        with cli_case() as (_, archive, _, _, _, _):
            data = archive.read_bytes()
            checksum = archive.with_suffix(".zip.sha256").read_text(encoding="utf-8")
            original_info = zipfile.ZipFile.getinfo
            def oversized(zipped, name):
                info = copy.copy(original_info(zipped, name))
                info.file_size = 8 * 1024**2 + 1
                return info
            with patch.object(zipfile.ZipFile, "getinfo", oversized), patch.object(zipfile.ZipFile, "read") as read:
                with self.assertRaises(ValueError):
                    validate_package_data(data, checksum, archive.name, "x64")
                read.assert_not_called()
            with patch("validate_packages.MAX_ARCHIVE_BYTES", 1), patch.object(zipfile, "ZipFile") as zipped:
                with self.assertRaises(ValueError):
                    validate_package_data(data, checksum, archive.name, "x64")
                zipped.assert_not_called()

    def test_literal_smoke_and_one_step_api_undo_use_only_scalars(self):
        api = FakeApi()
        row = {}
        host.basic(api, 1, [100, 101], row, locked_document(api))
        self.assertEqual(row, {"pretty": "passed", "compact": "passed", "pretty_undo_api": "passed", "compact_undo_api": "passed"})
        self.assertEqual(api.text, host.SOURCE)
        self.assertEqual(api.changes, len(host.SOURCE))
        self.assertNotIn(2182, [call[1] for call in api.calls])  # SCI_GETTEXT
        self.assertNotIn(2181, [call[1] for call in api.calls])  # SCI_SETTEXT
        self.assertTrue(all(type(argument) is int for call in api.calls for argument in call))

    def test_identity_change_aborts_before_read_or_write(self):
        for identity in ("buffer", "pointer"):
            api = FakeApi()
            document = locked_document(api)
            setattr(api, identity, 88)
            api.calls.clear()
            with self.assertRaises(host.HostError) as error:
                document.send(0x102, 65)
            self.assertEqual(error.exception.reason, "ownership")
            self.assertFalse(any(call[1] in (2006, 2007, 0x102) for call in api.calls))
        api = FakeApi()
        document = locked_document(api)
        api.view = 3
        with self.assertRaises(host.HostError):
            document.read()

    def test_new_command_noop_rejects_even_an_empty_existing_buffer(self):
        api = FakeApi()
        send = api.send
        def no_new(window, message, w=0, l=0):
            if message == 0x111 and w == 41001:
                return 0
            return send(window, message, w, l)
        api.send = no_new
        with self.assertRaises(host.HostError) as error:
            host.basic(api, 1, [100, 101], {}, locked_document(api))
        self.assertEqual(error.exception.reason, "ownership")
        self.assertEqual(api.changes, 0)
        self.assertFalse(any(call[1] in (0x102, 2031, 2037, 2160, 2175) for call in api.calls))

    def test_deferred_setup_notification_settles_before_format_dispatch(self):
        api = FakeApi()
        api.selection_pending = False
        send = api.send
        def delayed_selection(window, message, w=0, l=0):
            if message == 2160:
                api.selection_pending = True
            if message == 0x111 and w in (100, 101) and api.selection_pending:
                raise host.HostError("oracle")
            return send(window, message, w, l)
        api.send = delayed_selection
        def idle(seconds):
            self.assertEqual(seconds, 0.1)
            api.selection_pending = False
        with patch.object(host.time, "sleep", side_effect=idle) as sleep:
            result = {}
            host.basic(api, 1, [100, 101], result, locked_document(api))
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual(result["compact_undo_api"], "passed")

    def test_read_rejects_excess_length_non_ascii_and_changed_length(self):
        for data in (b"a" * 257, b"\xff"):
            api = FakeApi()
            api.text = data
            with self.assertRaises(host.HostError):
                locked_document(api).read()
        api = FakeApi()
        document = locked_document(api)
        with patch.object(api, "send", side_effect=lambda _, message, *args: 1 if message == 2006 else 65):
            with self.assertRaises(host.HostError):
                document.read()

    def test_deadline_process_exit_and_pid_ownership(self):
        api = host.Win32.__new__(host.Win32)
        api.process = Mock(pid=42)
        api.process.poll.return_value = None
        api.deadline = 10
        with patch.object(host.time, "monotonic", return_value=11):
            with self.assertRaises(host.HostError) as error:
                api.remaining()
            self.assertEqual(error.exception.reason, "timeout")
        api.process.poll.return_value = 0
        with self.assertRaises(host.HostError) as error:
            api.remaining()
        self.assertEqual(error.exception.reason, "host-exited")
        api.process.poll.return_value = None
        api.deadline = float("inf")
        api.user = Mock()
        api.user.GetWindowThreadProcessId.return_value = 0
        with self.assertRaises(host.HostError) as error:
            api.owned(7)
        self.assertEqual(error.exception.reason, "ownership")

    def test_owned_process_cleanup_success_timeout_and_failure(self):
        for reason in (None, "timeout", "module", "menus", "ownership"):
            row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
            process = Mock(pid=123)
            process.poll.side_effect = [None, 0]
            api = Mock()
            api.window.return_value = 1
            api.module.return_value = str(Path(row["portable_directory"]) / "plugins" / "NppLdapFormatter" / DLL_NAME)
            api.menus.return_value = [100, 101]
            if reason:
                api.window.side_effect = host.HostError(reason)
            with patch.object(host.subprocess, "Popen", return_value=process) as popen, patch.object(host, "basic") as basic:
                host.run_owned(Path(row["portable_directory"]), 1, row, api_type=lambda *args: api)
            process.terminate.assert_called_once()
            process.wait.assert_called_once_with(timeout=5)
            self.assertEqual(row["process_cleanup"], "passed")
            self.assertEqual(row["error"], reason or "none")
            self.assertEqual(basic.call_count, 0 if reason else 1)
            self.assertEqual(popen.call_args.kwargs["cwd"], Path(row["portable_directory"]))
        process = Mock()
        process.poll.return_value = None
        process.terminate.side_effect = OSError()
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        with patch.object(host.subprocess, "Popen", return_value=process):
            host.run_owned(Path(row["portable_directory"]), 1, row, api_type=Mock(side_effect=host.HostError("api")))
        self.assertEqual(row["process_cleanup"], "failed")
        self.assertEqual(row["error"], "api")

    def test_prepare_only_validated_zip_and_approved_files(self):
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": ""}):
            root = Path(temporary)
            dll = root / DLL_NAME
            dll.write_bytes(fake_dll(MACHINES["x64"]))
            archive = package(dll, root / "packages", "x64", "0.1.0")
            host_zip = root / "host.zip"
            with zipfile.ZipFile(host_zip, "w") as zipped:
                zipped.writestr("notepad++.exe", fake_dll(MACHINES["x64"], 2))
                zipped.writestr("langs.model.xml", "<xml/>")
                zipped.writestr("stylers.model.xml", "<xml/>")
                zipped.writestr("../private.txt", "forbidden")
                zipped.writestr("plugins/foreign.dll", b"foreign")
            digest = hashlib.sha256(host_zip.read_bytes()).hexdigest()
            build = provenance()["build"]
            build["dll_sha256"] = hashlib.sha256(dll.read_bytes()).hexdigest()
            runs = root / ".build/host-validation-4-2/runs"
            with patch.dict(host.HOST_HASHES, {"x64": digest}), patch.object(host, "file_version", return_value=host.VERSION):
                owned, metadata = host.prepare(archive, host_zip, digest, build, "x64", runs, root)
                self.assertEqual((owned / "plugins/NppLdapFormatter" / DLL_NAME).read_bytes(), dll.read_bytes())
                self.assertEqual((owned / "config.xml").read_text(encoding="utf-8", errors="strict"), host.CONFIG)
                self.assertEqual(list((owned / "plugins").iterdir()), [owned / "plugins/NppLdapFormatter"])
                self.assertFalse((root / "private.txt").exists())
                self.assertEqual(metadata["package_sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
                for invalid in ("0" * 64, "not-a-hash"):
                    with self.assertRaises(ValueError):
                        host.prepare(archive, host_zip, invalid, build, "x64", runs, root)
                for change in ({"runtime": "MultiThreadedDLL"}, {"architecture": "x86"}, {"dll_sha256": "0" * 64}):
                    with self.assertRaises(ValueError):
                        host.prepare(archive, host_zip, digest, {**build, **change}, "x64", runs, root)
                with patch.object(host, "file_version", return_value="0.0.0.0"):
                    with self.assertRaises(ValueError):
                        host.prepare(archive, host_zip, digest, build, "x64", runs, root)
                self.assertEqual([path.resolve() for path in runs.iterdir()], [owned])
                outside = root / "outside"
                outside.mkdir()
                marker = outside / "untouched.txt"
                marker.write_bytes(b"owned-by-someone-else")
                with patch.object(host.tempfile, "mkdtemp", return_value=str(outside)), patch.object(host.shutil, "rmtree") as remove:
                    with self.assertRaises(ValueError):
                        host.prepare(archive, host_zip, digest, build, "x64", runs, root)
                    remove.assert_not_called()
                self.assertEqual(marker.read_bytes(), b"owned-by-someone-else")

    def test_rejects_compiler_outside_the_1944_baseline(self):
        for compiler in ("19.43.35229.0", "18.0.0", "19.440.1", "private"):
            with self.assertRaises(ValueError):
                host.validate_build({**provenance()["build"], "compiler": compiler}, "x64", "a" * 64)

    def test_link_escape_rejected_before_writes_or_recursive_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary).resolve()
            root, outside = base / "workspace", base / "outside"
            root.mkdir()
            outside.mkdir()
            (root / ".build").mkdir()
            allowed = root / ".build/host-validation-4-2"
            # Model a junction resolution deterministically; creating actual links may require elevation.
            original = Path.resolve
            def linked(path, *args, **kwargs):
                if path == allowed:
                    return outside
                if path.is_relative_to(allowed):
                    return outside / path.relative_to(allowed)
                return original(path, *args, **kwargs)
            with patch.object(Path, "resolve", linked), patch.object(host.shutil, "rmtree") as remove:
                with self.assertRaises(ValueError):
                    host.safe_output(allowed / "report.json", (), root)
                with self.assertRaises(ValueError):
                    host.prepare(base / "missing.zip", base / "host.zip", "a" * 64, {}, "x64", allowed / "runs", root)
                remove.assert_not_called()

    def test_schema_rejects_content_fields_promotion_and_inconsistent_results(self):
        row = host.new_report("x64", "x64", provenance(), Path.cwd() / "owned")
        host.validate_report(row)
        for change in ({"text": "secret"}, {"human_ctrl_z": "passed"}, {"release_blocked": False}, {"menus": "passed"}, {"compact": "passed"}, {"schema": True}, {"error": "private diagnostic"}):
            with self.assertRaises(ValueError):
                host.validate_report({**row, **change})
        value = copy.deepcopy(row)
        value["provenance"]["dll"]["document_hash"] = "a" * 64
        with self.assertRaises(ValueError):
            host.validate_report(value)
        value = host.new_report("ARM64", "x64", provenance("ARM64"), Path.cwd() / "owned")
        self.assertEqual(value["execution_mode"], "cross-only")
        host.validate_report(value)
        with self.assertRaises(ValueError):
            host.validate_report({**value, "load": "passed"})
        self.assertEqual(host.execution_mode("x64", "x86"), "WOW64")

    def test_output_rejects_protected_paths_aliases_and_invalid_previous_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            allowed = root / ".build/host-validation-4-2"
            allowed.mkdir(parents=True)
            source = allowed / "source.json"
            source.write_text("{}", encoding="utf-8")
            for output in (root / "src/plugin.cpp", root / "AGENTS.md", allowed / "file.txt", source):
                with self.assertRaises(ValueError):
                    host.safe_output(output, (source,), root)
            output = allowed / "report.json"
            self.assertEqual(host.safe_output(output, (source,), root), output.resolve())
            output.write_text('{"text":"private"}', encoding="utf-8")
            with self.assertRaises(ValueError):
                host.safe_output(output, (source,), root)
            output.unlink()
            try:
                os.link(source, output)
            except OSError:
                return
            with self.assertRaises(ValueError):
                host.safe_output(output, (source,), root)


if __name__ == "__main__":
    unittest.main()
