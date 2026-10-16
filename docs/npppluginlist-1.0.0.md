# Preparación de nppPluginList — LDAP Formatter 1.0.0

Paquetes locales compilados el 2026-10-16 desde las mismas fuentes, Release,
MSVC v143, CRT estático y SDK fijado. No constituyen una release publicada ni
sustituyen los gates pendientes del [dossier](release-dossier-4.6.md).

| Arquitectura | ZIP bajo `.build/release-1.0.0/` | Bytes | SHA-256 del ZIP completo |
| --- | --- | ---: | --- |
| x64 | `x64/NppLdapFormatter-1.0.0-x64.zip` | 124732 | `59aba8486e18f646b9edfe91aa2a902be77466b00077cfb330ab38c2b68cbc27` |
| x86 | `x86/NppLdapFormatter-1.0.0-x86.zip` | 102617 | `fae55227ebc30f96e8e4c10df0e288e0f36211f2f0a111147d2e1df46aaee858` |
| ARM64 | `ARM64/NppLdapFormatter-1.0.0-ARM64.zip` | 102328 | `5ee4db6d3912fc592756f27c46f43a14cea3d885b41c2113fe92835da786c361` |

Cada ZIP se reabrió y contiene únicamente `NppLdapFormatter.dll` en su raíz,
idéntica al build correspondiente. Se comprobaron el encabezado PE, la máquina,
el checksum lateral y FileVersion/ProductVersion Windows `1.0.0.0`, sin ejecutar
la DLL ARM64. La versión del recurso procede de CMake.

El fork local es `C:/DevIgnacio/add-ldap-formatter`, remoto
`https://github.com/ignaciojulio/add-ldap-formatter.git`. Se prepararon
`src/pl.x64.json`, `src/pl.x86.json` y `src/pl.arm64.json` con carpeta
`NppLdapFormatter`, nombre visible `LDAP Formatter`, versión `1.0.0`, autor
Ignacio Javier Julio Posada y los hashes de esta tabla. La versión mínima
anunciada es Notepad++ `8.9.8.1`, baseline verificado por el proyecto.

La homepage es `https://github.com/ignaciojulio/npp-ldap-formatter`; los enlaces
de descarga preparados siguen este patrón, respetando `ARM64` en mayúsculas:

```text
https://github.com/ignaciojulio/npp-ldap-formatter/releases/download/v1.0.0/NppLdapFormatter-1.0.0-<arquitectura>.zip
```

## Verificación y límites

- Las tres listas pasan el esquema `pl.schema` del fork, unicidad de carpeta,
  nombre y URL, orden por `display-name` sin distinguir mayúsculas, y lectura
  UTF-8 estricta. Las entradas previas conservan sus valores; se reubicaron
  `NppTranslator` y, en x64, `SimplifyJson`, que estaban fuera de orden.
- CTest x64: 7/7. CTest x86: 5/7; ABI y E2E quedan **Not Run** porque los dos
  EXE no aparecen después de MSBuild. La causa no se ha atribuido. Es el bloqueo
  ya documentado en el dossier. ARM64: compilación cruzada y validación estática;
  ejecución nativa pendiente.
- Regresiones de empaquetado y scripts: 16/16, ejecutadas fuera del sandbox tras
  confirmar que sus restricciones impedían escribir las DLL temporales de prueba.
- No se publicó `v1.0.0`, no se subieron los ZIP ni se hizo push. Los enlaces
  requieren publicar estos bytes exactos para que la descarga y el hash del
  validador en línea funcionen. Reempaquetar cambia el hash aunque la DLL no cambie.
- No se ejecutó el validador global del fork, que descarga todas las entradas;
  se verificaron localmente esquema, recursos y paquetes de LDAP Formatter.
