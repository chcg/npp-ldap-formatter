# Dossier final de aseguramiento y decisión de release — 4.6

Fecha: **2026-10-15**. Baseline de esta comprobación:
`00f972d60105b1b6acd7f70ead18c89f371a89d4`.
Rama documental: `feat/release-decision-4.6`. No se modifica código, API pública,
versiones soportadas ni configuración de distribución.

## Dictamen formal HITL

**Release Candidate (Staging) Aprobado para pruebas manuales / Bloqueado para PR oficial a nppPluginList hasta certificación en CI.**

Se registra el dictamen solicitado expresamente por el usuario al iniciar 4.6.
Autoriza pruebas manuales controladas con fixtures sintéticos y arquitectura
coincidente; no autoriza publicar paquetes, enviar un PR ni distribuir como
release estable. No se atribuye una nueva ejecución humana a este ticket.

El candidato de staging identificado es **solo x64**: el ZIP local de 4.5,
`.build/release-sweep-4.5/NppLdapFormatter-0.1.0-x64.zip`, SHA-256
`881f7fdd2332dd64ba955bf7cda597bc6bcc42eeeeae5ae576a0f509382feb99`.
Los paquetes históricos x86/ARM64 se incluyen como evidencia, sin aprobación
de staging actual para esas arquitecturas.

La certificación CI es necesaria, pero **no suficiente** para levantar el
bloqueo: también deben resolverse los fallos de preservación, rendimiento y
las verificaciones nativas y de privacidad enumeradas aquí. El cierre de 4.6
certifica el dossier y la decisión, no la aprobación de todos los gates ni el
cierre de la iniciativa. El ticket 2.7 conserva su bloqueo ABI x86.

## Matriz de evidencia de las épicas 1–4

