# Validación de paquetes en hosts aislados (4.2)

## Aceptación HITL y decisión de cierre — 2026-10-14

El usuario confirmó la verificación humana interactiva en Notepad++ real después
del arreglo `f43e2b6664a2158ae729903fbb6b7e0bb754c531`, sobre el contexto de la copia
x64 8.9.8.1 preparada para HITL. La normalización de selección funciona y la
ejecución consecutiva Pretty/Compact no presenta cuadros de error modal:

| Ejercicio humano | Resultado confirmado |
|---|---|
| Pretty → Ctrl+Z | Restaura el estado original en exactamente un paso atómico |
| Pretty → Compact | Transforma correctamente el filtro completo |
| Pretty → Compact → Ctrl+Z | Restaura el estado Pretty anterior en exactamente un paso atómico |

La confirmación procede del usuario; no se deduce del Undo por API del runner.
No se registró contenido del documento. DLL x64 verificada: SHA-256
`fe312554cfffb2f6b1d382b4e16bd48dab5980cd6c3fb14c5510c79ed4bf40ad`.

**Decisión explícita del usuario:** cerrar 4.2 con HITL satisfactorio y diferir
formalmente la ejecución física ARM64 al futuro runner ARM64 del flujo de CI de
GitHub Actions. Esta decisión no certifica ejecución ARM64 ni habilita release.
La evidencia x86 previa sigue siendo WOW64; no se declara nueva aceptación humana
x86 ni se cierra la limitación ABI de 2.7.

### ARM64: certificación estática y ejecución diferida

Se recompiló ARM64 desde `f43e2b6`, Release con MSVC v143 y runtime estático
`MultiThreaded` (`/MT`), comprobado en el proyecto generado. Lectura independiente
del binario: DOS `MZ`, firma `PE`, arquitectura `0xAA64`, PE32+ y flag DLL válidos.
`link.exe /dump /exports` confirmó los seis exports oficiales: `beNotified`,
`getFuncsArray`, `getName`, `isUnicode`, `messageProc` y `setInfo`.

| Artefacto ARM64 actualizado | Evidencia |
|---|---|
| DLL | 214528 bytes |
| SHA-256 DLL | `d43293e17ffecda2e4a00a704717cdd0d397b08a38b99d28c4dd6610c9d57523` |
| SHA-256 ZIP | `ef95812e84436fe6e71f67bf71616b3e1e8d6caafc7593d4305196527cafe9a1` |

Paquete local nuevo: `.build/host-validation-4-2/arm64-static-final/`; checksum
adyacente y contenido ZIP contrastados con la DLL compilada. Registro estático:
`static-evidence.json` en ese directorio. No se ejecutó ARM64 de forma nativa ni
emulada. La compilación cruzada y los exports no prueban inicialización ni Undo.

El workflow actual `.github/workflows/ci.yml` usa `windows-2022` x64 para la matriz;
su variante ARM64 sólo compila y empaqueta de forma cruzada. Queda diferido
incorporar/provisionar un runner Windows ARM64 nativo y ejecutar allí el host
Notepad++ ARM64 8.9.8.1 con el paquete correspondiente, conservando URL/run ID,
hashes y evidencia de carga, menús, Pretty/Compact y Undo. No hay ejecución remota
ni runner nativo ya certificado en esta evidencia. Los demás gates de release,
privacidad y rendimiento Compact de 4.1 conservan su estado independiente.

## Procedimiento y evidencia histórica de preparación

Para repetir HITL x64 tras el incidente Pretty → Compact, usar la
[DLL y copia portátil corregidas](package-host-selection-fix.md). La matriz
de esta guía se conserva como evidencia de los binarios anteriores.

La evidencia inicial usa ZIP de distribución y Notepad++ oficial 8.9.8.1 de la misma
arquitectura. El smoke comprueba módulo cargado, ruta física, dos comandos reales,
salidas básicas literales y Undo por API. La aceptación visual de Ctrl+Z sigue
pendiente en los reportes automáticos históricos. La aceptación posterior y la
decisión de cierre están registradas arriba; el release continúa bloqueado.

## Preparación reproducible

