# LDAP Formatter for Notepad++

[![Windows Release](https://github.com/ignaciojulio/npp-ldap-formatter/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ignaciojulio/npp-ldap-formatter/actions/workflows/ci.yml)
[![Version v1.0.0](https://img.shields.io/badge/version-v1.0.0-blue)](https://github.com/ignaciojulio/npp-ldap-formatter/releases/tag/v1.0.0)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![Windows x64 | x86 | ARM64](https://img.shields.io/badge/Windows-x64%20%7C%20x86%20%7C%20ARM64-0078D4)

Formatea y valida filtros LDAP directamente en Notepad++, sin conectarse a un servidor. El núcleo C++20 analiza una copia del texto fuera del hilo de interfaz y prepara la salida antes de modificar el documento mediante la API de Scintilla.

La validación estructural sigue [RFC 4515](https://www.rfc-editor.org/rfc/rfc4515), comprueba UTF-8 y escapes, y admite layout multilínea al pegar filtros. Una operación de formato completada se revierte con **un solo Ctrl+Z**.

## Características

- **Pretty:** estructura legible con indentación de dos espacios y saltos de línea según el modo CRLF/LF del documento.
- **Compact:** una sola línea continua para copiar filtros a otras herramientas.
- **Tolerancia a multilínea:** normaliza whitespace estructural y los bordes de valores simples pegados en varias líneas; conserva los espacios internos y los escapes. Los espacios significativos de valores de una línea se mantienen.
- **Reemplazo atómico:** aplica la salida validada en un único reemplazo y agrupa el deshacer mediante un guard RAII de Scintilla (`ScintillaUndoGuard`). Los errores de análisis se detectan antes de editar.
- **Privacidad:** cero telemetría, sin consultas LDAP remotas ni registro, envío o persistencia del contenido del documento. El procesamiento propio del filtro ocurre en memoria; esta garantía no describe la E/S del host Notepad++, su runtime u otros plugins.
- **Trabajo acotado:** hasta 5 MiB de entrada y de salida, 128 niveles de profundidad y un presupuesto de memoria del núcleo de 32 MiB. Los límites evitan trabajo sin control; no implican latencia constante para filtros grandes.
- **Windows nativo:** C++20, MSVC v143, runtime estático **`/MT`** en Release y paquetes separados para **x64, x86 y ARM64**.

## Instalación

### Plugins Admin

La incorporación oficial está en proceso. Cuando esté disponible en el catálogo de tu arquitectura, abre **Plugins → Plugins Admin**, busca **LDAP Formatter** e instálalo desde allí.

### Instalación manual

Requisito de referencia: **Notepad++ 8.9.8.1 o posterior**. La arquitectura de la DLL debe coincidir con la de Notepad++, incluso si Windows admite otra arquitectura.

1. Descarga el ZIP correspondiente desde [Releases — v1.0.0](https://github.com/ignaciojulio/npp-ldap-formatter/releases/tag/v1.0.0):

   | Notepad++ | Paquete |
   | --- | --- |
   | x64 | [NppLdapFormatter_x64.zip](https://github.com/ignaciojulio/npp-ldap-formatter/releases/download/v1.0.0/NppLdapFormatter_x64.zip) |
   | x86, 32 bits | [NppLdapFormatter_x86.zip](https://github.com/ignaciojulio/npp-ldap-formatter/releases/download/v1.0.0/NppLdapFormatter_x86.zip) |
   | ARM64 | [NppLdapFormatter_arm64.zip](https://github.com/ignaciojulio/npp-ldap-formatter/releases/download/v1.0.0/NppLdapFormatter_arm64.zip) |

2. Cierra Notepad++ y extrae `NppLdapFormatter.dll` en esta ubicación, creando la carpeta del plugin si hace falta:

   ```text
   <Notepad++>\plugins\NppLdapFormatter\NppLdapFormatter.dll
   ```

3. Reinicia Notepad++ y abre el menú **Plugins → LDAP Formatter**.

Los paquetes existen para las tres arquitecturas. La validación nativa completa x86/ARM64 continúa pendiente; disponer de un ZIP no certifica esa matriz de hosts.

## Uso y atajos

1. Abre un documento UTF-8 con un filtro LDAP.
2. Selecciona el filtro que quieras formatear. **Sin selección, se procesa el documento completo**, que debe contener un único filtro.
3. Elige **Plugins → LDAP Formatter → Format LDAP (Pretty)** o **Format LDAP (Compact)**.
4. Usa **Ctrl+Z** para deshacer el formato en un paso.

No se asignan atajos de formato de fábrica. Puedes configurarlos en **Settings → Shortcut Mapper → Plugin commands**.

Por ejemplo, este filtro Compact:

```ldap
(&(objectClass=person)(|(cn=Ana)(cn=Luis)))
```

se muestra en Pretty como:

```ldap
(&
  (objectClass=person)
  (|
    (cn=Ana)
    (cn=Luis)
  )
)
```

Si el filtro está incompleto, contiene UTF-8 inválido o supera un límite del núcleo, el plugin informa la causa sin aplicar el formato. No es un cliente LDAP: no comprueba atributos contra un esquema ni ejecuta consultas contra un directorio.

## Licencia y autoría

Creado por **Ignacio Julio**. Distribuido bajo la [licencia MIT](LICENSE).
