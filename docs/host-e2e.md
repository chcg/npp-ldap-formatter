# E2E de la DLL y puertas de host

La suite `plugin_dll_e2e_tests` carga la DLL Release de producción con
`LoadLibraryW`, resuelve sus seis exportaciones y ejecuta Pretty/Compact mediante
`getFuncsArray`. Enlaza únicamente los headers fijados y user32: no contiene
copias del núcleo, gateway, controlador ni PluginDefinition. Sus ventanas Win32
modelan dos documentos, selección independiente y un historial propio por vista.
Los resultados salen del worker real y se entregan bombeando mensajes con un
deadline de cinco segundos. Cada sesión envía SHUTDOWN, espera la quiescencia del
producto y drena mensajes antes de destruir ventanas o descargar la DLL.

Los casos de `NPPN_BUFFERACTIVATED`, `NPPN_FILEBEFORECLOSE` y `SCN_UPDATEUI`
capturan el contador de mensajes antes de solicitar el trabajo y bombean hasta
despachar un nuevo `WM_APP+37`, que el worker publica también al cancelar.
Exigen exactamente una entrega dentro del deadline: un worker que nunca termina
hace fallar la prueba. Después de despachar la entrega y drenar mensajes,
comprueban ausencia de reemplazo y aviso, texto, selección e historial exactos
en ambas vistas antes del SHUTDOWN del destructor de sesión. El caso de UPDATEUI
conserva la selección nueva del usuario; activación y cierre conservan la previa.
SHUTDOWN se verifica en un caso separado, sin usarlo como barrera de descarte de
los otros eventos. No se usan sleeps para sincronizar.

La prueba reforzada detectó un aviso `DocumentChanged` obsoleto tras UPDATEUI.
El controlador ahora invalida trabajo y diagnóstico al recibir un cambio de
selección de la vista activa, conservando los cambios programáticos durante
aplicación y restauración. La regresión unitaria usa una barrera del worker y
comprueba también que las actualizaciones sin selección y desde otra vista no
cancelen el trabajo vigente. Tras la corrección, la verificación Release x64 del
2026-10-14 pasa CTest 5/5 y Python 36/36 sin skips; los gates nativos siguen
pendientes y la puerta simulada de preservación sigue fallida.

El harness modifica en RAM exclusivamente el slot IAT de MessageBoxW de la DLL
cargada. Verifica firmas DOS/PE, arquitectura implícita del proceso, directorio de
imports, límites y memoria comprometida; guarda el puntero y restaura protección
y función mediante RAII. La función interceptada conserva WINAPI, recoge dueño,
título, flags y diagnóstico y devuelve IDOK sin modal. No hay exportaciones ni
hooks de pruebas añadidos al producto. Si el import no existe o la instrumentación
no es viable, la suite falla explícitamente.

## Ejecución reproducible

Usar CMD nativo desde la raíz. El checkout SDK debe corresponder al commit de
`cmake/Baseline.cmake`, con MSVC 14.44.35207 y SDK Windows 10.0.26100.0.
Los presets de build incluyen el mismo target para x86, x64 y ARM64.

```text
cmake --build --preset x64-release
ctest --preset x64-release
uv --cache-dir .uv-cache run python -m unittest discover -s tests
uv --cache-dir .uv-cache run python scripts/host_evidence.py --harness .build/x64/Release/plugin_dll_e2e_tests.exe --dll .build/x64/Release/NppLdapFormatter.dll --revision <commit-de-40-hex> --output .build/host-evidence.json
uv --cache-dir .uv-cache run python scripts/host_evidence.py --validate .build/host-evidence.json
```

Verificación local del 2026-10-14: `cmake --build build-x64 --config Release`
y `ctest --test-dir build-x64 -C Release --output-on-failure` pasaron con `/MT`
y `/analyze`. Python `unittest discover -s tests -p test_*.py` pasó 36/36,
sin skips. La suite E2E ejecutó 103 casos con cero fallos de aserción.
Lectura independiente de `build-x64/Release/NppLdapFormatter.dll`: 244736 bytes,
MZ/PE x64, SHA-256 `d67f48f166221be95c8ad75004b73b0c27a1d5f541793ef596a1e26d068a0eeb`.
Harness: 313344 bytes, MZ/PE x64, SHA-256
`cfa7de5d7900075b0914d8f864f550a7149f49fa9a6db810db98ffa49f4d5745`.
Los hashes identifican únicamente binarios. Las fuentes verificadas son las del
commit que incorpora esta evidencia; el reporte local se genera después del commit
con su identificador completo. Contención pasó, preservación simulada falló
en las seis inyecciones y las cuatro puertas nativas siguen `pending`.