Ejecutar desde la raíz en CMD. Compilar Release /MT con la matriz documentada en
`docs/ci.md`, crear un directorio nuevo por paquete con `scripts/package_plugin.py`
y validar ZIP, SHA-256 y DLL con `scripts/validate_packages.py`. No sustituir el ZIP
por una DLL ni usar el host simulado como evidencia nativa.

El orquestador guarda insumos y resultados bajo `.build/host-validation-4-2/`.
Cada arquitectura necesita su ZIP, checksum adyacente `.zip.sha256`, ZIP oficial de
Notepad++ y JSON de build con estos siete campos exactos:

```json
{
  "configuration": "Release",
  "architecture": "x64",
  "runtime": "MultiThreaded",
  "dll_sha256": "<SHA-256 de la DLL de producción dentro del ZIP>",
  "compiler": "19.44.35229.0",
  "source_revision": "<revisión Git de 40 dígitos hexadecimales minúsculos>",
  "source_digest": "<SHA-256 del índice de código de producto descrito abajo>"
}
```

La configuración, runtime y compilador se obtienen de los proyectos generados y
del registro del build ejecutado; el hash se calcula leyendo la DLL empaquetada.
El PE no demuestra por sí solo `/MT`. No copiar metadatos de la DLL del benchmark
como si describieran el plugin de producción. El runner exige coincidencia exacta
entre arquitectura, hash de la DLL y metadatos. Conserva además los tamaños,
arquitecturas PE, hashes de ejecutable/ZIP/DLL y versión del host en el resultado.

`source_revision` identifica el commit de referencia. `source_digest` identifica
el código de producto realmente usado, incluidas modificaciones locales. Se
reúnen, sin recursión adicional, `include/*.hpp`, `src/*.cpp`, `src/*.hpp`,
`src/*.def`, `CMakeLists.txt`, `CMakePresets.json` y `cmake/Baseline.cmake`. Ordenar
por ruta relativa POSIX; en los bytes de cada archivo normalizar CRLF a LF y
calcular SHA-256. Construir el índice con una línea por archivo:
`ruta/relativa:sha256hex` seguida de `\n`. El SHA-256 de ese índice UTF-8 es
`source_digest`. Ambos campos forman parte de los metadatos exactos vinculados al
SHA-256 de la DLL; un baseline y un booleano de modificaciones no los sustituyen.
Este algoritmo sólo procesa código/configuración del producto, jamás documentos
del editor. No requiere instrumentar ni recompilar el binario.

El runner fija los siguientes SHA-256 de los ZIP oficiales 8.9.8.1; un parámetro
`--host-sha256` diferente se rechaza aunque coincida con un archivo local:

| Arquitectura | SHA-256 ZIP oficial |
|---|---|
| x86 | `65d3435b5dcbefde47c401a2666132138e76a2b62e189ad4ffe568581b6adfd2` |
| x64 | `beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414` |
| ARM64 | `43bf78c7569fd43b03d4f86a1073ac4306b31099e4487bdbf536c42d16d03f48` |

Ejemplo (sustituir las rutas por los artefactos verificados):

```cmd
uv --cache-dir .uv-cache run python scripts/validate_host_packages.py --arch x64 --package .build/host-validation-4-2/packages/x64/NppLdapFormatter-0.1.0-x64.zip --host-archive .build/host-validation-4-2/hosts/x64/npp.8.9.8.1.portable.x64.zip --host-sha256 beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414 --build-metadata .build/host-validation-4-2/build-metadata/x64.json --output .build/host-validation-4-2/results/x64-new-run.json
```

Repetir con x86. Sobre Windows AMD64 se registra `WOW64` para x86 y `native` para
x64. ARM64 sólo se ejecuta cuando `GetNativeSystemInfo` identifica ARM64; en AMD64
se prepara el material, se registra `cross-only` y todos los resultados de
ejecución permanecen `pending`. Un build cruzado nunca satisface el host ARM64.

## Qué comprueba el smoke

Cada ejecución extrae a un directorio nuevo únicamente `notepad++.exe`, los dos
modelos XML oficiales y la DLL del paquete bajo `plugins/NppLdapFormatter/`.
Genera su propio perfil portátil con sesiones, backup, snapshots y actualizaciones
desactivados. No copia configuración, sesiones ni plugins de otra instalación.

