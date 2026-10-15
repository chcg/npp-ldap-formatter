# Auditoría 4.3: privacidad y regresiones de paquetes

Auditoría realizada el 2026-10-15 sobre el código propio del plugin de producción y los paquetes 0.1.0 x86, x64 y ARM64. La revisión de fuentes no encontró destinos propios de red, telemetría ni persistencia del contenido del editor. Los imports estáticos incluyen APIs de archivos y carga dinámica procedentes del enlace con el runtime; su presencia y la ausencia de imports de red no demuestran qué ocurre en todas las ejecuciones del proceso Notepad++.

La evidencia consolidada está en [package-privacy-audit-results.json](package-privacy-audit-results.json). Las regresiones de host simulado conservan la distinción entre contención y preservación: las pérdidas observadas por inyección de fallos mantienen `simulated_preservation: failed` y `release_blocked: true`. Esta auditoría no habilita una release.

La [revisión Ponytail del diff](ponytail-review-4.3.md) registra las correcciones y los límites de la revisión previa a integrar. CTest x64 pasó 7/7 y la suite Python final pasó 95/95.

## Alcance y procedencia

La revisión toma la lista efectiva de targets de `CMakeLists.txt`: `ldap_core` contiene `BuildInfo.cpp`, `LdapParser.cpp` y `CoreBudget.cpp`; `ldap_gateway` contiene `ScintillaGateway.cpp`, `PluginController.cpp`, `PluginDiagnostics.cpp` y `PluginWorker.cpp`; `NppLdapFormatter` añade `PluginDefinition.cpp`, `PluginExports.cpp` y su `.def`. Se contrastaron sus headers y sus relaciones con las suites existentes. `NativeUiBenchmark` es un target separado, opcional, y no pertenece a los ZIP auditados.

Los binarios se construyeron desde las fuentes de producción congeladas de la revisión `6b5dd197d41dd701558eaaf69690dc155b916a6f`; los scripts y este informe se incorporan después. El SHA-256 del conjunto de fuentes productivas registrado para este freeze es `0d66a888765dd738042eb4797919df6d0e772c6550316319b84190edab5325f8`. La evidencia final conserva esa revisión y ese digest; no presenta los binarios como compilados desde el futuro commit de la auditoría.

El digest se reproduce desde la raíz del repositorio con la unión de los archivos que coinciden con `include/*.hpp`, `src/*.cpp`, `src/*.hpp`, `src/*.def`, más `CMakeLists.txt`, `CMakePresets.json` y `cmake/Baseline.cmake`. Se ordenan lexicográficamente sus rutas relativas con separadores POSIX (`/`). Para cada archivo se leen los bytes, se reemplaza cada CRLF por LF (`data.replace(b"\r\n", b"\n")`, sin otros cambios) y se calcula SHA-256 en hexadecimal minúsculo. El índice concatena para cada ruta ordenada `ruta:sha256\n`; el digest global es SHA-256 de los bytes UTF-8 de ese índice, incluido el LF final. El manifiesto `source-inputs.json` de evidencia conserva cada ruta y hash normalizado para contrastarlo; ni ese índice ni los paquetes contienen hashes de documentos del editor.

La matriz nueva y sus paquetes están bajo `.build/privacy-audit-4-3`, separados de la evidencia 4.2. Se verificaron MZ/PE, máquina, límites de tamaño, DLL única en la raíz del ZIP y checksum del snapshot ZIP. El inventario se obtuvo con MSVC `link /dump /imports` y `/exports` sobre las DLL reales; no se añadió otro parser de PE/imports al proyecto. El runtime configurado y resuelto sigue siendo `MultiThreaded` (`/MT`) para Release.

## Flujo de datos revisado

