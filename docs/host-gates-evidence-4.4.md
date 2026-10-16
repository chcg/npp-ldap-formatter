# Gates de host 4.4

Fecha: 2026-10-15. **Decisión de release: bloqueada.** Esta entrega corrige la
recuperación del reemplazo propio y consolida evidencia; el cierre de 4.4 no
certifica los gates nativos pendientes ni cierra la épica.

[Revisión Ponytail del diff y consumidores](ponytail-review-4.4.md), completada
antes de integrar; los hallazgos BMad y su disposición quedan trazados allí.

## Resultados medidos

Build completo `build-x64` Release `/MT` y `/analyze`: pasó sin advertencias.
CTest completo: **7/7**; Python completo: **98/98**, sin skips. La ejecución
directa del harness cargando la DLL de producción devuelve **118 casos, cero
fallos de aserciones**, seis observaciones fijas de fallos y seis desigualdades
de integridad. `simulated_containment=passed`, `simulated_preservation=failed`,
`native_gates=pending`, `release_blocked=true`.

| Caso | Texto igual | Selección igual | Historial igual | REPLACESEL | SCI_UNDO |
|---:|---|---|---|---:|---:|
| 0: error antes de editar | sí | sí | no | 1 | 0 |
| 1: reemplazo completo con error | sí | sí | no | 1 | 1 |
| 2: borrado interno con error | sí | sí | no | 1 | 1 |
| 3: callback END cambia selección | no | no | no | 1 | 0 |
| 4: callback de token pierde identidad | sí | no | no | 0 | 0 |
| 5: callback de selección modifica bytes | no | sí | sí | 0 | 0 |

Los casos 1/2 recuperan exactamente buffer y selección; conservan la acción
fallida en Redo, por lo que el historial difiere. El token de contenedor también
modifica el historial en los casos 0 y 4, aunque REPLACESEL no cambie bytes.
Insertarlo descarta un Redo previo: esa pérdida no se oculta ni se reconstruye.
Los casos 3..5 mantienen sus discrepancias explícitas y no autorizan Undo ciego.
Las quince regresiones nuevas
fuera de las seis observaciones fijas comprueban borrado/error interno con
selección directa/inversa, edición independiente previa, fallo temprano,
pérdida de identidad al cerrar, Undo fallido, error de END, grupo externo/anidado,
marcador temprano con Redo previo y Pretty→Compact con dos Undo y dos Redo.
El simulador distingue índices de acciones de token/contenido y cursor lógico de
grupos; no pretende reproducir cada acción interna de Scintilla.
Los fallos deliberados sin recuperación no se presentan como preservación.

Identidades leídas independientemente de los binarios en disco; ambos tienen
MZ, firma PE `50450000` y máquina `8664` (x64):

| Artefacto | Bytes | SHA-256 |
|---|---:|---|
| `build-x64/Release/NppLdapFormatter.dll` | 247808 | `c7bc06831f7544688ab8664bb1017eddc9915ac828bef9f5aff06b4291e47c46` |
| `build-x64/Release/plugin_dll_e2e_tests.exe` | 327168 | `85ac1147f3b4cf19e1b61d2359968fada37bef446f6d8b7a6e071db201b90531` |

El build parte de `f8607ac1d053c9cd67ce65e4a5bbde8ca9721fe6` más los cambios
locales 4.4 de esta entrega; ese baseline por sí solo no identifica las fuentes
modificadas. La revisión de integración se registra después de consolidar.

Revisión consolidada de código y pruebas:
`b7bc7e3682cb85eea8a8510c18a4f4048e922f54`. Las verificaciones de la
continuación y las identidades de binarios anteriores corresponden a esas
fuentes; el registro posterior de este identificador sólo modifica documentación.

La matriz final de fuentes congeladas terminó con salida 0 para x86, x64 y
ARM64, Release `/MT` y `/analyze`, sin advertencias observadas. Se leyeron
independientemente las tres DLL; MZ/PE válidos y máquinas correspondientes:

