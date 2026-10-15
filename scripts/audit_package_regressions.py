"""Ejecutar regresiones simuladas contra la DLL del snapshot ZIP validado."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import zipfile
import zlib

if __package__:
    from . import host_evidence
    from .validate_packages import DLL_NAME, MACHINES, MAX_ARCHIVE_BYTES, validate_package_data
else:
    import host_evidence
    from validate_packages import DLL_NAME, MACHINES, MAX_ARCHIVE_BYTES, validate_package_data


class AuditArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, "Argumentos de auditoría inválidos\n")


def load_manifest(path: Path) -> dict:
    with path.open("rb") as stream:
        raw = stream.read(4097)
    if len(raw) > 4096:
        raise ValueError("Manifiesto fuera de límites")
    pairs = json.loads(raw.decode("utf-8", errors="strict"), object_pairs_hook=lambda values: values)
    keys = {"schema", "revision", "architecture", "archive_sha256", "dll_sha256", "harness_sha256", "suite_cases"}
    if not isinstance(pairs, list) or len(pairs) != 7 or any(not isinstance(pair, tuple) for pair in pairs):
        raise ValueError("Manifiesto inválido")
    value = dict(pairs)
    if set(value) != keys or type(value["schema"]) is not int or value["schema"] != 1:
        raise ValueError("Campos de manifiesto inválidos")
    if type(value["architecture"]) is not str or value["architecture"] not in MACHINES:
        raise ValueError("Arquitectura de manifiesto inválida")
    for key, length in (("revision", 40), ("archive_sha256", 64), ("dll_sha256", 64), ("harness_sha256", 64)):
        if type(value[key]) is not str or not re.fullmatch(r"[0-9a-f]{" + str(length) + r"}", value[key]):
            raise ValueError("Identidad de manifiesto inválida")
    if type(value["suite_cases"]) is not int or value["suite_cases"] < 1:
        raise ValueError("Cobertura de manifiesto inválida")
    return value


def run(archive: Path, arch: str, harness: Path, revision: str, manifest: Path) -> dict:
    if arch not in MACHINES or not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Arquitectura o revisión inválida")
    approved = load_manifest(manifest)
    if approved["revision"] != revision or approved["architecture"] != arch:
        raise ValueError("Argumentos distintos del manifiesto aprobado")
    with archive.open("rb") as stream:
        snapshot = stream.read(MAX_ARCHIVE_BYTES + 1)
    with archive.with_suffix(".zip.sha256").open(encoding="utf-8", errors="strict") as stream:
        checksum = stream.read(1025)
    if len(checksum) > 1024:
        raise ValueError("Checksum fuera de límites")
    digest, content = validate_package_data(snapshot, checksum, archive.name, arch)
    identity = host_evidence.inspect_pe(harness)
    if identity["machine"] != arch:
        raise ValueError("Harness incompatible con el paquete")
    if (digest != approved["archive_sha256"] or hashlib.sha256(content).hexdigest() != approved["dll_sha256"]
            or identity["sha256"] != approved["harness_sha256"]):
        raise ValueError("Artefactos distintos del manifiesto aprobado")
    supported = {"AMD64": {"x86", "x64"}, "X86_64": {"x86", "x64"},
                 "X86": {"x86"}, "I386": {"x86"}, "ARM64": {"ARM64"}, "AARCH64": {"ARM64"}}
    if platform.system() != "Windows" or arch not in supported.get(platform.machine().upper(), set()):
        raise ValueError("Plataforma sin harness ejecutable para esta arquitectura")
    # Sólo se materializa la DLL validada; TemporaryDirectory posee su limpieza.
    with tempfile.TemporaryDirectory(prefix="npp-ldap-package-audit-") as directory:
        root = Path(directory).resolve(strict=True)
        dll = root / DLL_NAME
        if dll.resolve().parent != root:
            raise ValueError("DLL fuera del directorio temporal propio")
        dll.write_bytes(content)
        if dll.read_bytes() != content:
            raise ValueError("La DLL temporal no coincide con el snapshot")
        expected = host_evidence.inspect_pe(dll)
        report = host_evidence.run(harness, dll, revision)
        host_evidence.validate_report(report)
        if report["dll"] != expected:
            raise ValueError("El reporte no identifica la DLL del snapshot")
        if report["simulated"]["cases"] != approved["suite_cases"]:
            raise ValueError("Cobertura distinta del manifiesto aprobado")
    return {"schema": 1, "archive_sha256": digest, "harness": identity, "report": report}


def main() -> int:
    parser = AuditArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--arch", choices=MACHINES, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = run(args.archive, args.arch, args.harness, args.revision, args.manifest)
        print(json.dumps(result, indent=2, ensure_ascii=True))
        return 0
    except (ValueError, OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError, EOFError, zlib.error, subprocess.TimeoutExpired):
        # No rutas, excepciones ni stdout/stderr del harness cruzan esta frontera.
        print("Auditoría de paquete rechazada; plataforma o insumos incompatibles; release bloqueado")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