| Etapa | Datos y operaciones propias | Frontera de privacidad e integridad |
|---|---|---|
| `PluginExports` → `PluginDefinition` | Exporta la ABI, registra los tres comandos y recibe `NppData`/notificaciones. `DllMain` sólo devuelve éxito. | No lee archivos, red ni configuración. Las notificaciones son prestadas; no se conserva su puntero de texto. |
| `PluginController` → `ScintillaGateway::capture` | En UI consulta identidad, selección, longitud, codepage, EOL y estado. Copia selección o documento a un buffer propio mediante `SCI_GETSELTEXT`/`SCI_GETTEXT`. | Valida resultados, identidad antes/después, UTF-8 estricto, NUL y límite de entrada. El documento permanece intacto durante captura y análisis. |
| `PluginWorker::work` → `formatFilter` | Transfiere la copia propia al worker, usa generación, cancelación y presupuesto; produce resultado semántico/salida o error con offset. | El worker no usa el documento vivo. El parser usa biblioteca estándar y datos en memoria, sin API de red, archivos o registro. No convierte a ANSI ni repara UTF-8 inválido. |
| `PluginController::processDelivery` → `ScintillaGateway::commit` | Vuelve a validar generación, identidad, estado y bytes capturados; prepara buffers antes del reemplazo. | Valida UTF-8/salida/tamaños antes de `SCI_REPLACESEL`; como máximo un reemplazo, con grupo de deshacer RAII y verificación posterior. Un fallo posterior puede dejar resultado indeterminado: no se presume preservación. |
| Historial y `trace` del controller | Mantiene en memoria metadatos de selección, tokens, flags, generación y handles para restauración y pruebas. | La sonda interna no almacena texto ni hashes de documentos y no escribe archivos. No es telemetría ni una traza OS de privacidad. |
| `PluginDiagnostics` → `MessageBoxW` | Construye causa/código desde tablas constantes y posiciones numéricas en bytes UTF-8; valida contexto antes de mostrar. | No incluye fragmentos de texto, filtros, DN ni credenciales. El diálogo muestra metadatos y no es un log persistente propio. |
| Cancelación/cierre | `shutdown` cancela, despierta y une el `jthread`; libera snapshots, mailbox, reservas e historial y destruye ventana/timer. | RAII limita vidas y limpieza de recursos. No se promete borrado criptográfico de memoria ni control de pagefile, dumps o recuperación del host. |

## APIs propias frente al runtime y al host

Las llamadas Win32 explícitas del código productivo se limitan a mensajería/editor/UI, coordinación de ventana y validación local de memoria/hilo: `SendMessageW`, `PostMessageW`, `MessageBoxW`, `IsWindow`, `GetCurrentThreadId`, `GetWindowThreadProcessId`, `VirtualQuery`, `GetModuleHandleExW`, `GetLastError`, `RegisterClassW`, `UnregisterClassW`, `CreateWindowExW`, `DestroyWindow`, `DefWindowProcW`, `SetWindowLongPtrW`, `GetWindowLongPtrW`, `SetTimer` y `KillTimer`. La clase `LdapFormatterWorker` usa una ventana `HWND_MESSAGE` local; no instala hooks globales.

`GetModuleHandleExW` obtiene la identidad del propio módulo, sin cargar código desde una ruta recibida del documento. `VirtualQuery` comprueba que la estructura de notificación esté en memoria legible; no enumera archivos ni procesos. La biblioteca estándar añade asignación, sincronización y soporte de `jthread`; esos imports no son destinos de contenido del parser.

No se encontraron llamadas propias a sockets/HTTP/LDAP remoto, servicios de telemetría, escritura de archivos o registro, logging del contenido, ni hashes del documento en estas fuentes productivas. No hay un cliente LDAP: el filtro se analiza localmente. Esta conclusión corresponde a la revisión del código propio; no equivale a una observación dinámica completa.

Las tres DLL importan `KERNEL32.dll` y `USER32.dll`; ARM64 añade `api-ms-win-core-synch-l1-2-0.dll`. Las tres incluyen `CreateFileW`, `WriteFile`, `GetProcAddress` y `LoadLibraryExW` en su inventario enlazado. Por ello sería incorrecto declarar que la DLL carece de APIs capaces de persistir o cargar código. No se hallaron consumidores propios que reciban el documento y llamen a esas APIs; el CRT estático aporta infraestructura de runtime. Sin una traza de ejecución y atribución de stacks no se asegura que ninguna ruta del CRT pueda invocarlas.

