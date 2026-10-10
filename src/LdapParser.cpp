#include "LdapParser.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <utility>

namespace ldap {
namespace {

struct Stopped { std::size_t offset{}; };
struct Checkpoint {
    std::stop_token token{};
    std::size_t ticks{};
    CorePhase phase{CorePhase::Entry};
    CheckpointObserver observer{};
    void* context{};
    void check(std::size_t offset, bool force = false) {
        if (force || (++ticks % 1024 == 0)) {
            if (observer) { observer(phase, offset, context); }
            if (token.stop_requested()) { throw Stopped{offset}; }
        }
    }
};

struct PhaseGuard {
    Checkpoint& stop;
    CorePhase previous;
    PhaseGuard(Checkpoint& checkpoint, CorePhase phase) noexcept
        : stop{checkpoint}, previous{checkpoint.phase} { stop.phase = phase; }
    ~PhaseGuard() { stop.phase = previous; }
};

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
    return ParseResult{{}, {}, ParseError{code, offset}};
}

[[nodiscard]] std::optional<ParseError> validateUtf8(std::string_view input, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::Utf8};
    for (std::size_t i{}; i < input.size();) {
        stop.check(i);
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
    std::string_view input, std::size_t begin, std::size_t end, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::Attribute};
    if (begin == end) { return begin; }
    std::size_t i{begin};
    const auto first{static_cast<unsigned char>(input[i])};
    if (asciiAlpha(first)) {
        ++i;
        while (i < end && keyChar(static_cast<unsigned char>(input[i]))) { stop.check(i); ++i; }
    } else if (asciiDigit(first)) {
        // numericOID: dos o más arcos sin ceros iniciales.
        std::size_t arcs{};
        while (true) {
            stop.check(i);
            if (i >= end || !asciiDigit(static_cast<unsigned char>(input[i]))) { return i; }
            if (input[i] == '0') {
                ++i;
                if (i < end && asciiDigit(static_cast<unsigned char>(input[i]))) { return i; }
            } else {
                do { stop.check(i); ++i; }
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
        do { stop.check(i); ++i; }
        while (i < end && keyChar(static_cast<unsigned char>(input[i])));
    }
    if (i != end) { return i; }
    return std::nullopt;
}

struct LeafResult {
    std::size_t next{};
    std::optional<ParseError> error{};
};

enum class WildcardPolicy { Allow, InvalidSyntax, UnsupportedFilter };

// The caller owns the phase so extraction does not change checkpoint traces.
[[nodiscard]] LeafResult scanAssertionValue(
    std::string_view input, std::size_t begin, WildcardPolicy wildcard, Checkpoint& stop) {
    for (std::size_t i{begin}; i < input.size(); ++i) {
        stop.check(i);
        const auto ch{static_cast<unsigned char>(input[i])};
        if (ch == ')') { return {i + 1, std::nullopt}; }
        if (ch == '(') { return {0, ParseError{LdapErrorCode::InvalidSyntax, i}}; }
        if (ch == '*' && wildcard != WildcardPolicy::Allow) {
            const auto code{wildcard == WildcardPolicy::UnsupportedFilter
                ? LdapErrorCode::UnsupportedFilter : LdapErrorCode::InvalidSyntax};
            return {0, ParseError{code, i}};
        }
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

void appendCheckedRange(BudgetString& output, std::string_view input,
    std::size_t begin, std::size_t end, Checkpoint& stop) {
    for (std::size_t pos{begin}; pos < end;) {
        stop.check(pos, true);
        const auto amount{std::min<std::size_t>(1024, end - pos)};
        output.append(input.data() + pos, amount);
        pos += amount;
    }
}

[[nodiscard]] std::optional<std::size_t> validateMatchingRule(
    std::string_view input, std::size_t begin, std::size_t end, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::MatchingRule};
    for (std::size_t i{begin}; i < end; ++i) {
        stop.check(i);
        if (input[i] == ';') { return i; }
    }
    return validateAttribute(input, begin, end, stop);
}

[[nodiscard]] constexpr bool isDn(std::string_view token) noexcept {
    return token.size() == 2
        && (token[0] == 'd' || token[0] == 'D')
        && (token[1] == 'n' || token[1] == 'N');
}

[[nodiscard]] LeafResult scanAssertionLeaf(std::string_view input, std::size_t open, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::Value};
    const std::size_t begin{open + 1};
    std::size_t cursor{begin};
    while (cursor < input.size()) {
        stop.check(cursor);
        const char ch{input[cursor]};
        if (ch == '=' || ch == '~' || ch == '>' || ch == '<' || ch == ':'
            || ch == ')' || ch == '(') { break; }
        ++cursor;
    }
    if (cursor == input.size()) {
        return {0, ParseError{LdapErrorCode::UnbalancedParenthesis, cursor}};
    }
    const char separator{input[cursor]};
    if (separator == '(' || separator == ')') {
        return {0, ParseError{LdapErrorCode::InvalidSyntax, cursor}};
    }
    const bool hasAttribute{cursor != begin};
    if (hasAttribute) {
        if (const auto bad{validateAttribute(input, begin, cursor, stop)}; bad.has_value()) {
            return {0, ParseError{LdapErrorCode::InvalidAttribute, *bad}};
        }
    } else if (separator != ':') {
        return {0, ParseError{LdapErrorCode::InvalidAttribute, begin}};
    }

    bool allowWildcard{};
    if (separator == ':') {
        const auto tokenEnd = [&input, &stop](std::size_t start) {
            std::size_t end{start};
            while (end < input.size() && input[end] != ':' && input[end] != '='
                && input[end] != '(' && input[end] != ')') { stop.check(end); ++end; }
            return end;
        };
        ++cursor;
        if (cursor < input.size() && input[cursor] == '=') {
            if (!hasAttribute) {
                return {0, ParseError{LdapErrorCode::InvalidAttribute, cursor - 1}};
            }
            ++cursor;
        } else {
            const std::size_t firstBegin{cursor};
            const std::size_t firstEnd{tokenEnd(firstBegin)};
            if (firstBegin == firstEnd) {
                return {0, ParseError{LdapErrorCode::InvalidAttribute, firstBegin}};
            }
            if (isDn(input.substr(firstBegin, firstEnd - firstBegin))) {
                if (firstEnd == input.size() || input[firstEnd] != ':') {
                    return {0, ParseError{LdapErrorCode::InvalidSyntax, firstEnd}};
                }
                cursor = firstEnd + 1;
                if (cursor < input.size() && input[cursor] == '=') {
                    if (!hasAttribute) {
                        return {0, ParseError{LdapErrorCode::InvalidAttribute, firstEnd}};
                    }
                    ++cursor;
                } else {
                    const std::size_t ruleBegin{cursor};
                    const std::size_t ruleEnd{tokenEnd(ruleBegin)};
                    if (const auto bad{validateMatchingRule(input, ruleBegin, ruleEnd, stop)};
                        bad.has_value()) {
                        return {0, ParseError{LdapErrorCode::InvalidAttribute, *bad}};
                    }
                    cursor = ruleEnd;
                    if (cursor + 1 >= input.size() || input[cursor] != ':'
                        || input[cursor + 1] != '=') {
                        return {0, ParseError{LdapErrorCode::InvalidSyntax, cursor}};
                    }
                    cursor += 2;
                }
            } else {
                if (const auto bad{validateMatchingRule(input, firstBegin, firstEnd, stop)};
                    bad.has_value()) {
                    return {0, ParseError{LdapErrorCode::InvalidAttribute, *bad}};
                }
                cursor = firstEnd;
                if (cursor + 1 >= input.size() || input[cursor] != ':'
                    || input[cursor + 1] != '=') {
                    return {0, ParseError{LdapErrorCode::InvalidSyntax, cursor}};
                }
                cursor += 2;
            }
        }
    } else if (separator == '=') {
        allowWildcard = true;
        ++cursor;
    } else {
        if (cursor + 1 >= input.size() || input[cursor + 1] != '=') {
            return {0, ParseError{LdapErrorCode::InvalidSyntax, cursor}};
        }
        cursor += 2;
    }

    return scanAssertionValue(input, cursor,
        allowWildcard ? WildcardPolicy::Allow : WildcardPolicy::InvalidSyntax, stop);
}

[[nodiscard]] LeafResult scanLeaf(std::string_view input, std::size_t open, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::Value};
    std::size_t equals{open + 1};
    while (equals < input.size() && input[equals] != '=') {
        stop.check(equals);
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
    if (const auto bad{validateAttribute(input, open + 1, equals, stop)}; bad.has_value()) {
        return {0, ParseError{LdapErrorCode::InvalidAttribute, *bad}};
    }

    return scanAssertionValue(input, equals + 1, WildcardPolicy::UnsupportedFilter, stop);
}

[[nodiscard]] ParseResult parseValidated(std::string_view input, Checkpoint& stop) {
    if (input.empty() || input.front() != '(') {
        return fail(LdapErrorCode::InvalidSyntax, 0);
    }
    if (input.size() < 3) { return fail(LdapErrorCode::InvalidSyntax, input.size()); }
    const auto first{input[1]};
    if (first == '&' || first == '|' || first == '!') {
        return fail(LdapErrorCode::UnsupportedFilter, 1);
    }
    const auto leaf{scanLeaf(input, 0, stop)};
    if (leaf.error.has_value()) {
        // El tracer 2.1 conserva su diagnóstico histórico para cierre ausente.
        const auto code{leaf.error->code == LdapErrorCode::UnbalancedParenthesis
            ? LdapErrorCode::InvalidSyntax : leaf.error->code};
        return fail(code, leaf.error->offset);
    }
    if (leaf.next != input.size()) {
        return fail(LdapErrorCode::InvalidSyntax, leaf.next);
    }
    return ParseResult{};
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

void skipStructuralSpace(std::string_view input, std::size_t& cursor, Checkpoint& stop) {
    while (cursor < input.size() && structuralSpace(input[cursor])) {
        PhaseGuard whitespace{stop, CorePhase::Whitespace};
        stop.check(cursor);
        ++cursor;
    }
}

[[nodiscard]] FormatResult formatFail(LdapErrorCode code, std::size_t offset) noexcept {
    return FormatResult{{}, {}, ParseError{code, offset}};
}

template<class Consumer>
[[nodiscard]] std::optional<ParseError> scanStructure(
    std::string_view input, Consumer& consume, Checkpoint& stop) {
    PhaseGuard phase{stop, CorePhase::Structure};
    enum class State { NeedFilter, NeedChild, AfterChild, Done };
    std::array<Frame, MaxDepth> stack{};
    std::size_t depth{};
    std::size_t cursor{};
    State state{State::NeedFilter};

    while (true) {
        stop.check(cursor);
        if (state == State::Done) {
            if (cursor == input.size()) { return std::nullopt; }
            return ParseError{input[cursor] == ')' ? LdapErrorCode::UnbalancedParenthesis
                                                     : LdapErrorCode::InvalidSyntax, cursor};
        }
        if (state == State::NeedChild || state == State::AfterChild) {
            skipStructuralSpace(input, cursor, stop);
            if (cursor == input.size()) {
                return ParseError{LdapErrorCode::UnbalancedParenthesis, cursor};
            }
            if (input[cursor] == ')') {
                if (state == State::NeedChild) {
                    return ParseError{LdapErrorCode::EmptyGroup, cursor};
                }
                if (const auto bad{consume(Event{EventKind::Close, cursor, cursor + 1, stack[depth - 1].op})}; bad) { return bad; }
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
        std::size_t component{cursor + 1};
        skipStructuralSpace(input, component, stop);
        if (component == input.size()) {
            return ParseError{LdapErrorCode::UnbalancedParenthesis, component};
        }
        const char op{input[component]};
        if (op == '&' || op == '|' || op == '!') {
            if (const auto bad{consume(Event{EventKind::Open, cursor, component + 1, op})}; bad) { return bad; }
            stack[depth++] = Frame{op, 0};
            cursor = component + 1;
            state = State::NeedChild;
            continue;
        }
        const auto leaf{scanAssertionLeaf(input, cursor, stop)};
        if (leaf.error.has_value()) { return leaf.error; }
        if (const auto bad{consume(Event{EventKind::Leaf, cursor, leaf.next, 0})}; bad) { return bad; }
        cursor = leaf.next;
        if (depth == 0) { state = State::Done; }
        else { ++stack[depth - 1].children; state = State::AfterChild; }
    }
}

struct Consumer {
    std::string_view input{};
    FormatOptions options{};
    Checkpoint& stop;
    BudgetString* output{};
    std::size_t size{};
    std::size_t depth{};
    std::optional<ParseError> overflow{};
    [[nodiscard]] std::optional<ParseError> operator()(const Event& event) {
        PhaseGuard phase{stop, output ? CorePhase::Copy : CorePhase::Count};
        stop.check(event.begin, true);
        const std::string_view eol{options.eol == Eol::CrLf ? "\r\n"
            : options.eol == Eol::Lf ? "\n" : "\r"};
        if (event.kind == EventKind::Close) { --depth; }
        const bool indent{options.mode == OutputMode::Pretty
            && (event.kind == EventKind::Close || depth != 0)};
        const std::size_t content{event.kind == EventKind::Open ? 2
            : event.kind == EventKind::Close ? 1 : event.end - event.begin};
        const std::size_t prefix{indent ? eol.size() + depth * 2 : 0};
        if (!overflow && (prefix > MaxOutputBytes - size
            || content > MaxOutputBytes - size - prefix)) {
            overflow = ParseError{LdapErrorCode::OutputTooLarge, event.begin};
        }
        if (!overflow) {
            size += prefix + content;
            if (output) {
                if (indent) {
                    output->append(eol);
                    output->append(depth * 2, ' ');
                }
                if (event.kind == EventKind::Open) { output->push_back('('); output->push_back(event.op); }
                else if (event.kind == EventKind::Close) { output->push_back(')'); }
                else {
                    appendCheckedRange(*output, input, event.begin, event.end, stop);
                }
            }
        }
        if (event.kind == EventKind::Open) { ++depth; }
        return std::nullopt;
    }
};

// Fixed workspace includes the 128-frame stack and bounded scanner/consumer locals.
// Returned object storage is reserved separately; output capacities use the allocator.
inline constexpr std::size_t WorkspaceBytes{sizeof(Frame) * MaxDepth + 2048};
struct Operation {
    std::shared_ptr<CoreBudget> budget{};
    std::optional<CoreBudget::Reservation> input{};
    std::optional<CoreBudget::Reservation> workspace{};
    Operation(std::string_view source, const FormatOptions& options)
        : budget{options.budget ? options.budget : std::make_shared<CoreBudget>()} {
        if (options.inputReservation) {
            if (options.inputReservation->budget() != budget.get()
                || options.inputReservation->bytes() < source.size()) { throw BudgetExceeded{}; }
        } else {
            input = budget->reserve(source.size());
            if (!input) { throw BudgetExceeded{}; }
        }
        workspace = budget->reserve(WorkspaceBytes);
        if (!workspace) { throw BudgetExceeded{}; }
    }
    [[nodiscard]] CoreBudget::Reservation resultStorage(std::size_t bytes) const {
        auto reservation{budget->reserve(bytes)};
        if (!reservation) { throw BudgetExceeded{}; }
        return std::move(*reservation);
    }
};
// Two consumers coexist while emitting. The 512-byte remainder covers primitive
// scanner/helper locals and temporary diagnostics beyond the named fixed objects.
static_assert(sizeof(Frame) * MaxDepth + 2 * sizeof(Consumer) + sizeof(Operation)
    + sizeof(Checkpoint) + sizeof(Event) + 512 <= WorkspaceBytes);
} // namespace

ParseResult parseSimpleEquality(std::string_view input, FormatOptions options) noexcept {
    try {
        Checkpoint stop{options.stop, 0, CorePhase::Entry, options.checkpoint, options.checkpointContext};
        stop.check(0, true);
        if (input.size() > MaxInputBytes) { return fail(LdapErrorCode::InputTooLarge, MaxInputBytes); }
        Operation operation{input, options};
        if (const auto bad{validateUtf8(input, stop)}; bad) { return fail(bad->code, bad->offset); }
        auto validated{parseValidated(input, stop)};
        if (!validated.ok()) { return validated; }
        ParseResult result{operation.resultStorage(sizeof(ParseResult)),
            BudgetString{BudgetAllocator<char>{operation.budget}}, std::nullopt};
        stop.check(0, true);
        // Force heap ownership so extracting even short text retains its charge.
        result.compact.reserve(std::max(input.size(), result.compact.capacity() + 1));
        PhaseGuard copy{stop, CorePhase::Copy};
        appendCheckedRange(result.compact, input, 0, input.size(), stop);
        stop.phase = CorePhase::Publish;
        stop.check(input.size(), true);
        return result;
    } catch (const Stopped& stopped) { return fail(LdapErrorCode::Cancelled, stopped.offset); }
    catch (const BudgetExceeded&) { return fail(LdapErrorCode::MemoryExceeded, 0); }
    catch (const std::bad_alloc&) { return fail(LdapErrorCode::ResourceFailure, 0); }
    catch (const std::length_error&) { return fail(LdapErrorCode::ResourceFailure, 0); }
    catch (...) { return fail(LdapErrorCode::InternalFailure, 0); }
}

FormatResult formatFilter(std::string_view input, FormatOptions options) noexcept {
    try {
        Checkpoint stop{options.stop, 0, CorePhase::Entry, options.checkpoint, options.checkpointContext};
        stop.check(0, true);
        if (input.size() > MaxInputBytes) { return formatFail(LdapErrorCode::InputTooLarge, MaxInputBytes); }
        Operation operation{input, options};
        if (const auto bad{validateUtf8(input, stop)}; bad) { return formatFail(bad->code, bad->offset); }
        Consumer count{input, options, stop};
        if (const auto bad{scanStructure(input, count, stop)}; bad) { return formatFail(bad->code, bad->offset); }
        if (count.overflow) { return formatFail(count.overflow->code, count.overflow->offset); }
        stop.check(input.size(), true);
        FormatResult result{operation.resultStorage(sizeof(FormatResult)),
            BudgetString{BudgetAllocator<char>{operation.budget}}, std::nullopt};
        // Force heap ownership so extracting even short text retains its charge.
        result.text.reserve(std::max(count.size, result.text.capacity() + 1));
        Consumer emit{input, options, stop, &result.text};
        if (const auto bad{scanStructure(input, emit, stop)}; bad) { return formatFail(bad->code, bad->offset); }
        stop.phase = CorePhase::Publish;
        stop.check(input.size(), true);
        return result;
    } catch (const Stopped& stopped) { return formatFail(LdapErrorCode::Cancelled, stopped.offset); }
    catch (const BudgetExceeded&) { return formatFail(LdapErrorCode::MemoryExceeded, 0); }
    catch (const std::bad_alloc&) { return formatFail(LdapErrorCode::ResourceFailure, 0); }
    catch (const std::length_error&) { return formatFail(LdapErrorCode::ResourceFailure, 0); }
    catch (...) { return formatFail(LdapErrorCode::InternalFailure, 0); }
}
} // namespace ldap
