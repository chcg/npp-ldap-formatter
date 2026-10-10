#include "LdapParser.hpp"

#include <array>
#include <new>
#include <stdexcept>
#include <vector>

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

struct LeafResult {
    std::size_t next{};
    std::optional<ParseError> error{};
};

[[nodiscard]] LeafResult scanLeaf(std::string_view input, std::size_t open) noexcept {
    std::size_t equals{open + 1};
    while (equals < input.size() && input[equals] != '=') {
        const auto ch{input[equals]};
        if (ch == '>' || ch == '<' || ch == '~' || ch == ':') {
            return {0, ParseError{LdapErrorCode::UnsupportedFilter, equals}};
        }
        if (ch == ')' || ch == '(') {
            return {0, ParseError{LdapErrorCode::InvalidSyntax, equals}};
        }
        ++equals;
    }
    if (equals == input.size()) {
        return {0, ParseError{LdapErrorCode::InvalidSyntax, equals}};
    }
    if (const auto bad{validateAttribute(input, open + 1, equals)}; bad.has_value()) {
        return {0, ParseError{LdapErrorCode::InvalidAttribute, *bad}};
    }

    for (std::size_t i{equals + 1}; i < input.size(); ++i) {
        const auto ch{static_cast<unsigned char>(input[i])};
        if (ch == ')') {
            return {i + 1, std::nullopt};
        }
        if (ch == '(') { return {0, ParseError{LdapErrorCode::InvalidSyntax, i}}; }
        if (ch == '*') { return {0, ParseError{LdapErrorCode::UnsupportedFilter, i}}; }
        if (ch == 0) { return {0, ParseError{LdapErrorCode::RawNul, i}}; }
        if (ch == '\\') {
            if (input.size() - i <= 2 || !asciiHex(static_cast<unsigned char>(input[i + 1]))
                || !asciiHex(static_cast<unsigned char>(input[i + 2]))) {
                return {0, ParseError{LdapErrorCode::InvalidEscape, i}};
            }
            i += 2;
        }
    }
    return {0, ParseError{LdapErrorCode::UnbalancedParenthesis, input.size()}};
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
    const auto leaf{scanLeaf(input, 0)};
    if (leaf.error.has_value()) {
        // El tracer 2.1 conserva su diagnóstico histórico para cierre ausente.
        const auto code{leaf.error->code == LdapErrorCode::UnbalancedParenthesis
            ? LdapErrorCode::InvalidSyntax : leaf.error->code};
        return fail(code, leaf.error->offset);
    }
    if (leaf.next != input.size()) {
        return fail(LdapErrorCode::InvalidSyntax, leaf.next);
    }
    return ParseResult{std::string{input}, std::nullopt};
}

enum class EventKind { Open, Leaf, Close };
struct Event {
    EventKind kind{};
    std::size_t begin{};
    std::size_t end{};
    char op{};
};

struct Frame {
    char op{};
    std::size_t children{};
};

[[nodiscard]] constexpr bool structuralSpace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

[[nodiscard]] FormatResult formatFail(LdapErrorCode code, std::size_t offset) noexcept {
    return FormatResult{{}, ParseError{code, offset}};
}

