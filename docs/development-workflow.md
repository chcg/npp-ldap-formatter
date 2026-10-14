# Flujo de desarrollo: BMad, Graphify y Ponytail

Estado verificado el 2026-10-14 en Windows, desde CMD en la raíz del repositorio. Las garantías de `AGENTS.md` gobiernan todo el flujo: integridad del documento, UTF-8 estricto, Scintilla, RAII y equivalencia x86/x64/ARM64.

## Instalación ejecutada

| Comando | Resultado verificado |
| --- | --- |
| `uv --cache-dir .uv-cache tool install graphifyy` | Graphify 0.9.90 instalado; CLI `graphify.exe` y `graphify-mcp.exe`. |
| `graphify install --project --platform codex` | Skill y referencias en `.codex/skills/graphify/`; `.codex/hooks.json` y sección inicial en `AGENTS.md`. |
| `graphify install --project --platform agents` | Skill y referencias en `.agents/skills/graphify/`, ruta de descubrimiento actual de Codex. Confirmada mediante `codex debug prompt-input`. |
| `graphify extract . --code-only` | Grafo local construido con AST, sin extracción semántica ni llamadas a un LLM. |
| `graphify extract . --code-only --force` y `graphify update .` | Exclusiones finales aplicadas y actualización comprobada: 881 nodos, 1.489 aristas; sin cambios de topología en la segunda pasada. |
| `graphify cluster-only . --no-label --no-viz` | Reporte generado sin etiquetado por LLM ni visualización. |
| `graphify hook install` | Hooks `post-commit`, `post-checkout` y `post-merge`; merge driver registrado. Confirmados por `graphify hook status` y lectura independiente. |
| `uv --cache-dir .uv-cache tool update-shell` | Directorio de ejecutables de uv añadido al PATH del usuario. Reabrir la terminal para heredar el cambio. |
| `codex plugin marketplace add DietrichGebert/ponytail` | Marketplace `ponytail` registrado en el perfil del usuario. |
| `codex plugin add ponytail@ponytail` | Ponytail 5.2.0 instalado y habilitado, confirmado con `codex plugin list --marketplace ponytail --json`. |

Durante la instalación, `graphify` todavía no figuraba en el PATH de la sesión: se invocó su ejecutable en `%USERPROFILE%\.local\bin\graphify.exe`. La CLI y Ponytail residen en el perfil del usuario; sus instrucciones y la skill descubrible se comparten desde el proyecto. No se añadió una dependencia de producción al plugin LDAP.

El directorio `.codex/` completo, incluida la copia `.codex/skills/graphify/` y `.codex/hooks.json` del instalador, queda local e ignorado. `.agents/skills/graphify/` queda disponible para versionarse y para el descubrimiento documentado por Codex. El hook local `PreToolUse` es un no-op de compatibilidad: la regla Graphify primero procede de `AGENTS.md`, y la sincronización automática procede de los hooks de Git.

Para cargar el plugin en la aplicación, iniciar una sesión nueva o reiniciar Codex. El instalador de Ponytail indica revisar y confiar sus hooks mediante `/hooks`; su ejecución en esta sesión activa no se ha confirmado. Las reglas esenciales de Ponytail ya están en `AGENTS.md` y se aplican también sin esos hooks. Node está disponible y los tres scripts de hooks pasaron `node --check`.

## Grafo y alcance

- Extracción inicial: 32 archivos de código, 881 nodos, 1.489 aristas y 42 comunidades.
- Archivos locales: `graphify-out/graph.json`, `graphify-out/GRAPH_REPORT.md` y metadatos de extracción. No se versionan.
- Exclusiones: builds, binarios, cachés, `.agents/`, `.codex/`, `_bmad/`, `_bmad-output/`, documentación y contexto de planificación. Las fuentes registradas en los nodos no contienen directorios excluidos. Las exclusiones documentales también impiden que `update` añada nodos de metadatos de esos documentos.
- La extracción paralela encontró `[WinError 5]` en el sandbox y completó el trabajo secuencialmente.
- `cmake/sdk_probe.cpp`, `include/BuildInfo.hpp` y `src/PluginMessages.hpp` no produjeron símbolos. `Baseline.cmake` y `PluginExports.def` no fueron clasificados como código por esta versión. Los documentos se omiten deliberadamente con `--code-only`.
- El grafo es una ayuda de navegación. No representa completamente el despacho dinámico de mensajes Windows, ni sustituye inspección del código, contratos y pruebas de host.

