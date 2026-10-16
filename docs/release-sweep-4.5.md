# Sweep de distribución — ticket 4.5

Validación local del 2026-10-15, Windows CMD, rama
`refactor/release-assurance-4.5`, base `6ac335b`.

## Resultado

Se fija la caché del uvx interno en la raíz real del repositorio y se actualizan
los comandos reproducibles. Los scripts existentes ya comparten validadores
de paquete y hash; se conservan sin duplicarlos. Sin cambios en API pública,
parser o formato de distribución.

| Verificación | Evidencia |
| --- | --- |
| Regresión de caché | Falló antes del arreglo; pasa después |
| Build Release x64 | Completado; runtime MultiThreaded (/MT) |
| CTest Release x64 | 7/7, cero fallos |
| E2E DLL completa | 130 casos, cero fallos de aserción |
| Python unittest | 99 pruebas, cero fallos |
| ZIP x64 nuevo | DLL única en raíz, PE x64, igualdad con build y SHA-256 verificados |
| Revisiones | Ciega y Ponytail independientes, sin hallazgos accionables |

Paquete local: `.build/release-sweep-4.5/NppLdapFormatter-0.1.0-x64.zip`,
124424 bytes. SHA-256:
`881f7fdd2332dd64ba955bf7cda597bc6bcc42eeeeae5ae576a0f509382feb99`.
Se reabren ZIP y DLL con lectura independiente, además del validador de proyecto.
Los tests cubren fixtures de las tres arquitecturas y rechazan DLL anidada,
miembros extra, checksum/nombre alterado y discrepancia con DLL original.
Este paquete nuevo acredita solo x64; no acredita una nueva matriz de builds.

## Alcance y límites

La documentación de host recoge tolerancia estructural multilinea, preservación
de valores, diagnósticos amigables Release/técnicos Debug y aceptación HITL
acotada de [4.4](host-gates-evidence-4.4.md). `project-context/AGENTS.md` se
sincronizó localmente; está ignorado por Git y no forma parte del commit.

El harness sigue informando `preservation_gate=failed`, seis observaciones de
pérdida de integridad, `native_gates=pending` y `release_blocked=true`. Las
aserciones verdes verifican también la detección de esas limitaciones; no
convierten el gate de preservación en aprobado.

Permanecen pendientes ABI/E2E x86 por ejecutables ausentes, runner ARM64 nativo,
matriz nativa completa de callbacks/Undo/Redo/fallos, CI remota y completar
rendimiento y privacidad. Este sweep no autoriza una release ni sustituye
evidencia histórica. Véanse [host E2E](host-e2e.md), [CI](ci.md) y
[revisión Ponytail](ponytail-review-4.5.md).
