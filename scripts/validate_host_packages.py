"""Validate packaged plugins in owned portable Notepad++ processes, without document IPC buffers."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as W
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

from benchmark_ui import CONFIG, exact_keys, file_version, integer, load_json
from host_evidence import inspect_pe
from package_plugin import DLL_NAME, MACHINES, project_version, validate_dll
from validate_packages import MAX_ARCHIVE_BYTES, validate_package_data

ROOT = Path(__file__).resolve().parents[1]
PLUGIN_VERSION = project_version(ROOT / "CMakeLists.txt")
VERSION = "8.9.8.1"
HOST_HASHES = {
    "x86": "65d3435b5dcbefde47c401a2666132138e76a2b62e189ad4ffe568581b6adfd2",
    "x64": "beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414",
    "ARM64": "43bf78c7569fd43b03d4f86a1073ac4306b31099e4487bdbf536c42d16d03f48",
}
LABELS = ("Format LDAP (Pretty)", "Format LDAP (Compact)")
SOURCE = b"( & (cn=a) (sn=b) )"
PRETTY = b"(&\n  (cn=a)\n  (sn=b)\n)"
COMPACT = b"(&(cn=a)(sn=b))"
ERRORS = {"none", "launch", "timeout", "host-exited", "api", "ownership", "module", "menus", "oracle", "cleanup"}
STATES = {"pending", "passed", "failed"}
STAGES = ("launch", "input_block", "load", "menus", "pretty", "pretty_undo_api", "compact", "compact_undo_api")


class HostError(Exception):
    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(reason)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def hash_value(value: object) -> None:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise ValueError("Invalid binary hash")


def execution_mode(native: str, arch: str) -> str:
    if native == arch:
        return "native"
    if native == "x64" and arch == "x86":
        return "WOW64"
    return "cross-only"


def native_architecture() -> str:
    # Same GetNativeSystemInfo layout as benchmark_ui; permit native ARM64 transfer.
    class Info(ctypes.Structure):
        _fields_ = [("architecture", W.WORD), ("reserved", W.WORD), ("page", W.DWORD),
                    ("minimum", W.LPVOID), ("maximum", W.LPVOID), ("mask", ctypes.c_size_t),
                    ("processors", W.DWORD), ("type", W.DWORD), ("granularity", W.DWORD),
                    ("level", W.WORD), ("revision", W.WORD)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetNativeSystemInfo.argtypes = (ctypes.POINTER(Info),)
    kernel.GetNativeSystemInfo.restype = None
    info = Info()
    kernel.GetNativeSystemInfo(ctypes.byref(info))
    try:
        return {0: "x86", 9: "x64", 12: "ARM64"}[info.architecture]
    except KeyError:
        raise ValueError("Unsupported native architecture") from None


def validate_build(build: dict, arch: str, dll_hash: str) -> None:
    exact_keys(build, {"configuration", "architecture", "runtime", "dll_sha256", "compiler", "source_revision", "source_digest"})
    if build["configuration"] != "Release" or build["architecture"] != arch or build["runtime"] != "MultiThreaded" or build["dll_sha256"] != dll_hash:
        raise ValueError("Build must describe the exact Release /MT package")
    if not isinstance(build["compiler"], str) or re.fullmatch(r"19\.44\.[0-9]+(?:\.[0-9]+)?", build["compiler"]) is None:
        raise ValueError("Invalid compiler version")
    if not isinstance(build["source_revision"], str) or re.fullmatch(r"[0-9a-f]{40}", build["source_revision"]) is None:
        raise ValueError("Invalid source revision")
    hash_value(build["source_digest"])


def safe_output(output: Path, inputs: tuple[Path, ...], root: Path = ROOT) -> Path:
    resolved = output.resolve()
    allowed = (root / ".build" / "host-validation-4-2").resolve()
    if not allowed.is_relative_to(root.resolve()) or not resolved.is_relative_to(allowed) or resolved.suffix != ".json":
        raise ValueError("Evidence must be JSON under .build/host-validation-4-2")
    if any(resolved == path.resolve() or (output.exists() and path.exists() and output.samefile(path)) for path in inputs):
        raise ValueError("Evidence aliases an input")
    if output.exists():
        validate_report(load_json(output))
    return resolved


def prepare(package: Path, host_archive: Path, host_hash: str, build: dict, arch: str, runs: Path, root: Path = ROOT) -> tuple[Path, dict]:
    """Extract only three approved host files and the validated package DLL into a new tree."""
    workspace = root.resolve()
    allowed = (root / ".build" / "host-validation-4-2").resolve()
    if not allowed.is_relative_to(workspace) or runs.resolve() != (allowed / "runs"):
        raise ValueError("Runs directory escapes the owned workspace subtree")
    hash_value(host_hash)
    if package.name != f"NppLdapFormatter-{PLUGIN_VERSION}-{arch}.zip":
        raise ValueError("Incorrect package version/name")
    if host_hash != HOST_HASHES[arch]:
        raise ValueError("Host archive is not the pinned official release")
    with package.open("rb") as stream:
        package_bytes = stream.read(MAX_ARCHIVE_BYTES + 1)
    package_hash, dll = validate_package_data(package_bytes, package.with_suffix(".zip.sha256").read_text(encoding="utf-8", errors="strict"), package.name, arch)
    with host_archive.open("rb") as stream:
        archive_bytes = stream.read(100 * 1024**2 + 1)
    if not 1 <= len(archive_bytes) <= 100 * 1024**2:
        raise ValueError("Host archive size outside protocol bounds")
    if digest(archive_bytes) != host_hash:
        raise ValueError("Host archive hash mismatch")
    validate_build(build, arch, digest(dll))
    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
        names = ("notepad++.exe", "langs.model.xml", "stylers.model.xml")
        if any(archive.namelist().count(name) != 1 for name in names):
            raise ValueError("Host must contain unique root executable/models")
        if any(not 1 <= archive.getinfo(name).file_size <= (40 if name.endswith(".exe") else 2) * 1024**2 for name in names):
            raise ValueError("Invalid host member size")
        # No archive paths are used as destinations, so traversal and foreign plugins are excluded.
        files = {name: archive.read(name) for name in names}
    runs.mkdir(parents=True, exist_ok=True)
    isolated = Path(tempfile.mkdtemp(prefix=f"package-{arch}-", dir=runs)).resolve()
    try:
        if isolated.parent != runs.resolve() or not isolated.is_relative_to(workspace):
            raise ValueError("Portable directory escapes its owned parent")
        for name, data in files.items():
            (isolated / name).write_bytes(data)
        executable = isolated / "notepad++.exe"
        host = inspect_pe(executable)
        if host["machine"] != arch or file_version(executable) != VERSION:
            raise ValueError("Incorrect host architecture/version")
        # Validate the complete PE structure, reusing the DLL checker with only its DLL bit set.
        pe = bytearray(files["notepad++.exe"])
        import struct
        offset = struct.unpack_from("<I", pe, 60)[0]
        flags = struct.unpack_from("<H", pe, offset + 22)[0]
        if flags & 0x2000 or not flags & 0x0002:
            raise ValueError("Host is not an executable")
        struct.pack_into("<H", pe, offset + 22, flags | 0x2000)
        validate_dll(bytes(pe), arch)
        plugin = isolated / "plugins" / "NppLdapFormatter" / DLL_NAME
        plugin.parent.mkdir(parents=True)
        plugin.write_bytes(dll)
        if plugin.read_bytes() != dll:
            raise ValueError("Copied package mismatch")
        for name in ("doLocalConf.xml", "disableNppAutoUpdate.xml"):
            (isolated / name).write_bytes(b"")
        (isolated / "config.xml").write_text(CONFIG, encoding="utf-8")
        if (isolated / "config.xml").read_text(encoding="utf-8", errors="strict") != CONFIG:
            raise ValueError("Invalid UTF-8 configuration")
        return isolated, {"package_sha256": package_hash, "host_archive_sha256": host_hash,
                          "host": host, "dll": inspect_pe(plugin), "build": build}
    except BaseException:
        if isolated.resolve() != isolated or isolated.parent != runs.resolve() or not isolated.is_relative_to(workspace):
            raise ValueError("Unsafe portable cleanup target") from None
        shutil.rmtree(isolated)
        raise


def command(isolated: Path) -> list[str]:
    return [str(isolated / "notepad++.exe"), "-multiInst", "-nosession", f"-settingsDir={isolated}"]


class Win32:
    """Pointer-sized Win32 declarations. Messages to Scintilla contain scalars only."""
    def __init__(self, process, timeout: float):
        self.process = process
        self.deadline = time.monotonic() + timeout
        self.blocked_window = None
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.callback = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, ctypes.c_ssize_t)
        declarations = {
            "EnumWindows": ((self.callback, ctypes.c_ssize_t), W.BOOL),
            "GetWindowThreadProcessId": ((W.HWND, ctypes.POINTER(W.DWORD)), W.DWORD),
            "GetClassNameW": ((W.HWND, W.LPWSTR, ctypes.c_int), ctypes.c_int),
            "GetGUIThreadInfo": ((W.DWORD, W.LPVOID), W.BOOL),
            "IsChild": ((W.HWND, W.HWND), W.BOOL),
            "EnableWindow": ((W.HWND, W.BOOL), W.BOOL),
            "IsWindowEnabled": ((W.HWND,), W.BOOL),
            "SendMessageTimeoutW": ((W.HWND, W.UINT, ctypes.c_size_t, ctypes.c_ssize_t, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_size_t)), ctypes.c_ssize_t),
            "GetMenu": ((W.HWND,), W.HMENU),
            "GetMenuItemCount": ((W.HMENU,), ctypes.c_int),
            "GetMenuItemInfoW": ((W.HMENU, W.UINT, W.BOOL, W.LPVOID), W.BOOL),
        }
        for name, (arguments, result) in declarations.items():
            fn = getattr(self.user, name)
            fn.argtypes, fn.restype = arguments, result
        for name, arguments, result in (
            ("CreateToolhelp32Snapshot", (W.DWORD, W.DWORD), W.HANDLE),
            ("Module32FirstW", (W.HANDLE, W.LPVOID), W.BOOL),
            ("Module32NextW", (W.HANDLE, W.LPVOID), W.BOOL),
            ("CloseHandle", (W.HANDLE,), W.BOOL),
        ):
            fn = getattr(self.kernel, name)
            fn.argtypes, fn.restype = arguments, result

    def remaining(self) -> float:
        if self.process.poll() is not None:
            raise HostError("host-exited")
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise HostError("timeout")
        return remaining

    def owned(self, window: int) -> int:
        self.remaining()
        pid = W.DWORD()
        thread = self.user.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if not thread or pid.value != self.process.pid:
            raise HostError("ownership")
        return thread

    def klass(self, window: int) -> str:
        self.owned(window)
        value = ctypes.create_unicode_buffer(256)
        if not self.user.GetClassNameW(window, value, len(value)):
            raise HostError("api")
        return value.value

    def send(self, window: int, message: int, w: int = 0, l: int = 0) -> int:
        self.owned(window)
        if self.blocked_window is not None:
            self.owned(self.blocked_window)
            if self.user.IsWindowEnabled(self.blocked_window) or (window != self.blocked_window and not self.user.IsChild(self.blocked_window, window)):
                raise HostError("ownership")
        result = ctypes.c_size_t()
        milliseconds = max(1, min(1000, math.ceil(self.remaining() * 1000)))
        if not self.user.SendMessageTimeoutW(window, message, w, l, 0x23, milliseconds, ctypes.byref(result)):
            raise HostError("timeout" if ctypes.get_last_error() in (0, 1460) else "api")
        return ctypes.c_ssize_t(result.value).value

    def window(self) -> int:
        while True:
            self.remaining()
            found = []
            @self.callback
            def visit(window, _):
                pid = W.DWORD()
                self.user.GetWindowThreadProcessId(window, ctypes.byref(pid))
                if pid.value == self.process.pid:
                    value = ctypes.create_unicode_buffer(256)
                    if self.user.GetClassNameW(window, value, 256) and value.value == "Notepad++":
                        found.append(window)
                return True
            if not self.user.EnumWindows(visit, 0):
                raise HostError("api")
            if len(found) == 1:
                return found[0]
            if len(found) > 1:
                raise HostError("ownership")
            time.sleep(min(0.05, self.remaining()))

    def focused(self, window: int) -> int:
        class Gui(ctypes.Structure):
            _fields_ = [("size", W.DWORD), ("flags", W.DWORD)] + [(name, W.HWND) for name in ("active", "focus", "capture", "menu", "move", "caret")] + [("rect", W.RECT)]
        while True:
            info = Gui()
            info.size = ctypes.sizeof(info)
            thread = self.owned(window)
            if not self.user.GetGUIThreadInfo(thread, ctypes.byref(info)):
                raise HostError("api")
            if info.active == window and info.focus and self.user.IsChild(window, info.focus) and self.klass(info.focus) == "Scintilla":
                return info.focus
            # Main HWND can exist before startup has created/focused its owned editor.
            time.sleep(min(0.05, self.remaining()))

    def block_input(self, window: int) -> None:
        self.owned(window)
        if not self.user.IsWindowEnabled(window):
            raise HostError("ownership")
        self.user.EnableWindow(window, False)
        self.owned(window)
        if self.user.IsWindowEnabled(window):
            raise HostError("ownership")
        self.blocked_window = window

    def view_owned(self, window: int, scintilla: int, blocked: bool = True) -> None:
        self.owned(window)
        self.owned(scintilla)
        if not self.user.IsChild(window, scintilla) or self.klass(scintilla) != "Scintilla":
            raise HostError("ownership")
        if blocked and (self.blocked_window != window or self.user.IsWindowEnabled(window)):
            raise HostError("ownership")

    def module(self, expected: Path) -> str:
        class Module(ctypes.Structure):
            _fields_ = [("size", W.DWORD), ("id", W.DWORD), ("pid", W.DWORD), ("global_usage", W.DWORD), ("usage", W.DWORD),
                        ("base", W.LPVOID), ("base_size", W.DWORD), ("handle", W.HMODULE), ("name", W.WCHAR * 256), ("path", W.WCHAR * 260)]
        while True:
            self.remaining()
            snapshot = self.kernel.CreateToolhelp32Snapshot(0x08 | 0x10, self.process.pid)
            if snapshot == ctypes.c_void_p(-1).value:
                if ctypes.get_last_error() == 24:  # ERROR_BAD_LENGTH: loader changed during snapshot.
                    continue
                raise HostError("api")
            found = None
            try:
                entry = Module()
                entry.size = ctypes.sizeof(entry)
                ok = self.kernel.Module32FirstW(snapshot, ctypes.byref(entry))
                if not ok and ctypes.get_last_error() != 18:
                    raise HostError("api")
                while ok:
                    if entry.name.casefold() == DLL_NAME.casefold():
                        if found is not None or entry.pid != self.process.pid or Path(entry.path).resolve() != expected.resolve():
                            raise HostError("module")
                        found = str(expected.resolve())
                    ok = self.kernel.Module32NextW(snapshot, ctypes.byref(entry))
                if ctypes.get_last_error() != 18:
                    raise HostError("api")
            finally:
                if not self.kernel.CloseHandle(snapshot):
                    raise HostError("api")
            if found:
                if inspect_pe(expected)["sha256"] != self.dll_hash:
                    raise HostError("module")
                return found
            time.sleep(min(0.05, self.remaining()))

    def menus(self, window: int) -> list[int]:
        class Item(ctypes.Structure):
            _fields_ = [("size", W.UINT), ("mask", W.UINT), ("type", W.UINT), ("state", W.UINT), ("id", W.UINT),
                        ("submenu", W.HMENU), ("checked", W.HBITMAP), ("unchecked", W.HBITMAP), ("data", ctypes.c_size_t),
                        ("text", W.LPWSTR), ("length", W.UINT), ("bitmap", W.HBITMAP)]
        self.owned(window)
        root = self.user.GetMenu(window)
        if not root:
            raise HostError("menus")
        found = {}
        seen = set()
        def walk(menu, depth=0):
            self.remaining()
            if depth > 16 or menu in seen:
                raise HostError("menus")
            seen.add(menu)
            count = self.user.GetMenuItemCount(menu)
            if not 0 <= count <= 512:
                raise HostError("menus")
            for index in range(count):
                self.remaining()
                text = ctypes.create_unicode_buffer(256)
                item = Item()
                item.size, item.mask, item.text, item.length = ctypes.sizeof(item), 0x47, ctypes.cast(text, W.LPWSTR), 256
                if not self.user.GetMenuItemInfoW(menu, index, True, ctypes.byref(item)) or item.length >= 255:
                    raise HostError("menus")
                label = text.value
                if label in LABELS:
                    if label in found or not 1 <= item.id <= 65535 or item.state & 3:
                        raise HostError("menus")
                    found[label] = item.id
                if item.submenu:
                    walk(item.submenu, depth + 1)
        walk(root)
        if set(found) != set(LABELS) or len(set(found.values())) != 2:
            raise HostError("menus")
        return [found[label] for label in LABELS]


class Document:
    def __init__(self, api: Win32, window: int, scintilla: int | None = None):
        self.api, self.window = api, window
        self.scintilla = api.focused(window) if scintilla is None else scintilla
        api.view_owned(window, self.scintilla, blocked=scintilla is not None)
        self.buffer = api.send(window, 1024 + 1000 + 60)
        self.pointer = api.send(self.scintilla, 2357)  # SCI_GETDOCPOINTER: opaque identity only.
        if not self.buffer or not self.pointer:
            raise HostError("ownership")

    def check(self) -> None:
        self.api.view_owned(self.window, self.scintilla)
        if self.api.send(self.window, 2084) != self.buffer or self.api.send(self.scintilla, 2357) != self.pointer:
            raise HostError("ownership")

    def send(self, message: int, w: int = 0, l: int = 0) -> int:
        self.check()
        return self.api.send(self.scintilla, message, w, l)

    def read(self) -> bytes:
        size = self.send(2006)  # SCI_GETLENGTH
        if not 0 <= size <= 256:
            raise HostError("oracle")
        data = bytearray()
        for index in range(size):
            value = self.send(2007, index)  # SCI_GETCHARAT; never pass a local buffer cross-process.
            if not 0 <= value <= 127:
                raise HostError("oracle")
            data.append(value)
        if self.send(2006) != size:
            raise HostError("ownership")
        return bytes(data)

    def wait(self, expected: bytes) -> None:
        while self.read() != expected:
            time.sleep(min(0.02, self.api.remaining()))


def basic(api: Win32, window: int, ids: list[int], result: dict, original: Document) -> None:
    original.check()
    api.send(window, 0x111, 41001)  # IDM_FILE_NEW; no existing tab is used.
    document = Document(api, window, original.scintilla)
    if document.buffer == original.buffer or document.pointer == original.pointer or document.read() != b"":
        raise HostError("ownership")
    document.send(2031, 2)  # SCI_SETEOLMODE LF for literal oracles.
    document.send(2037, 65001)  # SCI_SETCODEPAGE UTF-8
    if document.send(2030) != 2 or document.send(2137) != 65001:
        raise HostError("api")
    for character in SOURCE:
        document.send(0x102, character)  # WM_CHAR; synthetic ASCII only, no clipboard.
    if document.read() != SOURCE:
        raise HostError("oracle")
    for name, identifier, expected in zip(("pretty", "compact"), ids, (PRETTY, COMPACT)):
        document.send(2160, 0, 0)  # SCI_SETSEL: no selection -> whole document.
        document.send(2175)  # SCI_EMPTYUNDOBUFFER: single command must undo in one step.
        # Let Scintilla emit deferred setup/selection notifications before starting the worker.
        time.sleep(min(0.1, api.remaining()))
        document.check()
        api.send(window, 0x111, identifier)
        document.wait(expected)
        result[name] = "passed"
        if document.send(2174) != 1:  # SCI_CANUNDO
            raise HostError("oracle")
        document.send(2176)  # SCI_UNDO; this is never human Ctrl+Z evidence.
        document.wait(SOURCE)
        while document.send(2008) != 0 or document.send(2009) != 0:
            time.sleep(min(0.02, api.remaining()))
        if document.send(2174) != 0:
            raise HostError("oracle")
        result[f"{name}_undo_api"] = "passed"


def run_owned(isolated: Path, timeout: float, row: dict, api_type=Win32) -> None:
    row["execution"] = "attempted"
    process = None
    watchdog = None
    watchdog_started = False
    expired = threading.Event()
    def expire():
        expired.set()
        try:
            if process.poll() is None:
                process.terminate()  # Exact retained Popen handle; never a discovered PID.
        except OSError:
            pass  # The ordinary cleanup path records a failure to terminate.
    try:
        process = subprocess.Popen(command(isolated), cwd=isolated, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        row["launch"] = "passed"
        # EnableWindow sends synchronous messages; process termination also bounds an unresponsive host.
        watchdog = threading.Timer(timeout, expire)
        watchdog.daemon = True
        watchdog.start()
        watchdog_started = True
        api = api_type(process, timeout)
        api.dll_hash = row["provenance"]["dll"]["sha256"]
        window = api.window()
        original = Document(api, window)
        api.block_input(window)
        original.check()
        row["input_block"] = "passed"
        row["loaded_module"] = api.module(isolated / "plugins" / "NppLdapFormatter" / DLL_NAME)
        row["load"] = "passed"
        row["menu_ids"] = api.menus(window)
        row["menus"] = "passed"
        basic(api, window, row["menu_ids"], row, original)
    except HostError as error:
        row["error"] = "timeout" if expired.is_set() else error.reason
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ctypes.ArgumentError):
        row["error"] = "timeout" if expired.is_set() else "launch" if process is None else "api"
    finally:
        if watchdog_started:
            watchdog.cancel()
            watchdog.join()
        if expired.is_set() and row["error"] == "none":
            row["error"] = "timeout"
        if row["error"] != "none":
            row[next((key for key in STAGES if row[key] == "pending"), STAGES[-1])] = "failed"
        # Only the exact Popen handle created here is eligible for termination.
        if process is None:
            row["process_cleanup"] = "not-needed"
        else:
            try:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
                if process.poll() is None:
                    raise HostError("cleanup")
                row["process_cleanup"] = "passed"
            except (OSError, subprocess.SubprocessError, HostError):
                row["process_cleanup"] = "failed"
                if row["error"] == "none":
                    row["error"] = "cleanup"


def new_report(arch: str, native: str, provenance: dict, isolated: Path) -> dict:
    return {"schema": 2, "architecture": arch, "native_architecture": native, "execution": "prepared",
            "execution_mode": execution_mode(native, arch), "host_version": VERSION, "plugin_version": PLUGIN_VERSION,
            "provenance": provenance, "portable_directory": str(isolated), "loaded_module": "",
            "menu_ids": [], **{key: "pending" for key in (*STAGES, "human_ctrl_z", "process_cleanup", "directory_cleanup")},
            "error": "none", "status": "in-progress", "other_host_gates": "pending", "release_blocked": True}


def validate_report(row: dict) -> None:
    exact_keys(row, {"schema", "architecture", "native_architecture", "execution_mode", "execution", "host_version", "plugin_version", "provenance", "portable_directory", "loaded_module", "menu_ids", *STAGES, "human_ctrl_z", "process_cleanup", "directory_cleanup", "error", "status", "other_host_gates", "release_blocked"})
    for key, choices in (("architecture", MACHINES), ("native_architecture", MACHINES), ("execution_mode", {"native", "WOW64", "cross-only"}), ("execution", {"prepared", "attempted"}), ("error", ERRORS), ("process_cleanup", STATES | {"not-needed"}), ("directory_cleanup", STATES)):
        if not isinstance(row[key], str) or row[key] not in choices:
            raise ValueError("Invalid metadata enum")
    if type(row["schema"]) is not int or row["schema"] != 2 or row["execution_mode"] != execution_mode(row["native_architecture"], row["architecture"]) or row["host_version"] != VERSION or row["plugin_version"] != PLUGIN_VERSION:
        raise ValueError("Invalid architecture/version metadata")
    if row["status"] != "in-progress" or row["other_host_gates"] != "pending" or row["release_blocked"] is not True or row["human_ctrl_z"] != "pending" or row["error"] not in ERRORS:
        raise ValueError("No human acceptance or release promotion is permitted")
    for key in STAGES:
        if not isinstance(row[key], str) or row[key] not in STATES:
            raise ValueError("Invalid result enum")
    provenance = row["provenance"]
    exact_keys(provenance, {"package_sha256", "host_archive_sha256", "host", "dll", "build"})
    for key in ("package_sha256", "host_archive_sha256"):
        hash_value(provenance[key])
    for key in ("host", "dll"):
        identity = provenance[key]
        exact_keys(identity, {"size", "sha256", "machine", "pe_valid"})
        integer(identity["size"], 64)
        hash_value(identity["sha256"])
        if identity["machine"] != row["architecture"] or identity["pe_valid"] is not True:
            raise ValueError("Incorrect binary identity")
    validate_build(provenance["build"], row["architecture"], provenance["dll"]["sha256"])
    if not isinstance(row["portable_directory"], str) or not Path(row["portable_directory"]).is_absolute():
        raise ValueError("Invalid owned portable path")
    expected = Path(row["portable_directory"]) / "plugins" / "NppLdapFormatter" / DLL_NAME
    if row["loaded_module"] != (str(expected) if row["load"] == "passed" else ""):
        raise ValueError("Incorrect loaded module path")
    ids = row["menu_ids"]
    if not isinstance(ids, list) or len(ids) not in (0, 2):
        raise ValueError("Invalid menu IDs")
    for identifier in ids:
        integer(identifier, 1, 65535)
    if len(ids) == 2 and ids[0] == ids[1] or (row["menus"] == "passed") != (len(ids) == 2):
        raise ValueError("Incorrect menu identity")
    if row["execution_mode"] == "cross-only" and row["execution"] != "prepared":
        raise ValueError("Cross compilation cannot certify native execution")
    if provenance["host_archive_sha256"] != HOST_HASHES[row["architecture"]]:
        raise ValueError("Host archive is not pinned")
    stages = [row[key] for key in STAGES]
    if row["execution"] == "prepared":
        if stages != ["pending"] * len(STAGES) or row["error"] != "none" or row["process_cleanup"] != "pending" or row["directory_cleanup"] != "pending":
            raise ValueError("Preparation cannot claim execution or cleanup")
    else:
        failed = stages.index("failed") if "failed" in stages else len(STAGES)
        if stages != ["passed"] * failed + (["failed"] + ["pending"] * (len(STAGES) - failed - 1) if failed < len(STAGES) else []):
            raise ValueError("Execution stages must form an ordered prefix")
        if (failed < len(STAGES)) != (row["error"] not in {"none", "cleanup"}):
            raise ValueError("Execution failure lacks a primary error")
        if row["launch"] == "failed":
            if row["error"] != "launch" or row["process_cleanup"] != "not-needed":
                raise ValueError("Failed launch cannot claim a process")
        elif row["process_cleanup"] not in {"passed", "failed"} or row["error"] == "launch":
            raise ValueError("Launched process must have a cleanup outcome")
        if row["process_cleanup"] == "failed":
            if row["directory_cleanup"] != "pending":
                raise ValueError("Retain directory until owned process exits")
        elif row["directory_cleanup"] not in {"passed", "failed"}:
            raise ValueError("Finished attempt needs directory cleanup outcome")
        cleanup_failed = "failed" in (row["process_cleanup"], row["directory_cleanup"])
        if row["error"] == "none" and cleanup_failed or row["error"] == "cleanup" and not cleanup_failed:
            raise ValueError("Cleanup result contradicts the error")


def write_report(output: Path, row: dict, inputs: tuple[Path, ...]) -> None:
    validate_report(row)
    safe_output(output, inputs, ROOT)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(row, stream, indent=2, ensure_ascii=True, allow_nan=False)
            stream.write("\n")
        validate_report(load_json(temporary))
        safe_output(output, inputs, ROOT)
        temporary.replace(output)
        validate_report(load_json(output))
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=MACHINES)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--host-archive", type=Path)
    parser.add_argument("--host-sha256")
    parser.add_argument("--build-metadata", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--prepare-hitl", action="store_true")
    parser.add_argument("--validate", type=Path)
    args = parser.parse_args()
    runs = ROOT / ".build" / "host-validation-4-2" / "runs"
    try:
        if args.validate:
            validate_report(load_json(args.validate))
            print("Evidence valid; human Ctrl+Z pending; release blocked")
            return 0
        if not all((args.arch, args.package, args.host_archive, args.host_sha256, args.build_metadata, args.output)):
            parser.error("--arch, --package, --host-archive, --host-sha256, --build-metadata and --output required")
        if sys.platform != "win32" or not math.isfinite(args.timeout) or not 1 <= args.timeout <= 120:
            raise ValueError("Windows and a finite 1..120 second deadline required")
        inputs = (args.package, args.host_archive, args.build_metadata)
        safe_output(args.output, inputs, ROOT)
        native = native_architecture()
        if not args.prepare_hitl and execution_mode(native, args.arch) != "cross-only" and args.arch != "x86" and ctypes.sizeof(ctypes.c_void_p) < 8:
            print("Use 64-bit Python for x64/ARM64 module enumeration; --prepare-hitl remains available")
            return 1
        isolated, provenance = prepare(args.package, args.host_archive, args.host_sha256, load_json(args.build_metadata), args.arch,
                                       runs, ROOT)
        row = new_report(args.arch, native, provenance, isolated)
        if args.prepare_hitl or row["execution_mode"] == "cross-only":
            write_report(args.output, row, inputs)
            print("Prepared owned portable directory; no human result recorded")
            print(subprocess.list2cmdline(command(isolated)))
            return 0
        run_owned(isolated, args.timeout, row)
        if row["process_cleanup"] in {"passed", "not-needed"}:
            try:
                if isolated.resolve() != isolated or isolated.parent != runs.resolve() or not isolated.is_relative_to(ROOT.resolve()):
                    raise ValueError("Unsafe portable cleanup target")
                shutil.rmtree(isolated)
                if isolated.exists():
                    raise OSError("Owned portable directory remains")
                row["directory_cleanup"] = "passed"
            except (OSError, ValueError):
                # Preserve the primary failure and directory; never retry a failed recursive delete.
                row["directory_cleanup"] = "failed"
                if row["error"] == "none":
                    row["error"] = "cleanup"
        write_report(args.output, row, inputs)
        print(f"Package smoke: {row['error']}; human Ctrl+Z pending; release blocked")
        return 0 if row["error"] == "none" else 1
    except (ValueError, OSError, UnicodeError, zipfile.BadZipFile, subprocess.SubprocessError):
        print("Package validation rejected; no certification")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
