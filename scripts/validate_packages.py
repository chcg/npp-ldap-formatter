"""Verificar ZIP de distribucion, DLL en raiz y SHA-256 sin modificar artefactos."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import zipfile

if __package__:
    from .package_plugin import DLL_NAME, MACHINES, project_version, validate_dll
else:
    from package_plugin import DLL_NAME, MACHINES, project_version, validate_dll


def validate_package(archive: Path, arch: str, dll: Path | None = None) -> str:
    data = archive.read_bytes()
    checksum = archive.with_suffix('.zip.sha256').read_text(encoding='utf-8').strip()
    match = re.fullmatch(r'([0-9a-f]{64})  (.+)', checksum)
    digest = hashlib.sha256(data).hexdigest()
    if match is None or match.group(1) != digest or match.group(2) != archive.name:
        raise ValueError('Checksum SHA-256 o nombre de ZIP incorrecto')
    with zipfile.ZipFile(io.BytesIO(data)) as zipped:
        if zipped.namelist() != [DLL_NAME]:
            raise ValueError('El ZIP debe contener solamente la DLL en raiz')
        content = zipped.read(DLL_NAME)
    validate_dll(content, arch)
    if dll is not None and content != dll.read_bytes():
        raise ValueError('La DLL empaquetada no coincide con el build')
    return digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages-dir', type=Path, required=True)
    parser.add_argument('--arch', choices=['all', *MACHINES], default='all')
    parser.add_argument('--dll', type=Path, help='Comparar con DLL original; exige --arch especifico')
    args = parser.parse_args()
    if args.dll and args.arch == 'all':
        parser.error('--dll exige una sola arquitectura')
    version = project_version(Path(__file__).resolve().parents[1] / 'CMakeLists.txt')
    arches = list(MACHINES) if args.arch == 'all' else [args.arch]
    for arch in arches:
        directory = args.packages_dir / arch if args.arch == 'all' else args.packages_dir
        archive = directory / f'NppLdapFormatter-{version}-{arch}.zip'
        digest = validate_package(archive, arch, args.dll)
        print(f'{arch}: {archive.stat().st_size} bytes; DLL raiz y SHA-256 verificados: {digest}')


if __name__ == '__main__':
    main()
