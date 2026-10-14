# Benchmark nativo de UI

Story 4.1 mide el gateway y core de producción dentro de Notepad++ portátil
8.9.8.1 x64. El resultado versionable es
[`benchmarks-ui-results.json`](benchmarks-ui-results.json). Esta evidencia no
promueve NFR-002 de referencia ni los demás gates de release.

## Resultados finales del 14 de octubre de 2026

La matriz final del binario corregido completó **seis escenarios y 210
ejecuciones válidas**, con cinco calentamientos y treinta muestras medidas por
escenario. El JSON de esquema 2 tiene **73607 bytes** y conserva todas las
observaciones. Cada p95 usa únicamente sus treinta muestras medidas.

Captura cumple **p95 <50 ms** en los seis escenarios. Commit Compact incumple en
1 MiB (**80.5948 ms**) y 5 MiB (**350.9664 ms**). Se publican ambas discrepancias,
sin relajar la meta ni promover NFR-002 de referencia o release.

Equipo medido: Windows 10 x64 10.0.19044, Intel Core i7-6700T @ 2.80 GHz, cuatro
núcleos físicos y ocho procesadores lógicos. RAM total visible para Windows
(`GlobalMemoryStatusEx`): **17067855872 bytes**, ligeramente inferior a 16 GiB;
no se equipara a RAM instalada. El almacenamiento SSD queda sin verificar.
Notepad++ es 8.9.8.1; Scintilla 5.6.6 se atribuye a la procedencia descrita abajo.

| Entrada bytes | Modo | Salida bytes | p95 captura ms | p95 formato ms | p95 commit ms | p95 total ms | Commit <50 ms |
|---:|---|---:|---:|---:|---:|---:|---|
| 262144 | Pretty | 165269 | 1.4445 | 5.1594 | 11.7437 | 18.9636 | Sí |
| 262144 | Compact | 89143 | 1.3818 | 3.6893 | 28.4955 | 31.8677 | Sí |
| 1048576 | Pretty | 661061 | 3.7347 | 20.5563 | 16.7089 | 39.5970 | Sí |
| 1048576 | Compact | 356575 | 3.6407 | 15.0237 | **80.5948** | 91.4617 | **No** |
| 5242880 | Pretty | 3304949 | 12.5293 | 71.9263 | 43.7031 | 125.9911 | Sí |
| 5242880 | Compact | 1782279 | 16.0405 | 69.2838 | **350.9664** | 420.5079 | **No** |

| Entrada bytes | Modo | Core pico bytes | WS base bytes | WS final bytes | WS pico bytes | WS delta bytes | Probes/ticks UI medidos | Gap UI máximo ms |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 262144 | Pretty | 1020598 | 30564352 | 47968256 | 52764672 | 22200320 | 29 | 5.6415 |
| 262144 | Compact | 792218 | 30638080 | 48476160 | 51404800 | 20766720 | 30 | 4.8568 |
| 1048576 | Pretty | 4080838 | 30609408 | 50266112 | 68636672 | 38027264 | 37 | 13.6068 |
| 1048576 | Compact | 3167370 | 30674944 | 57450496 | 64557056 | 33882112 | 34 | 14.5835 |
| 5242880 | Pretty | 20401110 | 30552064 | 63758336 | 148242432 | 117690368 | 111 | 56.8243 |
| 5242880 | Compact | 15833098 | 30711808 | 111067136 | 138797056 | 108085248 | 112 | 48.4446 |

El pico propio máximo fue **20401110 bytes**, dentro del límite de 33554432
bytes. WS absoluto/delta incluye host y Scintilla y no está sujeto al gate de
32 MiB del contador propio. Los seis escenarios registraron mensajes UI con
worker activo; los conteos agregan las treinta muestras y no garantizan un tick
en cada ejecución ni ausencia universal de bloqueo.

La DLL medida fue **`build-x64/Release/NativeUiBenchmark.dll`**, Release x64 /MT,
con MSVC **19.44.35229.0**, `_MSC_FULL_VER` **194435229**, toolset
**14.44.35207** y Windows SDK **10.0.26100.0**. Se verificaron independientemente
MZ/PE x64, tamaños, hashes, binding del manifiesto, compilador, digest de fuentes,
conteos, memoria y p95:

