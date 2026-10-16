# Baseline de compilación — ticket 1.1

Fecha: 2026-10-09.

C++20 obligatorio: PluginController necesita std::jthread/stop_token. C++17 no satisface la arquitectura aprobada.
Baseline: CMake 3.31.6, Visual Studio 2022, MSVC v143/14.44.35207 (compilador 19.44), Windows SDK 10.0.26100.0.
SDK oficial Notepad++ 8.9.8.1, commit dd40fe45598791e9aa38f3aeb200e10fb3d6338a, Scintilla 5.6.6 (version.txt = 566).
La versión mínima elegida es conservadora: no supone compatibilidad con hosts anteriores sin prueba.

## Arquitecturas y enlaces

Los presets win32/x64/arm64 seleccionan Win32/x64/ARM64. Cada DLL futura debe cargar en Notepad++ de igual arquitectura.
La selección de plataforma incluye `version=10.0.26100.0`: esto fija el SDK aunque haya versiones más recientes instaladas. Con CMP0149 en NEW, `CMAKE_SYSTEM_VERSION` por sí solo no lo fija.
La versión de toolset se comprueba en la solicitud del generador y en el directorio real de `cl.exe`; no se exige que `CMAKE_VS_PLATFORM_TOOLSET_VERSION` sea no vacío, porque CMake puede normalizarlo cuando coincide con el toolset predeterminado.
CRT estático /MT en Release y /MTd en Debug; toda memoria se libera en la DLL que la asignó y el host no recibe STL ni ownership.
npp_ldap_build_options es un contrato INTERFACE; npp_ldap_host_sdk consume headers y user32 del SDK Windows.
baseline_sdk_probe es una biblioteca STATIC de comprobación. El plugin se entregará como MODULE/DLL en 1.2 con core/gateway estáticos internos.
No se enlaza Scintilla como biblioteca: la integración utiliza mensajes al control del host.
No hay dependencias de runtime externas, vcpkg, Boost, ni descarga automática durante configure.

## SDK y herramientas

El SDK se obtiene en .build/notepad-plus-plus mediante clone sparse del tag v8.9.8.1; verificar el commit de Baseline.cmake.
Rutas requeridas: PowerEditor/src/MISC/PluginsManager y scintilla/include.
CMake verifica commit, ausencia de cambios locales, versión de Scintilla, headers y macros. El checkout debe conservar .git para comprobar la identidad de origen.
La instalación de Visual Studio/MSVC y SDK es prerrequisito de los presets nativos.

En CMD, ejecutar primero `set UV_CACHE_DIR=.uv-cache` para mantener también la caché de `uvx` en el workspace.
Comandos de manifiesto: uvx --from cmake==3.31.6 --with ninja==1.11.1.3 cmake --preset manifest-check.
Comandos nativos una vez instaladas las herramientas: cmake --preset win32 / x64 / arm64, y cmake --build --preset win32-release / x64-release / arm64-release.
Si CMake no está en PATH, sustituir `cmake` por `uvx --from cmake==3.31.6 --with ninja==1.11.1.3 cmake`. Los presets Visual Studio descubren el toolchain instalado; para inicializar CMD explícitamente, invocar `vcvars64.bat` de la instancia 2022 con `10.0.26100.0 -vcvars_ver=14.44` y ejecutar configure/build en la misma sesión.
Después de corregir una selección de plataforma ya almacenada en cache, usar `cmake --fresh --preset x64` (o el preset correspondiente) para regenerar ese directorio.
El primer comando no compila C++; el segundo compila sdk_probe contra Windows y headers fijados.

## Fuentes oficiales

- https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2017%202022.html
- https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html
- https://cmake.org/cmake/help/v3.31/variable/CMAKE_GENERATOR_PLATFORM.html
- https://github.com/notepad-plus-plus/notepad-plus-plus/tree/dd40fe45598791e9aa38f3aeb200e10fb3d6338a
