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

## Flujo BMad + Graphify + Ponytail

Las garantías LDAP, UTF-8, Scintilla, RAII y equivalencia x86/x64/ARM64 de este archivo son obligatorias. Dentro de ellas, Graphify guía la inspección, Ponytail limita la implementación y BMad gobierna el ciclo de vida. El minimalismo nunca elimina una validación, un requisito ni evidencia de verificación.

### Navegación e inspección: Graphify primero

- Antes de buscar a ciegas con `grep`/`rg`, leer archivos fuente completos o rastrear símbolos, consulta `graphify query "<pregunta>" --budget 1500`. Para dependencias usa `graphify path "<A>" "<B>"`; para un nodo, `graphify explain "<nodo>"`; para impacto inverso, `graphify affected "<nodo>"`.
- Usa nombres de símbolos o rutas del grafo como semillas. Resuelve nombres ambiguos con su identificador; una consulta sin coincidencias no demuestra ausencia de dependencias.
- Lee únicamente las líneas y archivos localizados por el subgrafo. Evita cargar archivos completos o el reporte completo cuando una consulta acotada basta. Confirma en el código real las relaciones que fundamentan un cambio: el AST no resuelve toda la interacción dinámica de Windows/Scintilla.
- Si el grafo falta o está desactualizado, ejecuta `graphify extract . --code-only` o `graphify update .` antes de inspeccionar. Si la herramienta falla o su cobertura es insuficiente, informa la limitación y usa `rg` dirigido a símbolos/rutas concretos para continuar; no inventes aristas.
- Después de cambiar código, ejecuta `graphify update .` antes de revisar el impacto. Los hooks de Git actualizan tras commit, checkout y merge; no sustituyen la actualización durante el trabajo sin commit.
- Conserva `graphify-out/` local e ignorado. Indexa únicamente código con AST local: no envíes documentos, filtros LDAP, DN, credenciales ni contenido del editor a servicios externos. No uses `graphify save-result` para persistir esos datos.
- Si delegas exploración de código, incluye estas reglas y los nodos relevantes en el encargo; evita repetir lecturas masivas entre agentes.

### Diseño e implementación: Ponytail y regla del peldaño

Elige el primer peldaño que resuelva completamente el requisito: omitir lo no solicitado → reutilizar código/patrones del proyecto → biblioteca estándar o API nativa → dependencia ya instalada → una expresión clara → mínimo código adicional necesario.

- No reinventes dependencias ni añadas interfaces, capas, wrappers, conversiones, opciones o flexibilidad para necesidades hipotéticas. Mantén las convenciones y capas existentes; prioriza claridad sobre reducir líneas artificialmente.
- Completa los cambios necesarios en consumidores, pruebas, fixtures y configuración. Corrige la causa compartida una sola vez tras consultar su impacto en el grafo.
- Conserva validación en límites de confianza, seguridad, accesibilidad, RAII, tamaños comprobados y manejo estricto de errores que evita pérdida de datos.
- Toda lógica crítica nueva o modificada con ramificaciones o manipulación de estado lleva un test unitario conciso que compruebe su comportamiento, incluidos errores relevantes. Los cambios mecánicos triviales no requieren tests que reflejen la implementación.
- Documenta un atajo con límite conocido mediante `shortcut: <límite>, <cuándo ampliar>`. No uses atajos para debilitar las garantías de este archivo.
- Estas reglas compartidas están siempre vigentes aunque los hooks de Ponytail todavía no estén activados en una sesión.

### Ciclo de vida y roles: BMad Method

- Planificación y tickets: usa exclusivamente `bmad-ticket` y sus scripts, en particular `uv --cache-dir .uv-cache run python .agents/skills/bmad-ticket/scripts/tickets.py --project-root . <comando>`. No edites manualmente estados de tickets ni sustituyas el store de BMad con un tracker paralelo.
- Verifica iniciativa mediante manifiesto, directorio de salida y configuración resuelta antes de operar. Usa una ruta explícita de iniciativa si el script instalado no resuelve el formato actual; no migres ni repares BMad silenciosamente.
- Para planificación rutinaria y operaciones mecánicas de tickets, selecciona Luna (`gpt-6-luna`) o Terra (`gpt-5.6-terra`) cuando estén disponibles en el host. Estos identificadores se comprobaron con `codex debug models` el 2026-10-14; revalida la disponibilidad al seleccionar. No afirmes un cambio de modelo que no ocurrió. Si no están disponibles, declara el modelo efectivo y conserva el flujo BMad.
- Construcción (`bmad-build` / Amelia): usa Sol (`gpt-6.1-sol`, disponible en el catálogo verificado); localiza con Graphify los puntos de impacto y aplica Ponytail para producir el mínimo código robusto que satisfaga los criterios de aceptación. Respeta los checks del flujo BMad y del proyecto. La selección ocurre al iniciar la sesión o delegar con soporte del host; esta directiva no cambia automáticamente el modelo de una sesión activa.
- Revisión (`bmad-code-review` y, para minimalismo, `$ponytail:ponytail-review`): revisa el diff y los consumidores, dependencias y pruebas de los nodos circundantes mediante `query`, `explain` y `affected`. Confirma relaciones relevantes en el código y ejecuta las pruebas correspondientes al impacto.
- No marques un ticket como `done` por terminar de escribir código: exige revisión del impacto, criterios de aceptación y evidencia de pruebas; registra la transición con `tickets.py mark` según el flujo instalado. No declares verde una arquitectura o prueba de host que no se haya ejecutado.

Instalación, comandos y estado verificado: `docs/development-workflow.md`.