- Host: **8546288 bytes**, SHA-256
  `c52c9ed2ec6d0b5b3f243b29f2124f98db56b6ba92bb42a9af2665709464b3cf`.
- DLL medida: **331264 bytes**, SHA-256
  `9fea5ff191c7506d61e50eddc3b12861867b7bc20daba27fe28642f9049e1e81`.
- Revisión registrada durante build:
  `2fd497676d3630c3df575e093c01d6b7b48eb73a`, con cambios locales.
- Digest de fuentes permitido por la build:
  `dc97ed3b6acf0c456d9b5916662fc1dcda800eab91f2b416cc10f106c0a0554a`.

La pasada inicial incompleta permanece en `.build/benchmarks-ui-first-pass.json`
y la pasada completa anterior a las correcciones en
`.build/benchmarks-ui-final-before-safety.json` (73254 bytes), como historial
local de metadatos numéricos. Sus mediciones no se atribuyen a la DLL final.

## Reproducir

Requiere CMD nativo de Windows AMD64, Git, uv, Visual Studio 2022 con toolset
v143 14.44.35207 y Windows SDK 10.0.26100.0. La build comprueba esas versiones.
Un Windows ARM64 ejecutando procesos x64 emulados se rechaza mediante
`GetNativeSystemInfo`; la arquitectura PE x64 se verifica por separado.

