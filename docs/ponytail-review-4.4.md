# Revisión Ponytail 4.4

What this change does: Recupera bytes y selección tras un reemplazo fallido
cuando las comprobaciones de identidad y grupo lo permiten, conserva el
diagnóstico y bloquea posteriores formatos. Rechaza grupos Undo externos,
comprueba el error de cierre y registra los cambios reales de historial de los
tokens de contenedor.

La revisión independiente inicial y su pasada incremental no encontraron
defectos nuevos de código ni capas innecesarias. Se revisaron diff completo,
consumidores del commit (controller y benchmark), notificaciones/diagnóstico,
guard, simulador, pruebas y consumidores del esquema de evidencia. No hay README
en este workspace. Carga asumida: un plugin en hilo UI, ámbitos de hasta 5 MiB y
presupuesto propio existente de 32 MiB.

La revisión BMad paralela encontró cuatro correcciones confirmadas: estado de
END borrado por consulta de identidad, falta de rechazo de grupo externo,
ausencia de marcadores en el historial simulado y reporte de recuperación sin
Undo. Las cuatro se corrigieron con regresiones. La documentación también se
acotó para no extender garantías a ediciones reentrantes de terceros no probadas;
ese gap se registra como medium no verificado en el ledger BMad y bloquea release.

La raíz comprobó después el build Release final sin advertencias, CTest 7/7,
Python 98/98 y 118 casos de DLL E2E sin fallos de aserción. Actualizó las seis
filas medidas y verificó por lectura independiente PE/tamaño/SHA de DLL y harness
en [la evidencia consolidada](host-gates-evidence-4.4.md). La revisión no ejecutó
builds concurrentes ni se atribuye esas ejecuciones.

Verdict: Ship para integrar este cambio. La preservación integral del historial
sigue fallida; release bloqueada.

La revisión independiente final de la continuación autorizada no encontró
defectos nuevos. Contrastó el diff, consumidores de gateway/controller,
historial simulado, pruebas y validación de evidencia; no ejecutó tests.
La raíz repitió build x64, CTest 7/7, harness 118/0 y Python 98/98.

Not checked: host nativo ante fallo interno, orden real de callbacks, edición
reentrante de terceros, runtime x86/ARM64, CI GitHub, nuevo rendimiento y traza
dinámica de privacidad. La matriz de evidencia distingue los builds de esas
verificaciones pendientes.
