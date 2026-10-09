#include "LdapParser.hpp"

#include <new>
#include <stdexcept>

namespace ldap {
namespace {

[[nodiscard]] constexpr bool asciiAlpha(unsigned char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

[[nodiscard]] constexpr bool asciiDigit(unsigned char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

[[nodiscard]] constexpr bool asciiHex(unsigned char ch) noexcept {
    return asciiDigit(ch) || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
}

[[nodiscard]] constexpr bool keyChar(unsigned char ch) noexcept {
    return asciiAlpha(ch) || asciiDigit(ch) || ch == '-';
}

[[nodiscard]] ParseResult fail(LdapErrorCode code, std::size_t offset) noexcept {
    return ParseResult{{}, ParseError{code, offset}};
}

[[nodiscard]] std::optional<ParseError> validateUtf8(std::string_view input) noexcept {
    for (std::size_t i{}; i < input.size();) {
        const auto lead{static_cast<unsigned char>(input[i])};
        if (lead < 0x80) { ++i; continue; }

        std::size_t length{};
        unsigned char secondMin{0x80};
        unsigned char secondMax{0xBF};
        if (lead >= 0xC2 && lead <= 0xDF) { length = 2; }
        else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) { secondMin = 0xA0; } // Sobrelargo.
            if (lead == 0xED) { secondMax = 0x9F; } // Sustitutos UTF-16.
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) { secondMin = 0x90; } // Sobrelargo.
            if (lead == 0xF4) { secondMax = 0x8F; } // Más allá de U+10FFFF.
        } else { return ParseError{LdapErrorCode::InvalidUtf8, i}; }

        for (std::size_t j{1}; j < length; ++j) {
            if (j >= input.size() - i) {
                return ParseError{LdapErrorCode::InvalidUtf8, i};
            }
            const auto byte{static_cast<unsigned char>(input[i + j])};
            if (byte < (j == 1 ? secondMin : 0x80)
                || byte > (j == 1 ? secondMax : 0xBF)) {
                return ParseError{LdapErrorCode::InvalidUtf8, i + j};
            }
        }
        i += length;
    }
    return std::nullopt;
}

// Devuelve la primera posición inválida, o ninguna si el atributo es válido.
[[nodiscard]] std::optional<std::size_t> validateAttribute(
    std::string_view input, std::size_t begin, std::size_t end) noexcept {
    if (begin == end) { return begin; }
    std::size_t i{begin};
    const auto first{static_cast<unsigned char>(input[i])};
    if (asciiAlpha(first)) {
        ++i;
        while (i < end && keyChar(static_cast<unsigned char>(input[i]))) { ++i; }
    } else if (asciiDigit(first)) {
        // numericOID: dos o más arcos sin ceros iniciales.
        std::size_t arcs{};
        while (true) {
            if (i >= end || !asciiDigit(static_cast<unsigned char>(input[i]))) { return i; }
            if (input[i] == '0') {
                ++i;
                if (i < end && asciiDigit(static_cast<unsigned char>(input[i]))) { return i; }
            } else {
                do { ++i; }
                while (i < end && asciiDigit(static_cast<unsigned char>(input[i])));
            }
            ++arcs;
            if (i >= end || input[i] != '.') { break; }
            ++i;
            if (i == end) { return i; }
        }
        if (arcs < 2) { return begin; }
    } else { return begin; }

    while (i < end && input[i] == ';') {
        ++i;
        if (i == end || !keyChar(static_cast<unsigned char>(input[i]))) { return i; }
        do { ++i; }
        while (i < end && keyChar(static_cast<unsigned char>(input[i])));
    }
    if (i != end) { return i; }
    return std::nullopt;
}

[[nodiscard]] ParseResult parseValidated(std::string_view input) {
    if (input.empty() || input.front() != '(') {
        return fail(LdapErrorCode::InvalidSyntax, 0);
    }
    if (input.size() < 3) { return fail(LdapErrorCode::InvalidSyntax, input.size()); }
    const auto first{input[1]};
    if (first == '&' || first == '|' || first == '!') {
        return fail(LdapErrorCode::UnsupportedFilter, 1);
    }

    std::size_t equals{1};
    while (equals < input.size() && input[equals] != '=') {
        const auto ch{input[equals]};
        if (ch == '>' || ch == '<' || ch == '~' || ch == ':') {
            return fail(LdapErrorCode::UnsupportedFilter, equals);
        }
        if (ch == ')' || ch == '(') {
            return fail(LdapErrorCode::InvalidSyntax, equals);
        }
        ++equals;
    }
    if (equals == input.size()) { return fail(LdapErrorCode::InvalidSyntax, equals); }
    if (const auto bad{validateAttribute(input, 1, equals)}; bad.has_value()) {
        return fail(LdapErrorCode::InvalidAttribute, *bad);
    }

    for (std::size_t i{equals + 1}; i < input.size(); ++i) {
        const auto ch{static_cast<unsigned char>(input[i])};
        if (ch == ')') {
            if (i + 1 != input.size()) {
                return fail(LdapErrorCode::InvalidSyntax, i + 1);
            }
            // En la hoja simple no hay whitespace estructural válido.
            return ParseResult{std::string{input}, std::nullopt};
        }
        if (ch == '(') { return fail(LdapErrorCode::InvalidSyntax, i); }
        if (ch == '*') { return fail(LdapErrorCode::UnsupportedFilter, i); }
        if (ch == 0) { return fail(LdapErrorCode::RawNul, i); }
        if (ch == '\\') {
            if (input.size() - i <= 2 || !asciiHex(static_cast<unsigned char>(input[i + 1]))
                || !asciiHex(static_cast<unsigned char>(input[i + 2]))) {
                return fail(LdapErrorCode::InvalidEscape, i);
            }
            i += 2;
        }
    }
    return fail(LdapErrorCode::InvalidSyntax, input.size());
}
} // namespace

ParseResult parseSimpleEquality(std::string_view input) noexcept {
    try {
        if (const auto bad{validateUtf8(input)}; bad.has_value()) {
            return fail(bad->code, bad->offset);
        }
        return parseValidated(input);
    } catch (const std::bad_alloc&) {
        return fail(LdapErrorCode::ResourceFailure, 0);
    } catch (const std::length_error&) {
        return fail(LdapErrorCode::ResourceFailure, 0);
    } catch (...) {
        return fail(LdapErrorCode::InternalFailure, 0);
    }
}
} // namespace ldap