En un checkout limpio, preparar los headers oficiales fijados y descargar el ZIP
desde los [assets oficiales de Notepad++ 8.9.8.1](https://github.com/notepad-plus-plus/notepad-plus-plus/releases/tag/v8.9.8.1):

```cmd
if not exist .build mkdir .build
git clone --depth 1 --branch v8.9.8.1 https://github.com/notepad-plus-plus/notepad-plus-plus.git .build\notepad-plus-plus
git -C .build\notepad-plus-plus rev-parse HEAD
mkdir .build\benchmark-host-8.9.8.1
curl.exe --fail --location --output .build\benchmark-host-8.9.8.1\npp.8.9.8.1.portable.x64.zip https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.9.8.1/npp.8.9.8.1.portable.x64.zip
certutil -hashfile .build\benchmark-host-8.9.8.1\npp.8.9.8.1.portable.x64.zip SHA256
```

Confirmar que HEAD es `dd40fe45598791e9aa38f3aeb200e10fb3d6338a` y que SHA-256
es exactamente
`beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414`.
Detenerse ante cualquier diferencia. Solo después, extraer en una carpeta nueva:

```cmd
mkdir .build\benchmark-host-8.9.8.1\portable
tar.exe -xf .build\benchmark-host-8.9.8.1\npp.8.9.8.1.portable.x64.zip -C .build\benchmark-host-8.9.8.1\portable
```

Descubrir/invocar CMake **3.31.6** por su versión, sin fijar directorios internos
de caché. `UV_CACHE_DIR` mantiene toda resolución de uv/uvx en el workspace:

```cmd
set UV_CACHE_DIR=.uv-cache
uvx --from cmake==3.31.6 cmake --version
uvx --from cmake==3.31.6 cmake --preset x64 -DNPP_LDAP_NATIVE_UI_BENCHMARK=ON
uvx --from cmake==3.31.6 cmake --build .build\x64 --config Release
uv --cache-dir .uv-cache run python scripts\benchmark_ui.py
uv --cache-dir .uv-cache run python scripts\benchmark_ui.py --validate docs\benchmarks-ui-results.json
```

Si CMake ya está disponible en PATH, `where cmake` y `cmake --version` permiten
usar ese ejecutable cuando su versión sea 3.31.6. El runner acepta `--dll` y
`--build-metadata` para otra carpeta CMake equivalente;
ambos deben proceder de la misma build Release x64 /MT. La DLL rechaza compilar
en Debug, con CRT dinámico o para otra arquitectura. El target opcional no se
incluye en paquetes del plugin.

El runner verifica el ZIP oficial local
`.build/benchmark-host-8.9.8.1/npp.8.9.8.1.portable.x64.zip`, SHA-256
`beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414`,
la versión PE del host, arquitectura y correspondencia del EXE con el ZIP.
Verifica el manifiesto generado después de enlazar la DLL: contiene su SHA-256,
compilador exacto (`_MSC_FULL_VER`), toolset, SDK, revisión de headers y CRT. El
hash se comprueba antes de lanzar el host y sobre la copia aislada; el compilador
nativo observado debe coincidir exactamente. Revisión Git y estado modificado
proceden de esa build, sin tomar el HEAD del momento de ejecución. `source_digest`
es SHA-256 de un índice ordenado `ruta-relativa:SHA256-del-archivo` de fuentes
C++/headers y configuración de build permitidos explícitamente por
`cmake/BenchmarkMetadata.cmake`; nunca incluye documentos ni datos del editor.
El informe conserva identidad PE y SHA-256 del host y la DLL. Scintilla 5.6.6 se atribuye al SDK fijado
y la procedencia oficial del host; no existe una consulta de versión runtime
Scintilla usada por este protocolo.

## Protocolo

Se crea una carpeta portátil y un proceso nuevos por cada combinación de
262144, 1048576 y 5242880 bytes con Pretty y Compact. Solo se copian el EXE,
modelos de estilos/lenguajes y la DLL de benchmark. Marcadores de configuración
local y actualizador desactivado, `-multiInst`, `-nosession` y `-settingsDir`
aislado evitan consumir sesiones/configuración del usuario. `config.xml`
desactiva recuperación, snapshots, backups, actualizaciones y nombres de pestaña
derivados del contenido. No se copia ni ejecuta el actualizador ni otros plugins.

La DLL crea un documento nuevo tras NPPN_READY y difiere las iteraciones a una
ventana de mensajes en el hilo UI. El fixture RFC4515 tiene tamaño exacto,
Unicode, escapes hexadecimales, presencia, substring, matching rule extensible,
AND/OR/NOT y comparaciones. La separación estructural permite que ambas salidas
sean diferentes y menores que la entrada, incluido el límite de 5 MiB.

Cada escenario ejecuta cinco calentamientos y treinta muestras. Tras instalar
el fixture, un timer UI de baja prioridad espera al menos 100 ms antes de
capturar para permitir que termine el repaint/idle del setup. Esa pausa no entra
en ninguna fase. No se descartan muestras por latencia ni se repiten hasta
obtener un gate favorable. La primera pasada exploratoria, previa a separar
setup/repaint, quedó incompleta y no constituye el informe final.

Las fases usan QueryPerformanceCounter:

- **capture_ms:** `ScintillaGateway::capture` en UI, incluida copia, identidad,
  validación UTF-8 y reserva del snapshot.
- **format_ms:** `formatFilter` en un worker sobre el snapshot inmutable, con
  presupuesto compartido y reserva de entrada de producción. El worker no llama
  Scintilla; solo publica mensajes de coordinación al hilo UI. El campo
  `worker_scintilla_calls: 0` declara esa ruta verificada en código, sin añadir
  sondas a las llamadas calientes del producto.
- **commit_ms:** `ScintillaGateway::commit` en UI, incluida revalidación,
  validación de salida y un reemplazo agrupado para Undo.
- **total_ms:** tiempo transcurrido entre inicio de captura y fin de commit,
  incluyendo despacho y esperas UI del worker. No es la suma de percentiles.

El oracle compara todos los bytes leídos tras el commit con la salida preparada.
Setup, oracle, destrucción del resultado y limpieza quedan fuera de esas fases.
Cada p95 toma exclusivamente las últimas treinta ejecuciones: nearest-rank,
posición `ceil(0.95 * 30) = 29` en orden ascendente. Se conserva cada muestra,
incluidos calentamientos, con conteos separados. Captura y commit exigen cada
uno **p95 < 50 ms**; un valor igual o mayor se publica como discrepancia.

## Memoria y respuesta UI

`core_peak_bytes` es el máximo exacto de adquisiciones del contador compartido
CoreBudget, incluyendo su baseline conservador, snapshot, almacenamiento core y
reservas transitorias del commit. La adquisición atómica registra el high-water
antes de devolver; liberar o rechazar una adquisición no lo reduce. Cada
ejecución tiene presupuesto nuevo y el escenario publica el máximo de las 35.
El límite es **33554432 bytes**; entrada y salida no exceden **5242880 bytes**.
Este contador mide lo que contabiliza CoreBudget, no toda la memoria del proceso
ni las asignaciones auxiliares del harness/Scintilla.

Working Set base se obtiene antes del primer setup, con el documento nuevo vacío.
Working Set final se obtiene después de liberar snapshot, resultado y presupuesto
de la última ejecución y limpiar el documento sintético. La identidad retenida
(buffer, vista y documento) se revalida antes de cada escritura; un cambio de
pestaña aborta sin escribir sobre la nueva. Longitud cero, estado Scintilla y
savepoint se verifican antes de publicar un resultado exitoso. El pico es
`PeakWorkingSetSize` del sistema operativo durante toda la vida del proceso,
incluyendo arranque, host, Scintilla, DLL y harness. Delta es **pico absoluto menos
base**, no un pico de memoria propia ni un requisito de 32 MiB. El pico puede
incluir actividad anterior al baseline; las tres cifras absolutas se conservan.

Un timer UI solicita mensajes cada 10 ms y el worker publica también un probe
inicial. Solo cuentan mensajes procesados mientras el flag de formato está
activo. `ui_max_gap_ms` es el máximo intervalo observado desde el fin de captura
hasta atención de probe/timer o recogida de finalización; incluye el retraso de
despacho inicial/final. El timer tiene granularidad Windows y baja prioridad;
no establece un límite universal de bloqueo. Cero ticks significa respuesta UI
no observada durante ese intervalo, especialmente si el formato termina rápido;
se publica como discrepancia, sin esperar artificialmente dentro del formato.

El runner termina únicamente los handles de procesos que creó y elimina sus
carpetas temporales. Un timeout, salida temprana o resultado inválido produce
escenario incompleto, diagnóstico enumerado y, si existe, etapa/código/conteo
numéricos. Nunca sintetiza muestras. Los únicos datos persistidos por el harness
son tiempos, conteos, tamaños y metadatos; no se guardan filtros, valores, DN,
credenciales, documentos ni hashes del contenido sintético.

## Alcance

La referencia exige Windows 11 x64, al menos cuatro núcleos físicos, 16 GiB y SSD.
El equipo medido usa Windows 10: esta diferencia impide promover la evidencia de
referencia. El tipo de almacenamiento queda sin verificar. La RAM física se
publica en bytes y cualquier valor inferior a 16 GiB se señala explícitamente.
Compilar x64 o ejecutar este benchmark no certifica hosts x86/ARM64, rollback,
selección Undo/Redo, privacidad completa ni el resto de gates de release.

## Verificación

Las pruebas de fixtures cubren los tres tamaños exactos, UTF-8, límites y
equivalencia canónica de ambos modos. Las pruebas core cubren pico inicial,
adquisiciones concurrentes con máximo conocido, límite inclusivo, rechazos y
liberación. Las pruebas Python cubren esquema estricto, treinta muestras,
nearest-rank sin calentamientos, valores no finitos, versiones, memoria,
duraciones, gates fallidos, resultados incompletos, rutas protegidas, éxito del
runner, timeout y limpieza de su propio proceso. En este entorno las pruebas
Python necesitan salir del sandbox para acceder a los directorios temporales.

La verificación final del orquestador compiló `build-x64` Release sin errores y
ejecutó CTest **7/7** y Python **51/51**. La matriz nativa posterior se ejecutó
con fuentes congeladas y sin comprobaciones CPU paralelas. Esta actualización
documental transcribe el JSON final; no ejecutó otra matriz ni modificó código.