El runner relee MZ/PE, tamaño, arquitectura y SHA-256 del binario. Solo acepta la
salida JSON de esquema estricto: enums, contadores e igualdades. Nunca persiste ni
reenvía stderr/stdout arbitrarios. El SHA-256 identifica exclusivamente la DLL;
están prohibidos texto, filtros, DN, credenciales y hashes del documento. Los
campos desconocidos, versiones de esquema que no sean enteros, puertas faltantes o afirmaciones de certificación nativa
desde una simulación son errores. La salida no puede ser la DLL o el harness,
incluidos enlaces al mismo archivo: se comprueba antes de ejecutar y de escribir.
La puerta de preservación se deriva de las igualdades medidas; cero pérdidas no
autoriza release ni promueve puertas nativas.

## Cobertura y significado de las puertas

La matriz normal combina Pretty/Compact, ambas vistas, selección directa/inversa
o documento completo, tres EOL y ambos órdenes del callback de contenedor. Sus
oráculos son literales independientes del producto. Comprueba un reemplazo,
BEGIN/END balanceados, exterior intacto, selección y texto exactos en Undo/Redo,
además de conservar un paso de edición independiente anterior. Los rechazos
cubren UTF-8/NUL, sintaxis, escapes, límites de entrada/salida de 5 MiB y
profundidad 128, read-only, Undo
desactivado, selección múltiple/rectangular/virtual, codepage y fallos de API.
Una selección válida puede formatearse en un documento superior a 5 MiB.
La cuota propia de 32 MiB permanece vigente en el producto y está cubierta por
las pruebas existentes de núcleo/gateway con agotamiento explícito del budget.
La DLL no expone ese budget: esta suite no afirma haberlo agotado mediante las
exportaciones públicas ni altera el producto para hacerlo accesible.

Las inyecciones se identifican por un índice fijo, sin contenido documental:

| Índice | Inyección |
|---|---|
| 0 | REPLACESEL falla antes de modificar bytes |
| 1 | REPLACESEL modifica y después indica error |
| 2 | REPLACESEL borra internamente y después falla |
| 3 | Callback de cierre de grupo cambia selección |
| 4 | Callback de token cambia identidad documental |
| 5 | Callback de selección cambia contenido y falla la compensación |

Para cada inyección se registra igualdad real de texto, selección e historial,
reemplazos y llamadas Undo. Desde 4.4, los casos 1 y 2 permiten una única llamada
Undo controlada del grupo propio tras comprobar identidad e historial, y exigen
texto y selección restaurados. El caso 0 restaura selección sin deshacer una
edición anterior. Los casos 3 a 5 no autorizan Undo automático. Contención exige
bloqueo posterior, máximo un intento, ningún Undo ciego y balance del grupo.
La recuperación puede dejar una acción Redo: no equivale a historial idéntico.
Los resultados del 2026-10-14 anteriores son históricos; la evidencia actual está
en [host-gates-evidence-4.4.md](host-gates-evidence-4.4.md).
Contención aprobada no equivale a
preservación: cualquier desigualdad deja `simulated_preservation=failed` y release
bloqueado. Incluso un fallo temprano puede conservar bytes y perder selección.

## Protocolo nativo pendiente

Un CTest verde certifica solo el proceso sintético. El host fijado es Notepad++
8.9.8.1 con Scintilla 5.6.6. Hasta disponer de ese ejecutable y evidencia verificable,
`native_callbacks`, `native_undo_selection`, `native_failure_preservation` y
`native_arm64` permanecen `pending`. La épica no puede declararse íntegramente
cerrada ni autorizar release mientras estas puertas estén pendientes o fallidas.

En una máquina de referencia aislada, registrar SO/arquitectura, versión exacta
del host, commit y tamaño/hash de la DLL. Usar fixtures sintéticos exclusivamente
en RAM. Cargar la DLL de la arquitectura correspondiente y repetir la matriz de
formatos, EOL, ámbitos y dos vistas con una edición independiente anterior.
Registrar orden y último paso de las notificaciones de contenedor, igualdad de
texto/selección/historial antes y después de un Undo y Redo, y ausencia de cambios
en la otra vista. Repetir activación, cierre y selección mientras el worker corre.

Para preservación ante fallo interno, instrumentar exclusivamente una instancia
de prueba del host para provocar error antes y después de sus efectos. Registrar
las tres igualdades, número de reemplazos, bloqueo y número de Undo automáticos;
para el fallo de reemplazo propio exigir exactamente uno tras el borrado y cero
cuando no hay una acción de contenido propia que deshacer. Comprobar que la
recuperación nunca deshace una edición independiente anterior;
no sustituir evidencia por suposiciones de restauración. Si se pierde integridad,
marcar puerta `failed`, mantener release bloqueado y elevar la discrepancia para
resolver el requisito. Adjuntar solo metadatos y resultados; no capturas o logs
con documentos. Repetir en ARM64 nativo. El siguiente hito es esa ejecución y la
resolución explícita de las discrepancias de preservación observadas.