| Épica | Entrega y evidencia localizable | Resultado y frontera |
| --- | --- | --- |
| 1 — Plataforma | [Baseline](build-baseline.md), [CI y paquetes](ci.md), `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml` | C++20, SDK fijado, MSVC v143, runtime estático Release y targets separados. Compilación histórica x86/x64/ARM64 verificada en [4.4](host-gates-evidence-4.4.md#resultados-medidos). CI configurada no equivale a CI ejecutada; ARM64 es compilación cruzada. |
| 2 — Core RFC4515 | `tests/LdapParserTests.cpp`, `include/LdapParser.hpp`, [alcance actual](host-e2e.md#comportamiento-actual-y-alcance-de-la-aceptación) | Igualdad, comparaciones, presencia/substring, extensibles, grupos, escapes, UTF-8, idempotencia, cancelación y límites cubiertos en suites x64. Entrada/salida hasta 5 MiB, profundidad 128, presupuesto compartido 32 MiB; excesos rechazados. 2.7 permanece en review por ABI x86 ausente. |
| 3 — Integración Scintilla | [Host E2E](host-e2e.md), `ScintillaGatewayTests`, `PluginControllerTests`, `PluginDllE2ETests`, [recuperación 4.4](host-gates-evidence-4.4.md#alcance-de-la-recuperación) | Snapshot y UTF-8 antes de editar, worker sin llamadas Scintilla, revalidación y un reemplazo agrupado. E2E actual: 130 casos, cero fallos de aserción; preservación global fallida, gates nativos pendientes. |
| 4 — Aseguramiento | [4.1 rendimiento](benchmarks-ui.md), [4.2 hosts](package-host-validation.md), [4.3 privacidad](package-privacy-audit.md), [4.4 gates/HITL](host-gates-evidence-4.4.md), [4.5 sweep](release-sweep-4.5.md) | Evidencia versionada con identidades y límites. Hay aprobación humana parcial del caso multilinea y Undo; no certificación integral de release. Este dossier reúne las decisiones sin sustituir informes originales. |

## Rendimiento y memoria — 4.1

Fuente: [informe completo](benchmarks-ui.md#resultados-finales-del-14-de-octubre-de-2026)
y [muestras JSON](benchmarks-ui-results.json), seis escenarios, 210 ejecuciones
válidas (cinco calentamientos y treinta muestras medidas por escenario).

| Ámbito | Modo | p95 captura ms | p95 commit ms | Gate commit <50 ms |
| --- | --- | ---: | ---: | --- |
| 256 KiB | Pretty | 1.4445 | 11.7437 | Cumple |
| 256 KiB | Compact | 1.3818 | 28.4955 | Cumple |
| 1 MiB | Pretty | 3.7347 | 16.7089 | Cumple |
| 1 MiB | Compact | 3.6407 | **80.5948 ≈ 80.6** | **Incumple** |
| 5 MiB | Pretty | 12.5293 | 43.7031 | Cumple |
| 5 MiB | Compact | 16.0405 | **350.9664 ≈ 351** | **Incumple** |

Pico propio máximo: **20 401 110 bytes**, es decir **20.4 MB decimales ≈19.46 MiB**,
inferior a **33 554 432 bytes =32 MiB**. La cifra informal «20.4 MiB» no es la
conversión correcta. Es el contador CoreBudget, no toda la memoria del proceso.
El Working Set pico del host llegó a 148 242 432 bytes; no se compara con ese
límite de memoria propia.

Equipo histórico: Windows 10 10.0.19044, i7-6700T, cuatro núcleos físicos,
17 067 855 872 bytes de RAM visible, SSD sin verificar. No satisface la referencia
Windows 11/16 GiB/SSD. DLL medida `NativeUiBenchmark.dll`, SHA-256
`9fea5ff191c7506d61e50eddc3b12861867b7bc20daba27fe28642f9049e1e81`;
no es la DLL de producto actual. No se volvió a medir rendimiento en 4.6.
NFR-002 y AC-07 permanecen sin aprobar.

## Hosts, selecciones y aceptación humana — 4.2 y 4.4

[4.2](package-host-validation.md) conserva la validación histórica de paquetes,
sus discrepancias y protocolo; [4.4](host-gates-evidence-4.4.md) separa
recuperación simulada y aceptación humana posterior.

El HITL de 4.2, registrado el 2026-10-14 tras
`f43e2b6664a2158ae729903fbb6b7e0bb754c531`, corresponde a Notepad++ 8.9.8.1
x64: Pretty→Ctrl+Z restauró el original en un paso; Pretty→Compact transformó
el filtro completo sin modal y un Ctrl+Z posterior restauró Pretty en un paso.
La DLL histórica tenía 244736 bytes, SHA-256
`fe312554cfffb2f6b1d382b4e16bd48dab5980cd6c3fb14c5510c79ed4bf40ad`;
véase [identidad para HITL](package-host-selection-fix.md). No es el candidato
actual, ni acredita una nueva aceptación x86 o ARM64.

El usuario confirmó en Notepad++ nativo, tras
`054dec0f551496d53c1ffca4f7eb163a6ba5071b`, que una aserción simple multilinea
se formateó sin modal, Compact produjo una línea continua y Ctrl+Z revirtió todo
en **un paso atómico**, conservando selección y caret según lo reportado.
Es evidencia humana comunicada; no se aportaron versión, SO, arquitectura ni
SHA-256 de la DLL de esa prueba. No se asignan a ella identidades de otro build.

Se tolera whitespace estructural tras `(` y entre atributo y operadores simples
`=`, `~=`, `>=`, `<=`. Solo se recortan bordes del valor si el tramo inicial
contiene CR/LF. Se preservan espacios internos, escapes y bordes genuinos de
valores de una línea. Pretty de una raíz simple y Compact eliminan ese layout
periférico; las aserciones extensibles y el parser legado mantienen su alcance.
Release presenta mensajes amigables y accionables; Debug conserva enums y
offsets técnicos, disponibles internamente en ambas configuraciones.

La normalización y el Undo reportados no acreditan Redo consecutivo, ambas
vistas/orientaciones, orden de callbacks ni fallos internos nativos. El protocolo
completo sigue en [HITL pendiente](host-gates-evidence-4.4.md#hitl-para-notepad-nativo).

### Integridad ante fallos simulados

Ejecución actual: `cases=130`, `failures=0`, `containment_cases=6`,
`integrity_loss_cases=6`, **`preservation_gate=failed`**,
**`native_gates=pending`**, **`release_blocked=true`**.

| Caso fijo | Texto igual | Selección igual | Historial igual | SCI_UNDO |
| --- | --- | --- | --- | ---: |
| Error previo a editar | Sí | Sí | No | 0 |
| Reemplazo completo con error | Sí | Sí | No | 1 |
| Borrado interno con error | Sí | Sí | No | 1 |
| Callback END cambia selección | No | No | No | 0 |
| Callback token pierde identidad | Sí | No | No | 0 |
| Callback selección cambia bytes | No | Sí | Sí | 0 |

El rollback propio restaura bytes y selección tras borrado interno, pero deja
Redo residual y puede perder Redo anterior al insertar el token. Ante pérdida
de identidad o cambios reentrantes se evita Undo ciego. La recuperación puede
terminar `CommitIndeterminate` y bloquear nuevos comandos en la sesión. No se
promete recuperación ante fallo de SCI_UNDO, SEH o terminación del proceso.
Las aserciones verdes comprueban también que estas pérdidas sean detectadas;
no convierten el gate de preservación en aprobado.

## Seguridad y privacidad — 4.3

Fuentes: [auditoría y límites](package-privacy-audit.md) e
[inventario PE/paquetes JSON](package-privacy-audit-results.json).
Estas conclusiones son históricas: revisión de congelación
`6b5dd197d41dd701558eaaf69690dc155b916a6f`, digest productivo
`0d66a888765dd738042eb4797919df6d0e772c6550316319b84190edab5325f8`.
No se repitió esa auditoría para el candidato x64 actual; confirmar su
aplicabilidad es parte de la certificación pendiente.
La revisión del código propio no encontró cliente LDAP remoto, sockets/HTTP,
telemetría, logs, escritura de filtros/DN/credenciales ni hashes de documentos.
Los imports PE auditados no contienen bibliotecas de red. Release usa `/MT`.

Esto **no certifica cero I/O dinámico**: los binarios importan `CreateFileW`,
`WriteFile`, `LoadLibraryExW` y `GetProcAddress` por infraestructura del runtime.
No se hallaron consumidores propios que les entreguen contenido del documento.
La observación dinámica con atribución de stacks al plugin sigue pendiente.
El host, otros plugins, antivirus, pagefile y volcados quedan fuera de esa
conclusión. Los scripts de empaquetado sí escriben artefactos; no son la ruta
de procesamiento del editor. AC-10 permanece pendiente, sin relajar la política
de cero telemetría y cero persistencia de contenido.

## Empaquetado e identidades — 4.3 y 4.5

Nombre: `NppLdapFormatter-0.1.0-<arquitectura>.zip`, con
`NppLdapFormatter.dll` como único miembro en la raíz y checksum `.zip.sha256`.
Los scripts validan PE/arquitectura, límites, identidad con DLL original y hash
del **ZIP completo**. Rechazan DLL anidada, extras, hash o nombre alterado.
Véanse [CI y referencia de Plugins Admin](ci.md#validación-local-desde-cmd).

Los siguientes hashes identifican los **snapshots históricos de 4.3**, no una
matriz actual ni paquetes aprobados para publicación:

| Arquitectura | SHA-256 ZIP histórico 4.3 |
| --- | --- |
| x86 | `072f4f92f9c33982484203d7906e871e27624b1ac4426d2250af19488426bb42` |
| x64 | `b64fcad825c2c1f00413d20d95be9e41483716cadcd19a1718caa952bf384292` |
| ARM64 | `a6e0695db4ebcbb656fc8b2d1698d61ebdf6684ac6f3f51377724a8aca36674c` |

La evidencia [4.5](release-sweep-4.5.md) verifica únicamente un ZIP x64 nuevo de
124424 bytes en `.build/release-sweep-4.5/`, SHA-256
`881f7fdd2332dd64ba955bf7cda597bc6bcc42eeeeae5ae576a0f509382feb99`.
Su DLL tiene 251392 bytes, PE x64 y SHA-256
`e4473c04d988577f0d055f1753e33b3166595712f99004183ef4a02e96f13a33`.
No se fabrican hashes actuales x86/ARM64 ni se reutilizan los del ZIP del host.
Antes de Plugins Admin deberá generarse una matriz de la revisión candidata,
probarla y registrar sus hashes definitivos y procedencia de CI.

## Trazabilidad de aceptación de release

| Criterio | Evidencia primaria | Dictamen |
| --- | --- | --- |
| AC-01 espacios/jerarquía | `tests/LdapParserTests.cpp`, suite parser actual | Verificado automatizado x64 |
| AC-02 anidamiento/balance | Misma suite, grupos/profundidad/errores | Verificado automatizado x64 |
| AC-03 escapes exactos | Misma suite, escapes y roundtrip | Verificado automatizado x64 |
| AC-04 error no destructivo | Parser, gateway/controller y rechazos E2E | Verificado x64 para errores de análisis; Release amigable, diagnóstico técnico interno. No implica preservación ante fallo de commit |
| AC-05 ámbito seleccionado/documento | Gateway/controller/E2E, [host](host-e2e.md) | Verificado simulado x64; matriz nativa completa pendiente |
| AC-06 Undo/Redo sin daños | [4.4](host-gates-evidence-4.4.md#matriz-de-aceptación), HITL parcial anterior | Undo humano de un caso aprobado; preservación global fallida y Redo/matriz nativa pendientes |
| AC-07 NFR-002/referencia | [4.1](benchmarks-ui.md) | No aprobado: Compact excede p95 y equipo distinto de referencia |
| AC-08 tres arquitecturas | [4.4](host-gates-evidence-4.4.md#resultados-medidos), [CI](ci.md) | No aprobado: x86 ABI/E2E ausentes, ARM64 nativo y CI pendientes |
| AC-09 UTF-8/entrada truncada | Core/gateway/controller/rechazos E2E | Verificado automatizado x64; no extiende certificación a otros hosts |
| AC-10 ejecución sin red/persistencia | [4.3](package-privacy-audit.md) | Fuentes/imports revisados; observación dinámica pendiente |

## Gates para levantar el bloqueo oficial

1. Resolver la pérdida de historial/Redo y las discrepancias de integridad;
   demostrar recuperación y propiedad de acciones bajo fallos y reentrancia.
2. Completar HITL nativo: vistas, orientaciones, selección/caret, dos Undo/Redo,
   callbacks e inyección instrumentada según protocolo 4.4.
3. Recuperar y ejecutar ABI/E2E x86; 2.7 sigue en review por
   `ABI_x86_executable_missing_after_clean_build`.
4. Ejecutar en CPU ARM64 nativa. Queda diferido a infraestructura futura de
   GitHub Actions con runner ARM64 apropiado; el workflow actual usa
   `windows-2022` y compilación cruzada, **no implementa esa ejecución nativa**.
5. Ejecutar CI real y conservar URL/run ID, revisión, resultados y artefactos.
   No hay ejecución remota acreditada en este dossier.
6. Corregir o resolver formalmente los incumplimientos p95 sin relajar el gate;
   repetir benchmark en el entorno de referencia sobre el candidato exacto.
7. Completar observación dinámica de privacidad atribuida al plugin; persistir
   únicamente metadatos no sensibles, nunca contenido del editor.

## Verificación de esta entrega

Comandos ejecutados desde CMD, usando el CMake 3.31.6 instalado en caché local
cuando no está en PATH:

```cmd
cmake --build build-x64 --config Release
ctest --test-dir build-x64 -C Release --output-on-failure
uv --cache-dir .uv-cache run python -m unittest discover -s tests -p test_*.py
```

Release x64 `/MT` compiló; **CTest 7/7**, **Python 99/99**, sin skips. El patrón
Python se pasa sin comillas por la peculiaridad de invocación de este entorno.
Python se ejecutó fuera del sandbox para permitir fixtures temporales.
La ejecución directa E2E cargó la DLL de producción: **130 casos, cero fallos
de aserción**, con los gates fallidos/pendientes declarados arriba.
No se repitieron benchmark, auditoría dinámica, HITL ni matriz nativa completa.

En esta sesión se invocaron los ejecutables reales
`.uv-cache\archive-v0\UhbCJPjRa_aKYAyY\Lib\site-packages\cmake\data\bin\cmake.exe`
y `ctest.exe` del mismo directorio con los argumentos del bloque anterior. Esa
ruta interna es un registro de ejecución, no una ruta portable: para reproducir,
resolver CMake 3.31.6 como indica [el baseline](build-baseline.md).
El comando E2E adicional fue:

```cmd
build-x64\Release\plugin_dll_e2e_tests.exe C:\DevIgnacio\npp-ldap-formatter\build-x64\Release\NppLdapFormatter.dll
```

Lectura independiente de los binarios efectivamente usados en esta ejecución:

| Archivo bajo `build-x64/Release/` | Bytes | SHA-256 |
| --- | ---: | --- |
| `NppLdapFormatter.dll` | 251392 | `e4473c04d988577f0d055f1753e33b3166595712f99004183ef4a02e96f13a33` |
| `plugin_dll_e2e_tests.exe` | 331776 | `adf1658e5b5edca3f5344c77fd3fa758f00399e5270c57536409426529403158` |

Ambos son PE x64 (`8664`). La DLL coincide con la identidad del candidato de
4.5; el reporte numérico de la ejecución directa se transcribe en la sección
de integridad de este dossier, sin contenido del documento.

Revisión Ponytail independiente: sin hallazgos accionables. La revisión ciega
pidió precisar candidato staging, binarios ejecutados, snapshot de privacidad
e invocación real; se incorporaron las cuatro precisiones. Se verificaron
UTF-8 estricto, encabezado, enlaces locales, anclas y hashes históricos contra
el JSON. Carga prevista de este documento: revisión humana local; sin nuevas
dependencias ni lógica ejecutable. Las revisiones no repitieron pruebas nativas.