La búsqueda de ventana se limita al PID del `Popen` creado. Toolhelp con
`SNAPMODULE | SNAPMODULE32` verifica la ruta del módulo cargado y su hash.
`GetMenu`/`GetMenuItemInfoW` recuperan los IDs de `Format LDAP (Pretty)` y
`Format LDAP (Compact)`. Todos los handles y resultados usan tipos del tamaño de
puntero. Cada llamada IPC tiene timeout y el escenario tiene un deadline global.
Un watchdog, creado después del `Popen`, retiene exclusivamente su handle: al
vencer el plazo termina ese proceso propio. Así también se limita una llamada
síncrona `EnableWindow` bloqueada por un host que no responde. Se cancela y une el
watchdog antes de finalizar la limpieza. Esta cota depende de que Windows permita
terminar el proceso: si falla esa operación no puede garantizarse el retorno de
una API síncrona bloqueada. Cuando retorna el control, el fallo de terminación se
registra en limpieza; no se intenta terminar otro proceso.

Tras descubrir la ventana, el smoke espera dentro del deadline a que exista su
editor con foco (`GetGUIThreadInfo`), captura su HWND e identidad y bloquea la
entrada humana únicamente en esa ventana mediante `EnableWindow(False)`.
Revalida inmediatamente la identidad original y `IsWindowEnabled`; sólo entonces
continúa con módulo, menús y Nuevo. Nuevo debe producir BufferID y puntero de
documento distintos. Mientras la ventana está bloqueada, valida PID, parentesco,
HWND, BufferID, documento y que la entrada siga bloqueada antes de cada operación,
sin depender del foco perdido al deshabilitarla. Un cambio aborta antes de leer o
escribir el documento diferente. Usa sólo ASCII sintético propio en RAM,
`WM_CHAR`, mensajes escalares y lecturas acotadas `SCI_GETLENGTH/SCI_GETCHARAT`.
Nunca pasa punteros de buffers locales a `SCI_GETTEXT/SCI_SETTEXT` de otro proceso,
no usa memoria remota y no accede al clipboard.

Esta instancia dedicada excluye automatización externa concurrente. Deshabilitar
la ventana impide cambios humanos de pestaña, pero no convierte las comprobaciones
y mensajes separados en una transacción atómica entre procesos ni impide que otro
programa envíe sus propios mensajes Win32. La preparación HITL no aplica este
bloqueo: sus copias permanecen interactivas y separadas del smoke automático.

El documento anterior nunca se reutiliza, aunque estuviera vacío. Las rutas
resueltas de resultados y temporales deben quedar
dentro del workspace; antes de cada borrado recursivo se revalida el directorio
propio y su padre para rechazar escapes por enlaces. El compilador aceptado es
exclusivamente la familia MSVC 19.44 del baseline.

Los oráculos son literales independientes: Pretty con LF y dos espacios, y
Compact sin espacios de formato. Ambos parten del mismo documento sintético y
sin selección. Vacía el historial antes de cada comando y comprueba que una sola
llamada `SCI_UNDO` restaure los bytes originales y agote ese historial. El JSON
registra `pretty_undo_api` y `compact_undo_api`; nunca convierte esos resultados en
aceptación humana de Ctrl+Z. No imprime ni guarda el documento o su hash.

Después del setup de selección e historial, el runner deja 100 ms dentro del
deadline para que el host procese notificaciones diferidas de Scintilla; después
revalida la identidad antes del comando. El primer intento x64 sin esa oportunidad
de idle cargó el módulo y los menús, pero agotó el plazo en Pretty. El reintento
con los mismos binarios pasó ambos oráculos y Undo por API. Es compatible con la
cancelación por `SCN_UPDATEUI` pendiente; no constituye una prueba instrumentada
de su causa. El resultado fallido inicial se conserva junto al reintento.

El proceso automático termina únicamente mediante el handle del `Popen` que creó.
Espera su salida y verifica la eliminación del directorio propio. Un fallo de
limpieza queda explícito; no intenta terminar procesos por nombre ni por un PID
descubierto. Las excepciones se reducen a enums, nunca a mensajes con contenido.
Si falla el lanzamiento, el reporte registra `launch: failed` y
`process_cleanup: not-needed`. La limpieza conserva el error primario. Si falla
la eliminación del directorio, se conserva la evidencia y el árbol propio y no se
reintenta ese borrado desde `finally`.

