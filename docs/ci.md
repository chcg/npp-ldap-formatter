# CI/CD Windows

El workflow `.github/workflows/ci.yml` corre en push, pull request y ejecución manual, sin publicar releases ni desplegar. Usa CMD y `windows-2022`; tres jobs independientes compilan Win32/x86, x64 y ARM64, Release, C++20, MSVC v143, `/MT`, `/W4 /WX` y análisis estático `/analyze`.

Se reutilizan los presets y el manifiesto: MSVC 14.44.35207, SDK 10.0.26100.0, CMake 3.31.6 y checkout oficial Notepad++ fijado por commit. Si cambia la imagen del runner y falta el baseline, la configuración falla; no selecciona versiones distintas silenciosamente. Las acciones checkout/upload-artifact están fijadas por SHA y usan permisos mínimos, sin conservar credenciales Git.

Los jobs x86/x64 ejecutan el harness ABI mediante CTest y fallan si no hay pruebas. ARM64 compila DLL/harness de forma cruzada y verifica la arquitectura PE durante el empaquetado; no ejecuta un binario ARM64 en un runner x64. La prueba en host nativo ARM64 sigue siendo una puerta de distribución posterior.

Cada artefacto contiene un ZIP `NppLdapFormatter-<version>-<arch>.zip` y su archivo `.zip.sha256`. Dentro del ZIP hay solamente `NppLdapFormatter.dll` en raíz, sin carpeta envolvente. El empaquetador valida formato PE, flag DLL y arquitectura antes de empaquetar, vuelve a leer el ZIP y calcula SHA-256 sobre sus bytes finales. Registra el digest en logs y resumen del job. El digest del contenedor de upload-artifact no reemplaza este checksum del ZIP de distribución.

La inclusión de archivos bajo `.build` en upload-artifact es explícita y limitada a ZIP/checksum, no al workspace ni a secretos. Retención: 14 días. Una colisión de nombre de paquete falla en lugar de reemplazar silenciosamente evidencia anterior.

## Validación local desde CMD

```bat
uv run python -m unittest discover -s tests -p test_*.py -v
uv run python scripts/build_matrix.py
uv run python scripts/build_matrix.py --arch x64 --test
uv run python scripts/package_plugin.py --arch x64 --dll .build/x64/Release/NppLdapFormatter.dll --output-dir .build/packages/x64
uv run python scripts/validate_packages.py --arch x64 --packages-dir .build/packages/x64 --dll .build/x64/Release/NppLdapFormatter.dll
uv run python scripts/validate_packages.py --packages-dir .build/ci-verification
```

Para repetir el empaquetado, usar un directorio de salida nuevo. Compilar los presets win32/arm64 antes de empaquetar esas arquitecturas. Un build local y el lint del workflow no demuestran una ejecución de GitHub Actions: esa evidencia requerirá publicar el proyecto y conservar URL/run ID y artefactos remotos.

`build_matrix.py` reutiliza los presets desde la raíz real del proyecto, por lo que no necesita `vcvars` ni CMake en PATH: por defecto usa `uvx` con CMake 3.31.6. Exige Windows, Visual Studio 2022/MSVC y SDK fijados ya instalados, junto al checkout SDK correcto en `.build/notepad-plus-plus`. No instala ni modifica Visual Studio o el SDK. `--cmake <ruta>` permite usar el ejecutable disponible en CI; los checks del manifiesto siguen activos. No ejecutar desde PowerShell ni anidar comillas literales en las rutas.

Por defecto compila solamente las tres arquitecturas; no declara pruebas aprobadas. `--test` exige CTest x86/x64 y devuelve error ante el primer fallo; ARM64 no ejecuta binarios nativos en host x64. CI usa el mismo script con `--arch`/`--cmake cmake`/`--test`. El validador es de solo lectura: por defecto busca `<directorio>/<arquitectura>/NppLdapFormatter-<version>-<arquitectura>.zip`; para `--arch` individual busca directamente en el directorio y admite `--dll` para comparar con el build original.

Referencias oficiales: [runner Windows 2022](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md), [paquetes Notepad++](https://github.com/notepad-plus-plus/npp-usermanual/blob/master/content/docs/plugins.md), [upload-artifact](https://github.com/actions/upload-artifact).
