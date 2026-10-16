#pragma once

#include "CoreBudget.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>

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
    InputTooLarge,
    OutputTooLarge,
    MemoryExceeded,
    Cancelled,
};

struct ParseError {
    LdapErrorCode code{};
    std::size_t offset{}; // Byte inválido; input.size() si falta un delimitador.
};

struct ParseResult {
    CoreBudget::Reservation storage{};
    // Solo contiene bytes cuando ok() es true; nunca entrega salida parcial.
    std::shared_ptr<BudgetString> compact{};
    std::optional<ParseError> error{};

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
    [[nodiscard]] std::string_view compactView() const noexcept {
        return compact ? std::string_view{*compact} : std::string_view{};
    }
};

enum class OutputMode { Pretty, Compact };
enum class Eol { CrLf, Lf, Cr };
[[nodiscard]] std::optional<ParseError> validateUtf8Bytes(std::string_view input) noexcept;
enum class CorePhase { Entry, Utf8, Attribute, MatchingRule, Value, Whitespace, Structure, Count, Copy, Publish };
using CheckpointObserver = void (*)(CorePhase, std::size_t, void*) noexcept;

struct FormatOptions {
    OutputMode mode{OutputMode::Pretty};
    Eol eol{Eol::CrLf};
    std::stop_token stop{};
    std::shared_ptr<CoreBudget> budget{};
    // A caller-owned reservation covering this borrowed input, on the same budget.
    const CoreBudget::Reservation* inputReservation{};
    // Optional synchronous observer: may request stop; must not modify input.
    // The callback and context must remain valid until the operation returns.
    CheckpointObserver checkpoint{};
    void* checkpointContext{};
};

struct FormatResult {
    CoreBudget::Reservation storage{};
    std::shared_ptr<BudgetString> text{};
    std::optional<ParseError> error{};

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
    [[nodiscard]] std::string_view textView() const noexcept {
        return text ? std::string_view{*text} : std::string_view{};
    }
};

// Acepta únicamente (attr=value) RFC 4515, sin espacios fuera de la hoja.
// Compact copia literalmente atributo, escapes y cada byte del valor.
// Valida UTF-8 de toda la entrada antes de reportar errores sintácticos.
// No lanza excepciones a través del contrato; fallos de recursos son tipados.
[[nodiscard]] ParseResult parseSimpleEquality(std::string_view input, FormatOptions options = {}) noexcept;

// Analiza grupos AND/OR/NOT y assertions RFC 4515 con pila explícita.
// Cuenta la raíz: acepta hasta 128 filtros abiertos y rechaza el siguiente '('.
// La entrada permite SP/HTAB/CR/LF solo entre nodos y entre '(' y un operador
// lógico, tras '(' antes de un atributo/regla y fuera de la raíz, en ambos modos.
// Pretty y Compact eliminan esos separadores estructurales redundantes.
// UTF-8 de toda la entrada se valida antes de diagnosticar la estructura.
// Presencia/subcadena distinguen '*' crudo de escapes hexadecimales; extensible
// admite atributo opcional solo cuando hay regla de coincidencia.
// Ambos modos preservan cada byte de los valores y escapes de las hojas.
// Los offsets siempre se refieren a la entrada original. No devuelve salida parcial ni lanza.
[[nodiscard]] FormatResult formatFilter(
    std::string_view input, FormatOptions options = {}) noexcept;

} // namespace ldap
