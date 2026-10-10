#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ldap {

enum class LdapErrorCode {
    InvalidUtf8,
    InvalidSyntax,
    InvalidAttribute,
    InvalidEscape,
    UnsupportedFilter,
    EmptyGroup,
    InvalidGroupArity,
    UnbalancedParenthesis,
    DepthExceeded,
    RawNul,
    ResourceFailure,
    InternalFailure,
};

struct ParseError {
    LdapErrorCode code{};
    std::size_t offset{}; // Byte inválido; input.size() si falta un delimitador.
};

struct ParseResult {
    // Solo contiene bytes cuando ok() es true; nunca entrega salida parcial.
    std::string compact{};
    std::optional<ParseError> error{};

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

enum class OutputMode { Pretty, Compact };
enum class Eol { CrLf, Lf, Cr };

struct FormatOptions {
    OutputMode mode{OutputMode::Pretty};
    Eol eol{Eol::CrLf};
};

struct FormatResult {
    std::string text{};
    std::optional<ParseError> error{};

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Acepta únicamente (attr=value) RFC 4515, sin espacios fuera de la hoja.
// Compact copia literalmente atributo, escapes y cada byte del valor.
// Valida UTF-8 de toda la entrada antes de reportar errores sintácticos.
// No lanza excepciones a través del contrato; fallos de recursos son tipados.
[[nodiscard]] ParseResult parseSimpleEquality(std::string_view input) noexcept;

// Analiza grupos AND/OR/NOT y assertions RFC 4515 con pila explícita.
// Cuenta la raíz: acepta hasta 128 filtros abiertos y rechaza el siguiente '('.
// La entrada permite SP/HTAB/CR/LF solo entre nodos y entre '(' y un operador
// lógico, en ambos modos; no los admite dentro de assertions ni fuera de la raíz.
// Compact elimina solo esos separadores estructurales.
// UTF-8 de toda la entrada se valida antes de diagnosticar la estructura.
// Presencia/subcadena distinguen '*' crudo de escapes hexadecimales; extensible
// admite atributo opcional solo cuando hay regla de coincidencia.
// Ambos modos preservan cada byte de las hojas. No devuelve salida parcial ni lanza.
[[nodiscard]] FormatResult formatFilter(
    std::string_view input, FormatOptions options = {}) noexcept;

} // namespace ldap