El inventario completo de símbolos por DLL está en `artifacts[].imported_functions` del JSON consolidado: x86 tiene 14 imports USER32 y 83 KERNEL32; x64, 14 y 86; ARM64, 14 y 78 más 2 del API set de sincronización. Entre los imports de runtime también figuran `FindFirstFileExW`, `FindNextFileW`, `FlushFileBuffers`, `WriteConsoleW`, `MultiByteToWideChar` y `WideCharToMultiByte`. Las conversiones del CRT no son conversiones del filtro: la ruta productiva revisada trata su entrada como UTF-8 estricto. x86 resuelve `GetWindowLongPtrW`/`SetWindowLongPtrW` a `GetWindowLongW`/`SetWindowLongW`, conforme a la API de Windows de esa arquitectura.

Notepad++, Scintilla, el sistema operativo, otros plugins, antivirus, pagefile y volcados quedan fuera de esta garantía. El host recibe la salida mediante la API del editor y puede guardar el documento según sus propias funciones. No se instrumentó todo el proceso, no se habilitó telemetría ni se enviaron datos a un servicio.

## Identidad de los paquetes reales

Cada ZIP contiene sólo `NppLdapFormatter.dll`. Las seis exportaciones observadas en todas las arquitecturas son `beNotified`, `getFuncsArray`, `getName`, `isUnicode`, `messageProc` y `setInfo`.

| Arquitectura | Bytes DLL | SHA-256 DLL | SHA-256 ZIP |
|---|---:|---|---|
| x86 | 188416 | `75248703f3c67aeb716e8c321f7a0b0941f7dbd60468edba99a062d672077e46` | `072f4f92f9c33982484203d7906e871e27624b1ac4426d2250af19488426bb42` |
| x64 | 244736 | `adeba51265876f02b19d0ca462090457ae5d99cc504eb7a5d4c63ef3c5307ef0` | `b64fcad825c2c1f00413d20d95be9e41483716cadcd19a1718caa952bf384292` |
| ARM64 | 214528 | `83c6df11e31479b273273880fb867ab2ecbefc743f42504616a94da49d72be33` | `a6e0695db4ebcbb656fc8b2d1698d61ebdf6684ac6f3f51377724a8aca36674c` |

## Regresión vinculada al snapshot

`scripts/audit_package_regressions.py` exige `--archive`, `--arch`, `--harness`, `--revision` y `--manifest`. El manifiesto local previamente revisado es JSON UTF-8 de hasta 4096 bytes, con exactamente siete campos: `schema` (entero 1), `revision` (40 hexadecimales minúsculos), `architecture` (`x86`, `x64` o `ARM64`), `archive_sha256`, `dll_sha256`, `harness_sha256` (64 hexadecimales minúsculos cada uno) y `suite_cases` (entero positivo). Rechaza duplicados, campos adicionales y tipos escalares incorrectos, incluidos booleanos usados como enteros.

El wrapper exige que revisión y arquitectura del CLI coincidan con el manifiesto. Lee una sola copia acotada del ZIP, valida su checksum y DLL con `validate_package_data`, y contrasta los SHA-256 de ZIP, DLL y harness con la tupla aprobada antes de ejecutar. También verifica que el harness tenga la misma máquina PE y pueda ejecutarse en la plataforma local. ARM64 no se ejecuta en AMD64; las pruebas positivas de dispatch x86/AMD64 y ARM64/ARM64 usan PE y subprocess sintéticos y no son ejecuciones nativas. Después del harness exige exactamente el número de casos aprobado.

El manifiesto fija una tupla de artefactos revisada localmente; no es una firma ni una prueba criptográfica de procedencia del build. `--revision` por sí solo es una etiqueta del operador. La revisión de fuentes y los builds/paquetes verificados independientemente sustentan esa etiqueta y la aprobación del manifiesto. Este wrapper sólo está destinado a paquetes y harness locales aprobados: ejecuta código nativo, no aísla ejecutables arbitrarios ni convierte un manifiesto escrito por un tercero en aprobación. Se asume que esos artefactos locales no se sustituyen concurrentemente durante la ejecución.

El wrapper escribe únicamente la DLL validada en un `TemporaryDirectory` propio, comprueba bytes por lectura independiente y que su ruta resuelta permanezca bajo esa raíz. No usa `extractall`, nombres internos arbitrarios, borrado recursivo construido desde entradas ni opción para sustituir la DLL. La limpieza RAII se limita al directorio temporal creado por el wrapper. Invoca `host_evidence.run` con esa ruta exacta y exige que la identidad del reporte corresponda a la DLL temporal validada.

