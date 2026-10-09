"""Paquete Notepad++: DLL en raíz y checksum SHA-256 del ZIP exacto."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import zipfile

MACHINES = {"x86": 0x014C, "x64": 0x8664, "ARM64": 0xAA64}
DLL_NAME = "NppLdapFormatter.dll"


def validate_dll(data: bytes, arch: str) -> None:
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("DLL ausente, truncada o sin encabezado DOS")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset + 24 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Encabezado PE invalido")
    machine = struct.unpack_from("<H", data, offset + 4)[0]
    characteristics = struct.unpack_from("<H", data, offset + 22)[0]
    if machine != MACHINES[arch] or not characteristics & 0x2000:
        raise ValueError("El binario no es una DLL de la arquitectura solicitada")
    sections = struct.unpack_from("<H", data, offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, offset + 20)[0]
    optional = offset + 24
    expected_magic = 0x10B if arch == "x86" else 0x20B
    minimum_size = 96 if arch == "x86" else 112
    section_table = optional + optional_size
    if optional_size < minimum_size or section_table > len(data):
        raise ValueError("Encabezado opcional PE truncado")
    if struct.unpack_from("<H", data, optional)[0] != expected_magic:
        raise ValueError("Formato PE32/PE32+ incompatible")
    if sections == 0 or section_table + sections * 40 > len(data):
        raise ValueError("Tabla de secciones PE invalida")
    for index in range(sections):
        section = section_table + index * 40
        size, pointer = struct.unpack_from("<II", data, section + 16)
        if size and (pointer < section_table + sections * 40 or pointer + size > len(data)):
            raise ValueError("Datos de seccion PE fuera de limites")


def package(dll: Path, output_dir: Path, arch: str, version: str) -> Path:
    if dll.name != DLL_NAME:
        raise ValueError("Nombre de DLL inesperado")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Version invalida")
    data = dll.read_bytes()
    validate_dll(data, arch)
    output_dir.mkdir(parents=True, exist_ok=True)
    archive = output_dir / f"NppLdapFormatter-{version}-{arch}.zip"
    checksum = archive.with_suffix(".zip.sha256")
    if archive.exists() or checksum.exists():
        raise FileExistsError("Ya existe el ZIP o su checksum")
    # No mezclar resultados de una ejecución anterior.
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as zipped:
        info = zipfile.ZipInfo(DLL_NAME, date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        zipped.writestr(info, data)
    with zipfile.ZipFile(archive) as zipped:
        if zipped.namelist() != [DLL_NAME] or zipped.read(DLL_NAME) != data:
            raise ValueError("Contenido ZIP no coincide con DLL original")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    with checksum.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{digest}  {archive.name}\n")
    print(f"SHA-256 {archive.name}: {digest}")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with Path(summary).open("a", encoding="utf-8") as stream:
            stream.write(f"\n### Paquete {arch}\n\n`{archive.name}`\n\nSHA-256: `{digest}`\n")
    return archive


def project_version(manifest: Path) -> str:
    match = re.search(r"project\(NppLdapFormatter VERSION ([0-9]+\.[0-9]+\.[0-9]+)\s", manifest.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError("Version de proyecto no encontrada")
    return match.group(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=MACHINES, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    manifest = Path(__file__).resolve().parents[1] / "CMakeLists.txt"
    package(args.dll, args.output_dir, args.arch, project_version(manifest))


if __name__ == "__main__":
    main()