Para ejecutar el smoke x64/ARM64 se requiere un intérprete Python de 64 bits; un
Python de 32 bits se rechaza antes de lanzar el host y muestra esa indicación.
`--prepare-hitl` y la preparación `cross-only` siguen disponibles sin ejecutar ni
enumerar módulos de un proceso de 64 bits.

## Ejercicio humano visible

Añadir `--prepare-hitl` al comando crea una copia portátil nueva y muestra el
comando exacto para abrirla visiblemente. No inicia el proceso ni escribe una
aceptación humana. Conservar únicamente ese directorio propio para el ejercicio.
La persona debe usar exclusivamente datos sintéticos de este ejemplo:

1. Ejecutar el comando impreso y confirmar que la ventana pertenece a la copia
   portátil indicada. Abrir un documento nuevo y escribir `( & (cn=a) (sn=b) )`.
2. Sin selección, ejecutar Plugins → LDAP Formatter → `Format LDAP (Pretty)`.
   Confirmar el árbol con dos espacios por nivel y los dos atributos en líneas
   separadas. Pulsar físicamente Ctrl+Z una vez y comprobar visualmente que vuelve
   exactamente la entrada sintética.
3. En el mismo documento restaurado, ejecutar `Format LDAP (Compact)` y confirmar
   `(&(cn=a)(sn=b))`. Pulsar físicamente Ctrl+Z una vez y confirmar la restauración.
4. Registrar por arquitectura únicamente resultado enumerado de cada ejercicio,
   fecha y versión/binarios asociados a la evidencia. No guardar capturas con
   datos privados, contenido del documento, hashes de documentos ni clipboard.
   Una discrepancia se registra como fallo; no se aprueba por inferencia.
5. Cerrar la instancia propia, descartando el documento sintético sin guardarlo.
   Eliminar sólo el directorio portátil impreso después de verificar que el
   proceso propio terminó. No usar una instalación personal para completar pasos.

El JSON del runner conserva siempre `human_ctrl_z: pending`. La observación humana
se añade como evidencia separada revisada por el orquestador; el script no ofrece
un flag para simularla ni para cerrar el ticket.

Ya hay copias preparadas para el ejercicio, independientes de los temporales
automáticos que se eliminaron. Abrir sólo una instancia cada vez desde CMD:

```cmd
C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x64-rwj0uoiw\notepad++.exe -multiInst -nosession -settingsDir=C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x64-rwj0uoiw
C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x86-23d44hru\notepad++.exe -multiInst -nosession -settingsDir=C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x86-23d44hru
```

Por cada arquitectura, la persona debe completar este registro separado sin
marcarlo por haber leído el resultado automático:

- [ ] Confirmar copia portátil propia, versión 8.9.8.1 y ambos menús exactos.
- [ ] Pretty produce la estructura esperada y un Ctrl+Z físico restaura la entrada.
- [ ] Compact produce la salida esperada y un Ctrl+Z físico restaura la entrada.
- [ ] Registrar enums de resultado, arquitectura, fecha y referencia binaria;
  cerrar la instancia y descartar el documento sintético sin guardarlo.

Para ARM64, transferir el ZIP del paquete y su checksum, el ZIP oficial ARM64 y
el JSON del build a una máquina Windows ARM64 nativa. Repetir allí preparación,
smoke y ejercicio humano con el mismo script. El directorio preparado en AMD64
también contiene la copia portátil ARM64, pero prepararlo no constituye ejecución.
La copia ARM64 preparada es
`.build/host-validation-4-2/runs/package-ARM64-ozl8bgrq/`. No abrirla en este host
AMD64 como evidencia de ejecución ARM64.

## Matriz y límites de la evidencia

La [matriz observada](package-host-validation-results.json) conserva resultados
del 2026-10-14 en Windows 10 build 19044 AMD64, plugin 0.1.0 y host 8.9.8.1.
El contenedor usa schema 1 y sus filas actuales son ejecuciones nuevas schema 2.
Los reportes anteriores permanecen históricos; no se convirtieron ni se
renombraron como observaciones revisadas. El orquestador validó cada fila con el
validador correspondiente y comprobó por lectura
independiente SHA-256/PE y la igualdad entre DLL en disco y dentro del ZIP.
El JSON contiene tamaños, versiones, hashes ZIP/DLL/host, rutas de módulos cargados,
IDs de menú y metadatos Release /MT con MSVC 19.44.35229.0. La consolidación usa
los siete campos de build, contrasta `source_digest` con los archivos de producto
y vincula los metadatos al hash de la DLL. Los nuevos smoke x86/x64 comprobaron
`launch` e `input_block`, además de los seis checks de la tabla, con watchdog.