Comprobaciones funcionales realizadas: `query Scintilla --budget 800`, `explain ScintillaGateway`, `path` con identificadores completos y `--undirected`, y `affected` a profundidad 1. La consulta amplia señaló truncamiento de presupuesto; se acotó la inspección con los otros comandos. La ruta verificada enlaza `PluginController` con `ScintillaGateway` mediante una referencia extraída. Los nombres ambiguos se resolvieron usando identificadores del grafo.

Para mantenerlo actualizado durante una implementación, ejecutar `graphify update .` después de editar código y antes de revisar el impacto. Los hooks posteriores a commit, checkout y merge complementan ese paso. Su instalación fue verificada; no se hizo un commit, checkout ni merge real para probar los eventos. Git administra su propio intérprete para los hooks; los comandos del agente se ejecutan desde CMD.

No se midió ahorro de tokens comparado con una sesión previa. La extracción inicial y el reporte no consumieron tokens de un LLM; las consultas limitan el contexto devuelto mediante `--budget`.

## Roles y precedencia

| Etapa | Flujo | Modelo disponible verificado | Criterio |
| --- | --- | --- | --- |
| Planificación y tickets | `bmad-ticket` / `tickets.py` | `gpt-6-luna` o `gpt-5.6-terra` | Operar el store BMad con sus scripts. |
| Construcción | `bmad-build` / Amelia | `gpt-6.1-sol` | Graphify localiza impacto; Ponytail guía el mínimo cambio robusto. |
| Revisión | `bmad-code-review`, con `$ponytail:ponytail-review` para minimalismo | Selección explícita según el alcance | Revisar diff y nodos circundantes; confirmar criterios y pruebas antes de `done`. |

Los identificadores se comprobaron con `codex debug models`. Las directivas no cambian el modelo de una sesión activa; debe seleccionarse al iniciar la sesión o delegar con soporte del host. Revalidar disponibilidad antes de seleccionarlo.

Regla del peldaño: eliminar lo no solicitado, reutilizar el proyecto, usar biblioteca estándar/API nativa, usar dependencias instaladas y añadir solo el código restante necesario. Mantener límites de confianza, accesibilidad, RAII y errores estrictos. Toda lógica crítica con ramificaciones o estado requiere una prueba unitaria concisa de comportamiento.

## BMad existente

La iniciativa `initiative-plugin-oficial-cpp-notepadpp-ldap` se comprobó mediante su manifiesto en `_bmad/`, el directorio en `_bmad-output/` y la configuración resuelta por `_bmad/scripts/resolve_config.py`. Su estado es activo. La lectura de tickets con ruta explícita devuelve 24 tickets: 16 terminados, uno en revisión, uno en progreso y seis planificados.

La comprobación `setup.py --status` reporta `current: false`, scripts compartidos desactualizados, registros de módulos ausentes y archivos heredados. No se migró ni modificó BMad. Su configuración resuelve `active_initiative` como objeto de nivel superior, mientras el script de tickets documenta una clave bajo `core`; usar la ruta explícita evita depender de esa diferencia:

```bat
uv --cache-dir .uv-cache run python .agents/skills/bmad-ticket/scripts/tickets.py --project-root . status _bmad-output/initiative-plugin-oficial-cpp-notepadpp-ldap
```

Usar igualmente la ruta explícita al operar `next`, `find` o `mark`, conforme a la ayuda del script instalado. Los estados no se editaron durante esta integración.

## Git y verificación de archivos

`AGENTS.md` dejó de estar ignorado. `.gitignore` permite exclusivamente la skill Graphify bajo `.agents/`, mantiene las demás skills BMad locales e ignora `.codex/` completo. `.graphifyignore` se comparte; `.gitattributes` fue creado por Graphify para su merge driver. `git check-ignore` confirma las exclusiones de cachés, renders, builds y wrappers, y que las fuentes esenciales siguen visibles.

