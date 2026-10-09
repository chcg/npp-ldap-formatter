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
    std::size_t offset{}; // Desplazamiento en bytes desde el inicio de la entrada.
};

struct ParseResult {
    std::string compact{};
    std::optional<ParseError> error{};

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Analiza solo igualdad simple; no lanza excepciones a través del contrato.
[[nodiscard]] ParseResult parseSimpleEquality(std::string_view input) noexcept;

} // namespace ldap