La salida de éxito sólo contiene `schema: 1`, `archive_sha256`, la identidad PE `harness` (tamaño, SHA-256, máquina y validez PE) y el `report` estricto ya existente. No reenvía el manifiesto. No hay opción de certificación nativa, release o salida a archivo que pueda sobrescribir los inputs. Los errores muestran una causa genérica sin rutas, excepciones ni stdout/stderr del harness; el parser de argumentos también evita repetir opciones o valores inválidos en stdout/stderr. `host_evidence` rechaza stdout malformado, campos adicionales/sensibles, contadores inconsistentes, salida de fallo y stderr no vacío, aun con stdout válido. Este último rechazo endurece la frontera compartida con una prueba focal; no se relaja su esquema ni se persiste stderr.

## Trazabilidad de requisitos y evidencia

| Requisito | Suite/evidencia reutilizada | Resultado y alcance |
|---|---|---|
| UTF-8 estricto, NUL, errores sin editar | `LdapParserTests`, `ScintillaGatewayTests`, `PluginControllerTests`, 18 rechazos de `PluginDllE2ETests` | Corpus existente; la ejecución del wrapper aplica el E2E a la DLL exacta del paquete x64. No se duplicó el corpus. |
| Unicode, EOL, escapes y entrada incompleta | `tests/LdapParserTests.cpp` y E2E del menú real bajo host simulado | Cubiertos por suites existentes; mensajes de diagnóstico sin texto de entrada. |
| 5 MiB exactos/+1, profundidad 128/129 y expansión de salida | `LdapParserTests` y `PluginDllE2ETests` | Límites del parser y rechazo previo al reemplazo; selección válida en documento grande incluida en E2E. |
| Presupuesto y cancelación | `LdapParserTests`, `PluginControllerTests` | Reutiliza tests de cuota, recursos y cancelación. No añade cambios al comportamiento productivo. |
| Snapshot ZIP/DLL exacto y rechazo antes de ejecución | `tests/test_audit_package_regressions.py` | 14 pruebas pasan: límites/PE/SHA, snapshot inmutable, manifiesto malformado o con identidades distintas, cobertura exacta, argumentos sanitizados, identidad del harness, salida sensible/fallo y timeout. Dispatch positivo por arquitectura es simulado; limpieza temporal comprobada en éxito y error. |
| Sin persistencia de contenido en evidencia | Esquemas `host_evidence`, tests existentes y tests del wrapper | Sólo identidades de fuentes/binarios/ZIP, enums y números; nunca filtros, DN, credenciales, texto documental ni sus hashes. |
| Build equivalente x86/x64/ARM64, `/MT`, PE/imports/exports | Matriz nueva, validación de paquetes y `link /dump` | Las tres arquitecturas compilan y empaquetan; esto no certifica ejecución de host ni ABI en las tres. |
| Ejecución x64 | CTest `build-x64` y wrapper sobre paquete x64 | CTest 7/7; wrapper: 103 casos, 0 fallos de aserciones, 6 faultcases con 6 pérdidas de integridad. Contención pasa; preservación falla y release sigue bloqueada. |
| Scripts y fronteras de evidencia | `unittest discover` completo y suite focal del wrapper | El JSON consolidado registra la ejecución completa final; la suite focal modificada pasa 14/14 fuera del sandbox por permisos de temporales Windows. |
| ABI/E2E x86 | CTest de la matriz x86 | 5/7; ABI y E2E no ejecutados porque faltan sus EXE en ese build. Discrepancia documentada, sin declarar verde. |
| ARM64 | Build/ZIP/PE/imports/exports | Verificación estática cruzada. Runtime/host ARM64 pendiente; no se ejecuta en AMD64. |
| Fallos posteriores a la edición | Seis faultcases existentes de `PluginDllE2ETests` | Contención puede pasar con pérdida de texto/selección/historial. Se conservan igualdades, contadores y gate de preservación fallido; no se transforma en éxito de integridad. |

Los resultados de suites no deben interpretarse como una traza OS de red o archivos. Siguen separados los gates nativos de callbacks, selección/deshacer, preservación ante fallos y ARM64; las obligaciones de 4.4 no se cierran por inferencia desde 4.3.