| Arquitectura | Build Release /MT | Ejecución | Carga/menús/Pretty/Compact/ambos Undo API | Limpieza automática | Ctrl+Z humano |
|---|---|---|---|---|---|
| x64 | pasó | native | 6/6 pasaron | proceso y directorio: pasó | pendiente |
| x86 | pasó | WOW64 | 6/6 pasaron | proceso y directorio: pasó | pendiente |
| ARM64 | build cruzado pasó | cross-only | pendiente de host ARM64 nativo | no ejecutada | pendiente |

Los menús observados x86/x64 tienen IDs 22000 y 22001. Los resultados actuales
proceden de `.build/host-validation-4-2/results/x64-final-reviewed.json`,
`results/x86-final-reviewed.json` y `results/ARM64-reviewed.json`. El primer fallo
se conserva en `results/x64.json` y el primer reintento exitoso en
`results/x64-retry.json`. También se conserva `results/x64-reviewed.json`, cuyo
fallo en la captura inicial ocurrió antes de que estuviera listo el editor; la
espera acotada de disponibilidad propia permitió el reintento
`results/x64-reviewed-retry.json`, antes de los smoke finales con watchdog. La pausa
de idle no cambió binarios, oráculos ni deadline; la causa del fallo inicial sigue
siendo una inferencia, como se explica arriba.

| Paquete | SHA-256 del ZIP validado |
|---|---|
| x86 | `f942a65786d2f99acf7751495a49a033b827a63fb64171ce67c9b92700515adb` |
| x64 | `c91bb11a862b24f792dbc2603f46858d719aed84aecbb3987e337b36fddf35a1` |
| ARM64 | `ac91f959523a6b26a399696d7364646c1eb8a2ac9470ec0b6d01fcaf924723d6` |

La matriz de compilación de las tres arquitecturas y el build `build-x64`
terminaron con código 0. CTest x64 pasó 7/7 y la suite Python final pasó 80/80.
CTest x86 terminó con código 8: pasaron 5/7; `plugin_abi_smoke` y
`plugin_dll_e2e_tests` quedaron `Not Run` porque faltaban sus ejecutables en disco,
aunque MSBuild había informado su generación. No se atribuye una causa sin
evidencia y la ABI x86 de 2.7 permanece abierta. La carga real bajo WOW64 aporta
evidencia de este smoke, pero no sustituye esas pruebas pendientes.

Cada reporte nuevo usa schema 2. `execution: prepared` exige todos los pasos y
limpiezas pendientes; incluye HITL y `cross-only`. `execution: attempted` registra
los pasos en orden `launch`, `input_block`, `load`, `menus`, `pretty`,
`pretty_undo_api`, `compact`, `compact_undo_api`: un fallo deja los posteriores
pendientes y necesita un error explícito. Los enums mal tipados o contradictorios
se rechazan sin traceback en CLI. Un resultado cruzado no puede afirmar limpieza
de un proceso que nunca se ejecutó.

Cada JSON admite exclusivamente campos enumerados, conteos y metadatos binarios,
incluida ruta física del módulo y perfil propio. Su validador rechaza campos extra,
IDs inválidos, arquitecturas contradictorias y promociones humanas/de release:

```cmd
uv --cache-dir .uv-cache run python scripts/validate_host_packages.py --validate .build/host-validation-4-2/results/x64-final-reviewed.json
uv --cache-dir .uv-cache run python -m unittest discover -s tests -p test_validate_host_packages.py
```

Esta evidencia básica no promueve los gates avanzados de callbacks, preservación,
fallo después del borrado o privacidad completa documentados en `docs/host-e2e.md`.
Las discrepancias de rendimiento Compact de 4.1 siguen abiertas. No afirma CI
remota, host ARM64 ni aceptación HITL sin sus ejecuciones y observaciones reales.
