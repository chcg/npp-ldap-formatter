# Evidencia de conformidad del núcleo

`LdapParserTests.cpp` ejecuta fixtures con bytes esperados y propiedades deterministas. El perfil del proyecto valida UTF-8 estrictamente; no implementa la tolerancia opcional a UTF-8 inválido de consumidores antiguos descrita por RFC 4515.

| Contrato | Evidencia permanente |
|---|---|
| E2.S1: UTF-8, igualdad, bytes | `valid`, `invalid`, fixtures multibyte y errores con offsets de origen |
| E2.S2: estructura iterativa | AND/OR/NOT en `conformanceProperties`, aridad, balance y profundidad |
| E2.S3: assertions RFC | Catálogo de operadores, presencia/subcadena, extensibles, OID/opciones y escapes en `conformanceProperties` y fixtures negativos |
| E2.S4: emisión | Oráculo de árbol `fixtureText`, indentación de dos espacios; C(C), P(P), C(P), P(C), Pretty LF↔CRLF; whitespace literal conservado |
| E2.S5: límites y detención | `quotasAndCancellation`: entrada 5 MiB y +1, salida exacta CRLF y +1 CRLF/LF, memoria 32 MiB, RAII, concurrencia y nueve fases de cancelación; profundidad 128 aceptada y 129 rechazada |

Los diagnósticos de gramática, UTF-8, entrada y profundidad se ejecutan en Pretty/Compact × LF/CRLF. El desbordamiento de salida se verifica con fixtures específicos de EOL: Compact no expande las hojas. El oráculo no consulta el parser; el corpus combina cada hoja con AND, OR y NOT, e incluye un árbol de 128 niveles.

Fuente normativa: [RFC 4515, secciones 3 y 4](https://www.rfc-editor.org/rfc/rfc4515.html). El corpus aporta evidencia de las identidades sobre casos concretos; no constituye una demostración universal.

Verificación del ticket 2.7: Release x64, CTest completo 2/2, también en el preset x64 con `/analyze`. Compilación cruzada ARM64 con `/analyze` completada; el script de matriz omite ejecución ARM64. Las pruebas del núcleo x86 pasan, pero CTest x86 completo queda pendiente: `plugin_abi_smoke.exe` desaparece después del build, tanto en `.build/win32` como en un directorio nuevo `build-x86`. No se confirmó la causa ni se modificaron protecciones del entorno. No se acredita ejecución remota CI ni pruebas del host Notepad++.

La evidencia del núcleo está integrada; la certificación de la matriz y el cierre administrativo de 2.7/Épica 2 permanecen pendientes de resolver la prueba ABI x86. Las pruebas nativas ARM64 siguen requiriendo un runner adecuado.
