# Contexto de proyecto: plugin LDAP para Notepad++

## Orientación

Plugin nativo de Windows para analizar y formatear expresiones LDAP dentro de Notepad++. El código será C++ moderno; toda interacción con el documento usa la API de Notepad++ y mensajes de Scintilla. No declares comandos de compilación hasta que haya proyectos y SDK verificables.

## Integridad y seguridad

- No modifiques el documento hasta completar el análisis y preparar una salida UTF-8 válida.
- Ante error de sintaxis, codificación, longitud o API, conserva el texto original y reporta posición y causa.
- Valida rangos, tamaños, UTF-8 y resultados de toda llamada Scintilla.
- No registres ni persistas filtros LDAP, DN, credenciales ni contenido del documento.

## C++ moderno y propiedad

- Usa RAII para memoria, HANDLE, buffers, bloqueos y estado temporal.
- Expresa propiedad con valores, referencias no propietarias y std::unique_ptr; evita new/delete directos.
- Inicializa con {} y usa tipos estándar con límites comprobables.
- Mantén el parser sin efectos laterales: entrada UTF-8 validada → resultado semántico → salida. Ninguna fase edita el documento.
- Conserva cada error con desplazamiento en bytes UTF-8; no conviertas a ANSI ni sustituyas secuencias inválidas.

## Notepad++ y Scintilla

- Usa SendMessage y SCI_*; no retengas punteros entregados por el control fuera de la llamada.
- Usa sptr_t, uptr_t y Sci_Position. No reduzcas punteros, posiciones o longitudes a int.
- Lee documento y selección al inicio; aplica un único reemplazo solo tras validar todo y agrúpalo como una operación de deshacer.
- Las llamadas UI ocurren en el hilo UI. Un análisis de fondo recibe una copia inmutable y se revalida antes de aplicar cambios.

## Arquitecturas y verificación

- Mantén builds equivalentes x86, x64 y ARM64; el binario debe coincidir con el host Notepad++.
- No trunques punteros, handles, size_t o posiciones. Evita supuestos de tamaño de palabra, alineación, endianness o layout.
- Cubre UTF-8 inválido, entrada incompleta, profundidad excesiva, escapes y documento sin selección.

## Confiabilidad de entregables

- No declares un artefacto creado por el código de salida de un comando: confirma su ruta, tamaño y encabezado con una lectura independiente antes de entregarlo.
- Para documentos UTF-8, verifica que el lector del entorno los abra sin sustituciones ni errores antes de afirmar que se preservó la codificación.
- Verifica el estado de una iniciativa BMad con su manifiesto, el directorio de salida y la configuración resuelta; no infieras que está activa desde una sola fuente.
- Antes de automatizar una escritura mediante cmd, prueba la sintaxis de invocación con una operación inocua. Este entorno puede pasar las comillas de node -e literalmente y bash.exe puede no tener una distribución ejecutable.
- Si apply_patch no puede escribir en este workspace, informa el bloqueo y usa solamente una alternativa de escritura que el usuario haya autorizado; después verifica el contenido en disco.

<!-- bmad:context -->
<!-- Verificado 2026-10-09 contra archivos y ejecución local; workspace sin revisión Git. Bloque aditivo aprobado por el usuario. -->

## Ejecución y verificación de plataforma

- Desde la raíz, en CMD: `uv --cache-dir .uv-cache run python scripts/build_matrix.py` compila las tres arquitecturas; añadir `--test` exige ABI x86/x64.
- `uv --cache-dir .uv-cache run python scripts/validate_packages.py --packages-dir .build/ci-verification` verifica los tres ZIP existentes y sus SHA-256.
- No equiparar build con matriz de pruebas verde: siguen pendientes la ejecución GitHub, la ABI x86 y el host ARM64. Para requisitos y variantes, leer `docs/ci.md`.

<!-- /bmad:context -->

## Directivas operativas del entorno Windows

- Toda llamada a `uv` debe incluir explícitamente `--cache-dir .uv-cache`, o ejecutarse con `UV_CACHE_DIR=.uv-cache`, para evitar bloqueos de permisos en AppData. Mantén `.uv-cache/` excluido de Git.
- Pasa las rutas de skills como rutas relativas desde la raíz del repositorio, por ejemplo `--skill .agents/skills/bmad-build`; evita nombres sin calificar.
- Ejecuta los comandos en CMD nativo. No uses WSL/Bash ni anides comillas que provoquen el error Win32 123. Verifica primero la sintaxis con una operación inocua.
