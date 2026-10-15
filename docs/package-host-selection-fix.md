# Corrección HITL 4.2: Pretty seguido de Compact

La captura de una selección rechazaba el retorno correcto de `SCI_GETSELTEXT`.
El [código oficial del baseline 8.9.8.1](https://github.com/notepad-plus-plus/notepad-plus-plus/blob/v8.9.8.1/scintilla/src/Editor.cxx#L6406)
devuelve bytes sin terminador tanto al consultar capacidad como al copiar. El
gateway exigía un byte adicional; ambos hosts simulados repetían ese contrato
incorrecto. Se corrigieron los conteos y se conservó la reserva `size + 1`, la
validación del NUL, UTF-8, rangos, identidad y estado de Scintilla.

El reemplazo ya normalizaba explícitamente la selección con `SCI_SETSEL` sobre
todo el resultado, conservando su orientación. Esa conducta permanece: Compact
puede consumir directamente el resultado completo de Pretty. Una selección
incompleta sigue siendo un error sintáctico; no se amplía silenciosamente al
documento entero.

## Verificación del 2026-10-14

- Los mocks conformes al API reprodujeron 22 fallos antes del arreglo del gateway.
- Build Release x64 con MSVC 19.44.35229.0 y runtime `MultiThreaded` (`/MT`).
- CTest x64: 7/7. Python: 80/80.
- Regresión C++: documento sin selección, selección completa y rango con texto
  externo en ambas orientaciones; Pretty → Pretty → Compact → Compact conserva
  UTF-8, CRLF, rango íntegro y texto externo. Las repeticiones no reemplazan ni
  abren una operación Undo. Selección incompleta produce error sintáctico sin
  editar. Conteos incorrectos de consulta/copia y NUL inválido siguen rechazándose.
- Comprobación automatizada real en Notepad++ 8.9.8.1 x64 aislado: carga física,
  menús, oráculos básicos y Undo API pasaron. Un probe adicional ejecutó Pretty →
  Compact → Compact sin reiniciar selección ni deshacer entre comandos, comprobó
  rango completo y retorno real de `SCI_GETSELTEXT`; pasó. El probe local está en
  `.uv-cache/probe_selection_fix.py`; no es aceptación humana ni suite CI.
- Evidencia local: `.build/host-validation-4-2/selection-fix/native-smoke.json`
  y `consecutive.json`. No se guardó contenido del documento ni su hash.

## Binario para repetir HITL

La DLL de `build-x64/Release/NppLdapFormatter.dll` se copió y contrastó byte a byte
en una copia portátil nueva, sin sobrescribir una DLL cargada ni los paquetes
históricos. Tamaño: **244736 bytes**. SHA-256:
`fe312554cfffb2f6b1d382b4e16bd48dab5980cd6c3fb14c5510c79ed4bf40ad`.

Abrir desde CMD esta instancia de prueba:

```cmd
C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x64-atn_ig78\notepad++.exe -multiInst -nosession -settingsDir=C:\DevIgnacio\npp-ldap-formatter\.build\host-validation-4-2\runs\package-x64-atn_ig78
```

La DLL está bajo `plugins/NppLdapFormatter/NppLdapFormatter.dll` dentro de esa copia.
En un documento nuevo con datos sintéticos, ejecutar Pretty, después Compact sin
tocar la selección y repetir Compact; comprobar el resultado y Ctrl+Z físico.
Descartar el documento de prueba al cerrar. Registrar sólo resultados de la
observación, sin datos privados.

La [matriz anterior](package-host-validation-results.json) corresponde a los
binarios anteriores y se conserva como evidencia histórica. Este arreglo sólo
se compiló y ejecutó en x64; no promueve x86 ni ARM64. El ticket 4.2 continúa
`in-progress`, con Ctrl+Z humano y host ARM64 nativo pendientes; los bloqueos ABI
x86 y rendimiento Compact de 4.1 permanecen documentados.