Se leyeron independientemente las rutas, tamaños y encabezados del grafo, reporte, skill, referencias, manifiesto Ponytail y hooks. Los documentos se decodificaron con UTF-8 estricto, sin sustituciones. `git diff --check` pasó. No se compiló ni ejecutó la matriz del plugin LDAP por este cambio de tooling. Los cambios de código que ya estaban en el workspace se conservaron; no se creó un commit.

## Auditoría de continuación del 2026-10-14

La consulta `graphify query "ScintillaGateway or LdapParser" --budget 1500` respondió con 35 nodos y 64 aristas. Graphify 0.9.90 y sus tres hooks Git están instalados; `graphify hook-check` terminó correctamente. No se dispararon eventos reales de commit, checkout o merge durante esta auditoría.

Ponytail 5.2.0 está instalado y habilitado. La revisión del diff aplicó el modo `full` y YAGNI. Los scripts de activación, seguimiento de modo y subagentes pasaron `node --check`. Un sondeo de solo lectura resolvió `defaultMode=full`, `codexHookEnvironment=false` y `observedMode=null`: no acredita ejecución automática de hooks en esta sesión.

`bmad-prep.cmd` falló dentro del sandbox con `[Errno 13]` al leer el manifiesto del render existente. El comando canónico de `render_skill.py` terminó correctamente al ejecutarlo fuera del sandbox y se leyó independientemente el workflow generado (3962 bytes, UTF-8 estricto). No se cambiaron ACL ni se borró la generación. El acceso dentro del sandbox sigue limitado; no se interpreta el mensaje del renderer como prueba de corrupción del contenido.

Manifiesto, output y configuración confirman la iniciativa activa. `tickets.py status` y `next` con ruta explícita confirman 3.1–3.6 `done`, 3.7 `in-progress` y 4.1/4.2 disponibles para iniciar. La rama `feat/scintilla-integration-3.7` corresponde a 3.7. El ticket 2.7 sigue en `review` con el bloqueo registrado `ABI_x86_executable_missing_after_clean_build`. No se modificaron estados: el siguiente destino es `bmad-build` sobre 3.7, atendiendo el checkpoint pendiente de su spec y usando la ruta explícita de iniciativa.

El SDK coincide con el commit fijado y sus headers están limpios. CMake no está en PATH de esta sesión; se usó el ejecutable instalado registrado en `.build/x64/CMakeCache.txt`. El build x64 y las cinco pruebas CTest pasaron. Las 16 pruebas Python de evidencia pasaron fuera del sandbox; dentro falló la escritura de un fixture temporal, incluso al ubicarlo en `.uv-cache/`. Las identidades PE, tamaños y arquitectura de la DLL y del harness x64 se verificaron con otra lectura.

El E2E reportó 103 casos, cero fallos de aserción y seis inyecciones con pérdida de integridad de texto, selección o historial. Reporta correctamente `preservation_gate=failed`, `native_gates=pending` y `release_blocked=true`. El resultado verde de CTest demuestra contención en el host simulado; no cierra preservación ni puertas nativas.

La revisión Ponytail encontró una brecha de prueba en `tests/PluginDllE2ETests.cpp:228`: los casos de activación, cierre y selección envían SHUTDOWN antes de observar la entrega del worker. Así, el cierre cancela el trabajo aunque falle la cancelación específica del evento. La corrección mínima es observar la finalización/entrega antes del SHUTDOWN en esos tres casos, manteniendo el caso de shutdown por separado. La auditoría no modifica código de pruebas ni de producto.

No se recompilaron x86/ARM64 ni se ejecutaron sus hosts, GitHub CI o los eventos reales de hooks. Las puertas nativas y la preservación siguen pendientes de resolución; no hay decisión de release.

Fuentes: [Graphify](https://github.com/Graphify-Labs/graphify), [instalación de Ponytail](https://github.com/DietrichGebert/ponytail/blob/main/INSTALL.md), [skills en Codex: documentación oficial](https://learn.chatgpt.com/docs/build-skills), [marketplaces y plugins: documentación oficial](https://developers.openai.com/plugins/build/plugins).
