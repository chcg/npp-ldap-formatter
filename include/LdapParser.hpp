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

// Acepta únicamente (attr=value) RFC 4515, sin espacios fuera de la hoja.
// Compact copia literalmente atributo, escapes y cada byte del valor.
// Valida UTF-8 de toda la entrada antes de reportar errores sintácticos.
// No lanza excepciones a través del contrato; fallos de recursos son tipados.
[[nodiscard]] ParseResult parseSimpleEquality(std::string_view input) noexcept;

} // namespace ldap
