"""Compilar los presets Release fijados, desde CMD y sin inicializar otro shell."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PRESETS = {'x86': 'win32', 'x64': 'x64', 'ARM64': 'arm64'}


def run_matrix(arches: list[str], cmake: list[str], tests: bool = False) -> None:
    for arch in arches:
        preset = PRESETS[arch]
        commands = [
            cmake + ['--preset', preset, '-DBUILD_TESTING=ON',
                     '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', '-DCMAKE_CXX_FLAGS=/analyze'],
            cmake + ['--build', '--preset', preset + '-release'],
        ]
        if tests and arch != 'ARM64':
            # uvx mantiene también ctest dentro del paquete CMake fijado.
            ctest = cmake[:-1] + [str(Path(cmake[-1]).with_name('ctest' + Path(cmake[-1]).suffix))]
            commands.append(ctest + ['--test-dir', '.build/' + preset, '-C', 'Release',
                                     '--output-on-failure', '--no-tests=error'])
        for command in commands:
            print(f'{arch}: {subprocess.list2cmdline(command)}', flush=True)
            subprocess.run(command, cwd=ROOT, check=True)
        print(f'{arch}: build Release completado; ' + (
            'ABI verificada' if tests and arch != 'ARM64' else 'ejecucion ABI no realizada'), flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['all', *PRESETS], default='all')
    parser.add_argument('--cmake', help='Ruta al ejecutable cmake; por defecto uvx CMake 3.31.6')
    parser.add_argument('--test', action='store_true', help='Ejecutar ABI x86/x64; ARM64 solo compilacion cruzada')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('El baseline requiere Windows y MSVC v143')
    cmake = [args.cmake] if args.cmake else ['uvx', '--cache-dir', str(ROOT / '.uv-cache'), '--from', 'cmake==3.31.6', 'cmake']
    run_matrix(list(PRESETS) if args.arch == 'all' else [args.arch], cmake, args.test)


if __name__ == '__main__':
    main()