[[nodiscard]] std::optional<ParseError> scanStructure(
    std::string_view input, std::vector<Event>& events) {
    enum class State { NeedFilter, NeedChild, AfterChild, Done };
    std::array<Frame, 128> stack{};
    std::size_t depth{};
    std::size_t cursor{};
    State state{State::NeedFilter};

    while (true) {
        if (state == State::Done) {
            if (cursor == input.size()) { return std::nullopt; }
            return ParseError{input[cursor] == ')' ? LdapErrorCode::UnbalancedParenthesis
                                                     : LdapErrorCode::InvalidSyntax, cursor};
        }
        if (state == State::NeedChild || state == State::AfterChild) {
            while (cursor < input.size() && structuralSpace(input[cursor])) { ++cursor; }
            if (cursor == input.size()) {
                return ParseError{LdapErrorCode::UnbalancedParenthesis, cursor};
            }
            if (input[cursor] == ')') {
                if (state == State::NeedChild) {
                    return ParseError{LdapErrorCode::EmptyGroup, cursor};
                }
                events.push_back(Event{EventKind::Close, cursor, cursor + 1, stack[depth - 1].op});
                ++cursor;
                --depth;
                if (depth == 0) { state = State::Done; }
                else { ++stack[depth - 1].children; state = State::AfterChild; }
                continue;
            }
            if (input[cursor] != '(') {
                return ParseError{LdapErrorCode::InvalidSyntax, cursor};
            }
            if (state == State::AfterChild && stack[depth - 1].op == '!') {
                return ParseError{LdapErrorCode::InvalidGroupArity, cursor};
            }
            state = State::NeedFilter;
            continue;
        }
        if (cursor == input.size()) {
            return ParseError{LdapErrorCode::UnbalancedParenthesis, cursor};
        }
        if (input[cursor] != '(') {
            return ParseError{input[cursor] == ')' ? LdapErrorCode::UnbalancedParenthesis
                                                     : LdapErrorCode::InvalidSyntax, cursor};
        }
        if (depth == stack.size()) {
            return ParseError{LdapErrorCode::DepthExceeded, cursor};
        }
        if (cursor + 1 == input.size()) {
            return ParseError{LdapErrorCode::UnbalancedParenthesis, input.size()};
        }
        const char op{input[cursor + 1]};
        if (op == '&' || op == '|' || op == '!') {
            events.push_back(Event{EventKind::Open, cursor, cursor + 2, op});
            stack[depth++] = Frame{op, 0};
            cursor += 2;
            state = State::NeedChild;
            continue;
        }
        const auto leaf{scanLeaf(input, cursor)};
        if (leaf.error.has_value()) { return leaf.error; }
        events.push_back(Event{EventKind::Leaf, cursor, leaf.next, 0});
        cursor = leaf.next;
        if (depth == 0) { state = State::Done; }
        else { ++stack[depth - 1].children; state = State::AfterChild; }
    }
}

[[nodiscard]] std::string emitStructure(
    std::string_view input, const std::vector<Event>& events, FormatOptions options) {
    const std::string_view eol{options.eol == Eol::CrLf ? "\r\n"
        : options.eol == Eol::Lf ? "\n" : "\r"};
    std::string output{};
    output.reserve(input.size());
    std::size_t depth{};
    for (const auto& event : events) {
        if (event.kind == EventKind::Close) {
            --depth;
            if (options.mode == OutputMode::Pretty) {
                output.append(eol);
                output.append(depth * 2, ' ');
            }
            output.push_back(')');
            continue;
        }
        if (options.mode == OutputMode::Pretty && depth != 0) {
            output.append(eol);
            output.append(depth * 2, ' ');
        }
        if (event.kind == EventKind::Open) {
            output.push_back('(');
            output.push_back(event.op);
            ++depth;
        } else {
            output.append(input.substr(event.begin, event.end - event.begin));
        }
    }
    return output;
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

FormatResult formatFilter(std::string_view input, FormatOptions options) noexcept {
    try {
        if (const auto bad{validateUtf8(input)}; bad.has_value()) {
            return formatFail(bad->code, bad->offset);
        }
        std::vector<Event> events{};
        if (const auto bad{scanStructure(input, events)}; bad.has_value()) {
            return formatFail(bad->code, bad->offset);
        }
        return FormatResult{emitStructure(input, events, options), std::nullopt};
    } catch (const std::bad_alloc&) {
        return formatFail(LdapErrorCode::ResourceFailure, 0);
    } catch (const std::length_error&) {
        return formatFail(LdapErrorCode::ResourceFailure, 0);
    } catch (...) {
        return formatFail(LdapErrorCode::InternalFailure, 0);
    }
}
} // namespace ldap