| DLL de matriz | Bytes | Máquina PE | SHA-256 |
|---|---:|---|---|
| `.build/win32/Release/NppLdapFormatter.dll` | 189952 | 014c | `ae9f730045079cabdbe37c50410c912655db9210c680253207585abdb66c48e8` |
| `.build/x64/Release/NppLdapFormatter.dll` | 247808 | 8664 | `9f1d1a8f87dc2199be34bb047982ac36059774f42ecc5f1306120ba9a846b653` |
| `.build/arm64/Release/NppLdapFormatter.dll` | 217600 | aa64 | `1e937142d663dcaca32bb8709df0eb53302c9f5d7fdb0ad1370fdbf8ef807cd8` |

El CTest 7/7 y la tabla de fallos corresponden específicamente a `build-x64`,
cuya identidad se muestra arriba; no se atribuyen a la otra DLL de `.build/x64`.
ARM64 es sólo compilación cruzada, sin ejecutar su host ni sus EXE.

## Alcance de la recuperación

`ScintillaUndoGuard` cierra el grupo propio antes de solicitar `SCI_UNDO`.
El gateway comprueba identidad de vista/buffer/documento e historial antes de
recuperar. Rechaza un grupo Undo ya abierto, exige profundidad uno antes del
reemplazo y cero tras cerrar, y comprueba el estado de END antes de consultar
identidad. La profundidad corresponde a
[SCI_GETUNDOSEQUENCE](https://scintilla.org/ScintillaDoc.html#SCI_GETUNDOSEQUENCE),
presente en los headers fijados. Un error anterior a la edición restaura la selección sin deshacer
una edición independiente. Después de un borrado parcial o reemplazo con error,
se admite una única recuperación del grupo propio y se verifican los bytes
originales, longitud, anchor y caret. La orientación inversa se conserva.

Una excepción interna se simula dentro del host y se convierte en estado de
error antes de volver por el callback Win32. No se lanzan excepciones a través
de `SendMessageW`. No se promete recuperación de una excepción SEH, terminación
del proceso o fallo del propio `SCI_UNDO`.

El resultado continúa siendo `CommitIndeterminate`: se bloquean nuevos comandos
de formato en esa sesión. La recuperación no vuelve a ejecutar REPLACESEL ni
borra el historial. Un Undo puede dejar la acción fallida disponible en Redo;
por ello texto y selección recuperados no equivalen a historial idéntico ni a
preservación global aprobada. Los casos probados de cambio de identidad, mutación
en preflight y cambio de historial o grupo al cerrar rechazan Undo automático.
Las ediciones reentrantes de terceros dentro de REPLACESEL no están verificadas:
falta instrumentar esa frontera en el host nativo antes de certificar propiedad
exclusiva de las acciones nuevas. Este límite queda registrado en el ledger BMad
y conserva el bloqueo de release.

## Matriz de aceptación

| Criterio | Evidencia reproducible | Resultado y límite |
|---|---|---|
| AC-06: Undo/Redo atómico y ámbito intacto | `tests/PluginDllE2ETests.cpp`, DLL de producción, ambas vistas, Pretty/Compact, tres EOL, selección directa/inversa/documento y ambos órdenes de callback | Verificación simulada; `native_callbacks` y `native_undo_selection` pendientes de HITL. |
| Fallo interno de REPLACESEL | Casos fijos 0..2 y regresiones de borrado parcial/excepción interna; comparación exacta de texto, anchor, caret y edición anterior | Recuperación del buffer y selección exigida; historial Redo residual mantiene gate global bloqueado. |
| AC-07: NFR-002 y ausencia de edición durante análisis | [Benchmark 4.1](benchmarks-ui.md), [datos](benchmarks-ui-results.json), pruebas de cancelación/worker | Evidencia histórica: Compact 1 MiB p95 commit 80.5948 ms y 5 MiB 350.9664 ms incumplen <50 ms. Windows 10 difiere de Windows 11 de referencia; SSD sin verificar y RAM visible inferior a 16 GiB. No se atribuyen esas mediciones al binario modificado en 4.4. Repetición nativa requerida. |
| AC-08: x86/x64/ARM64 | Matriz final de esta entrega, [límites previos](package-host-validation.md), [CI](ci.md) | Compilación de las tres pasó. CTest x64 7/7; x86 5/7, ABI/E2E Not Run por EXE ausentes. ARM64 sólo cruzado y GitHub sin ejecutar. Gate global bloqueado. |
| AC-09: UTF-8 inválido/truncado/no ASCII sin corrupción | Suites core/gateway/controller y 18 rechazos de DLL E2E | Verificación automatizada x64; incluye límites, NUL, escapes y selección válida en documento >5 MiB. No sustituye gates nativos. |
| AC-10: sin tráfico ni persistencia de contenido | [Auditoría 4.3](package-privacy-audit.md), [datos](package-privacy-audit-results.json) | Fuentes/imports y regresiones históricas; no constituyen observación dinámica completa. Inspección de ejecución con atribución al plugin pendiente. El cambio 4.4 sólo añade mensajes Scintilla y estado en memoria. |

## Reproducción en CMD

Desde la raíz con los SDK fijados y CMake 3.31.6 disponible en PATH:

```cmd
cmake --build build-x64 --config Release
ctest --test-dir build-x64 -C Release --output-on-failure
uv --cache-dir .uv-cache run python -m unittest discover -s tests -p test_*.py
```

El patrón Python se pasa sin comillas: en este host las comillas de `-p` llegan
literales al programa y producen cero pruebas. Eso no cuenta como verificación.
En la continuación autorizada se repitieron build x64, CTest 7/7, harness
118 casos sin fallos y Python 98/98. Python necesitó ejecutarse fuera del
sandbox: dentro, los fixtures en `%TEMP%` fallaron por permisos; ese intento
no se considera aprobado. La lectura independiente volvió a confirmar UTF-8
estricto de los documentos y las cinco identidades PE de las tablas anteriores.
Si CMake no está en PATH, usar el ejecutable instalado de 3.31.6 o `uvx` con
`UV_CACHE_DIR=.uv-cache`, tal como describe [la matriz](ci.md).

CTest x86 final en `.build/win32`: **5/7**, salida **8**. Parser, gateway,
controller y las dos suites del benchmark pasan. `plugin_abi_smoke` y
`plugin_dll_e2e_tests` quedan **Not Run**: sus EXE no existen en disco tras el
build, aunque MSBuild informa esas rutas como generadas. La lectura independiente
confirma su ausencia; no se atribuye causa ni se declara ABI x86 aprobada.

## HITL para Notepad++ nativo

Estado: **pendiente; release bloqueada**. Usar Notepad++ 8.9.8.1 y Scintilla 5.6.6
con DLL de arquitectura coincidente, perfil portátil aislado y fixtures sintéticos
únicamente en RAM. Registrar versión/SO/arquitectura, revisión y SHA-256/tamaño PE
de host, DLL y harness. Los hashes corresponden a binarios, nunca al documento.

1. En ambas vistas, probar Pretty y Compact con selección directa, inversa,
   documento completo y cursor al inicio/medio/final. Conservar una edición
   independiente anterior y un documento inactivo. Tras dos formatos distintos,
   repetir Ctrl+Z/Ctrl+Z/Ctrl+Y/Ctrl+Y y comprobar texto, selStart/selEnd,
   anchor/caret, exterior y vista inactiva en cada paso.
2. Observar el orden efectivo de notificaciones de contenedor y último paso;
   verificar restauración después de que Scintilla termine de mover el cursor.
   No certificar orden nativo a partir del orden simulado.
3. En una instancia de prueba instrumentada, provocar error antes de editar,
   después del borrado interno y después de insertar. Repetir la excepción interna
   capturada por Scintilla. Verificar identidad e historial, un Undo propio tras
   borrar, cero Undo de la edición anterior, selección exacta y bloqueo posterior.
   Medir por separado igualdad del historial y presencia de Redo residual; cualquier
   pérdida o imposibilidad de instrumentación mantiene el gate fallido/pendiente.
4. Repetir en x86 y ARM64 nativo. Ejecutar ABI/E2E propios y CI GitHub; registrar
   resultados sin promover una arquitectura no ejecutada.
5. Repetir benchmark de referencia y observación dinámica de red/persistencia
   con atribución al plugin; no guardar contenido de fixtures, trazas con filtros,
   DN, credenciales ni capturas del documento. Publicar sólo enums, contadores,
   igualdades y metadatos binarios.

Aceptación humana: no realizada en 4.4. Ninguna fila pendiente equivale a passed.
