#include "LdapParser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <atomic>
#include <thread>
#include <barrier>
#include <new>

namespace {
std::atomic<bool> failNextAllocation{};
std::atomic<std::ptrdiff_t> failAllocationAfter{-1};
}
#ifdef _MSC_VER
_Ret_notnull_ _Post_writable_byte_size_(bytes)
#endif
void* operator new(std::size_t bytes) {
    if (failNextAllocation.exchange(false)) { throw std::bad_alloc{}; }
    if (failAllocationAfter.load() >= 0 && failAllocationAfter.fetch_sub(1) == 0) {
        throw std::bad_alloc{};
    }
    if (auto* pointer{std::malloc(bytes == 0 ? 1 : bytes)}) { return pointer; }
    throw std::bad_alloc{};
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
#ifdef _MSC_VER
_Ret_notnull_ _Post_writable_byte_size_(bytes)
#endif
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
int failures{};

void valid(std::string_view input) {
    const auto result{ldap::parseSimpleEquality(input)};
    if (!result.ok() || result.compactView() != input) {
        std::cerr << "Valid filter rejected or bytes changed\n";
        ++failures;
        return;
    }
    const auto repeated{ldap::parseSimpleEquality(result.compactView())};
    if (!repeated.ok() || repeated.compactView() != result.compactView()) {
        std::cerr << "Compact output is not idempotent\n";
        ++failures;
    }
}

void invalid(std::string_view input, ldap::LdapErrorCode code, std::size_t offset) {
    const auto result{ldap::parseSimpleEquality(input)};
    if (result.ok() || !result.compactView().empty() || result.error->code != code
        || result.error->offset != offset) {
        std::cerr << "Wrong error code/offset or partial output\n";
        ++failures;
    }
}

void formatted(std::string_view input, ldap::FormatOptions options, std::string_view expected) {
    const auto result{ldap::formatFilter(input, options)};
    if (!result.ok() || result.textView() != expected) {
        std::cerr << "Structural formatter changed expected bytes\n";
        ++failures;
        return;
    }
    const auto repeated{ldap::formatFilter(result.textView(), options)};
    if (!repeated.ok() || repeated.textView() != expected) {
        std::cerr << "Structural formatter is not idempotent\n";
        ++failures;
    }
}

void formatInvalid(std::string_view input, ldap::LdapErrorCode code, std::size_t offset) {
    for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
        for (const auto eol : {ldap::Eol::Lf, ldap::Eol::CrLf}) {
            // Este fixture de expansión usa CRLF; LF tiene fronteras propias.
            if (code == ldap::LdapErrorCode::OutputTooLarge
                && (mode != ldap::OutputMode::Pretty || eol != ldap::Eol::CrLf)) { continue; }
            const auto result{ldap::formatFilter(input, {mode, eol})};
            if (result.ok() || !result.textView().empty() || result.error->code != code
                || result.error->offset != offset) {
                std::cerr << "Wrong structural error for " << input.substr(0, 80)
                    << " mode=" << static_cast<int>(mode) << " EOL=" << static_cast<int>(eol)
                    << ": expected " << static_cast<int>(code) << '/' << offset;
                if (result.error) {
                    std::cerr << ", got " << static_cast<int>(result.error->code)
                        << '/' << result.error->offset;
                }
                std::cerr << '\n';
                ++failures;
            }
        }
    }
}

void idempotentModes(std::string_view input, std::string_view expectedCompact,
    std::string_view expectedPretty, ldap::Eol eol) {
    const ldap::FormatOptions compact{ldap::OutputMode::Compact, eol};
    const ldap::FormatOptions pretty{ldap::OutputMode::Pretty, eol};
    const auto compactOnce{ldap::formatFilter(input, compact)};
    const auto prettyOnce{ldap::formatFilter(input, pretty)};
    if (!compactOnce.ok() || compactOnce.textView() != expectedCompact
        || !prettyOnce.ok() || prettyOnce.textView() != expectedPretty) {
        std::cerr << "Cross-mode fixture " << input.substr(0, 80)
            << " EOL=" << static_cast<int>(eol) << " canonical bytes failed\n";
        ++failures;
        return;
    }
    const auto compactTwice{ldap::formatFilter(compactOnce.textView(), compact)};
    const auto prettyTwice{ldap::formatFilter(prettyOnce.textView(), pretty)};
    const auto compactFromPretty{ldap::formatFilter(prettyOnce.textView(), compact)};
    const auto prettyFromCompact{ldap::formatFilter(compactOnce.textView(), pretty)};
    if (!compactTwice.ok() || compactTwice.textView() != expectedCompact
        || !prettyTwice.ok() || prettyTwice.textView() != expectedPretty
        || !compactFromPretty.ok() || compactFromPretty.textView() != expectedCompact
        || !prettyFromCompact.ok() || prettyFromCompact.textView() != expectedPretty) {
        std::cerr << "Fixture " << input.substr(0, 80) << " EOL=" << static_cast<int>(eol)
            << " C(C)=" << (compactTwice.ok() && compactTwice.textView() == expectedCompact)
            << " P(P)=" << (prettyTwice.ok() && prettyTwice.textView() == expectedPretty)
            << " C(P)=" << (compactFromPretty.ok() && compactFromPretty.textView() == expectedCompact)
            << " P(C)=" << (prettyFromCompact.ok() && prettyFromCompact.textView() == expectedPretty) << '\n';
        ++failures;
    }
}

std::string nestedNot(std::size_t levels) {
    std::string input{};
    for (std::size_t i{1}; i < levels; ++i) { input.append("(!"); }
    input.append("(cn=x)");
    for (std::size_t i{1}; i < levels; ++i) { input.push_back(')'); }
    return input;
}

// Oráculo de emisión independiente del parser: las hojas son bytes inmutables.
struct FilterFixture {
    std::string leaf{};
    char operation{};
    std::vector<FilterFixture> children{};
};

std::string fixtureText(const FilterFixture& node, std::string_view eol,
    std::size_t depth = 0, bool decorated = false) {
    if (!node.leaf.empty()) { return node.leaf; }
    std::string result{"("};
    if (decorated) { result.append(" \t"); }
    result.push_back(node.operation);
    for (const auto& child : node.children) {
        if (decorated) { result.append(" \r\n\t "); }
        else if (!eol.empty()) {
            result.append(eol);
            result.append(2 * (depth + 1), ' ');
        }
        result.append(fixtureText(child, eol, depth + 1, decorated));
    }
    if (decorated) { result.append(" \n "); }
    else if (!eol.empty()) {
        result.append(eol);
        result.append(2 * depth, ' ');
    }
    result.push_back(')');
    return result;
}

void conformanceProperties() {
    // RFC 4515 §4 más bordes del perfil UTF-8 estricto del proyecto.
    const std::vector<std::string> leaves{
        "(cn=Babs Jensen)", "(seeAlso=)", "(o=univ*of*mich*)",
        "(cn=*)", "(cn=**)", "(cn=*\\2A*)", "(cn=\\28\\29\\2A\\5C\\00\\ff)",
        "(cn=\\2a\\5c)", "(cn~=John Doe)", "(age>=21)", "(age<=65)",
        "(cn:caseExactMatch:=Fred Flintstone)", "(cn:=Betty Rubble)", "(cn:dn:dn:=x)",
        "(sn:dn:2.4.6.8.10:=Barney Rubble)", "(o:dn:=Ace Industry)",
        "(:1.2.3:=Wilma Flintstone)", "(:DN:2.4.6.8.10:=Dino)",
        "(1.2.840.113556;binary=\\00)", "(cn= John\tDoe\r\n )",
        "(cn=Jos\xC3\xA9 \xF0\x9F\x98\x80)"};
    std::vector<FilterFixture> corpus{};
    for (const auto& leaf : leaves) { corpus.push_back({leaf, {}, {}}); }
    // Cada hoja recorre AND, OR y NOT; las combinaciones son reproducibles.
    for (std::size_t i{}; i < leaves.size(); ++i) {
        corpus.push_back({{}, '&', {
            {leaves[i], {}, {}},
            {{}, '|', {{leaves[(i + 1) % leaves.size()], {}, {}},
                {{}, '!', {{leaves[(i + 2) % leaves.size()], {}, {}}}}}}}});
    }
    FilterFixture deep{leaves.front(), {}, {}};
    for (std::size_t level{1}; level < ldap::MaxDepth; ++level) {
        std::vector<FilterFixture> child{};
        child.push_back(std::move(deep));
        deep = {{}, '!', std::move(child)};
    }
    corpus.push_back(std::move(deep));
    for (const auto& node : corpus) {
        const auto compact{fixtureText(node, {})};
        const auto decorated{fixtureText(node, {}, 0, true)};
        for (const auto eol : {ldap::Eol::Lf, ldap::Eol::CrLf, ldap::Eol::Cr}) {
            const auto newline{eol == ldap::Eol::Lf ? "\n" : eol == ldap::Eol::Cr ? "\r" : "\r\n"};
            const auto pretty{fixtureText(node, newline)};
            idempotentModes(compact, compact, pretty, eol);
            idempotentModes(decorated, compact, pretty, eol);
            // P_LF(P_CRLF(x)) y P_CRLF(P_LF(x)); conservar EOL literales.
            for (const auto sourceEol : {"\n", "\r\n", "\r"}) {
                const auto other{fixtureText(node, sourceEol)};
                idempotentModes(other, compact, pretty, eol);
            }
        }
    }
    using ldap::LdapErrorCode;
    formatInvalid("(&(cn=Jos\xC3\xA9)(sn=\\GG))", LdapErrorCode::InvalidEscape, 16);
    formatInvalid("(&(cn=Jos\xC3\xA9)(sn=x)", LdapErrorCode::UnbalancedParenthesis, 18);
    formatInvalid("(&(cn=x)(sn=\xED\xA0\x80))", LdapErrorCode::InvalidUtf8, 13);
    formatInvalid("(cn=x)(sn=y)", LdapErrorCode::InvalidSyntax, 6);
    formatInvalid("(&(!(cn=x)(sn=y)))", LdapErrorCode::InvalidGroupArity, 10);
    auto budget{std::make_shared<ldap::CoreBudget>()};
    auto occupied{budget->reserve(budget->remaining())};
    std::stop_source cancelled{};
    cancelled.request_stop();
    for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
        for (const auto eol : {ldap::Eol::Lf, ldap::Eol::CrLf}) {
            const auto exhausted{ldap::formatFilter("(cn=x)", {mode, eol, {}, budget})};
            if (exhausted.ok() || !exhausted.textView().empty()
                || exhausted.error->code != LdapErrorCode::MemoryExceeded
                || exhausted.error->offset != 0 || budget->used() != ldap::MaxCoreBytes) {
                std::cerr << "Memory diagnostic matrix failed\n";
                ++failures;
            }
            const auto stopped{ldap::formatFilter("(cn=x)", {mode, eol, cancelled.get_token(), budget})};
            if (stopped.ok() || !stopped.textView().empty()
                || stopped.error->code != LdapErrorCode::Cancelled || stopped.error->offset != 0) {
                std::cerr << "Entry cancellation diagnostic matrix failed\n";
                ++failures;
            }
        }
    }
}

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}

void allocatorLifecycle() {
    auto budget{std::make_shared<ldap::CoreBudget>()};
    const auto baseline{budget->used()};
    {
        ldap::BudgetAllocator<char> source{budget};
        auto moved{std::move(source)};
        require(source.budget() == budget && moved.budget() == budget,
            "Allocator move lost source ownership");
        ldap::BudgetAllocator<char> assigned{};
        assigned = std::move(moved);
        require(moved.budget() == budget && assigned.budget() == budget,
            "Allocator assignment lost source ownership");
        ldap::BudgetString original{"value", source};
        ldap::BudgetString destination{std::move(original)};
        original.assign(64, 'x');
        require(destination == std::string{"value"} && original.size() == 64,
            "Moved-from budget string could not be reused");
    }
    require(budget->used() == baseline, "Allocator/string move leaked budget");
    for (const auto from : {"short", "a source longer than the small string buffer"}) {
        for (const auto to : {"short", "a target longer than the small string buffer"}) {
            auto other{std::make_shared<ldap::CoreBudget>()};
            const auto otherBaseline{other->used()};
            {
                ldap::BudgetString source{from, ldap::BudgetAllocator<char>{budget}};
                ldap::BudgetString target{to, ldap::BudgetAllocator<char>{other}};
                target = std::move(source);
                source.assign(64, 'x');
                require(target == std::string{from} && source.size() == 64,
                    "Unequal-budget string move assignment lost bytes or source ownership");
            }
            require(budget->used() == baseline && other->used() == otherBaseline,
                "Unequal-budget string move assignment leaked reservations");
        }
    }
    failNextAllocation.store(true);
    {
        ldap::FormatResult formattedError{};
        ldap::ParseResult parsedError{};
        require(formattedError.textView().empty() && parsedError.compactView().empty(),
            "Default result unexpectedly contains bytes");
    }
    const bool noAllocation{failNextAllocation.exchange(false)};
    require(noAllocation, "Empty failure result allocated memory");
    for (std::size_t allowance{}; allowance <= 256; ++allowance) {
        auto held{budget->reserve(budget->remaining() - allowance)};
        const auto charged{budget->used()};
        try {
            const auto text{std::allocate_shared<ldap::BudgetString>(
                ldap::BudgetAllocator<ldap::BudgetString>{budget}, "", ldap::BudgetAllocator<char>{budget})};
            require(text->empty(), "Fresh budget string contains bytes");
        } catch (const ldap::BudgetExceeded&) {}
        require(budget->used() == charged, "Output bookkeeping quota failure leaked storage");
    }
    require(budget->used() == baseline, "Output bookkeeping retained reservations");
}

void outputAllocationFailures() {
    auto budget{std::make_shared<ldap::CoreBudget>()};
    const auto baseline{budget->used()};
    const std::string input{"(cn=" + std::string(512, 'x') + ")"};
    ldap::FormatOptions options{ldap::OutputMode::Compact}; options.budget = budget;
    const auto verify = [&](auto make, auto view) {
        bool completed{};
        for (std::ptrdiff_t skip{}; skip != 16 && !completed; ++skip) {
            {
                failAllocationAfter.store(skip);
                const auto result{make(input, options)};
                const bool injected{failAllocationAfter.exchange(-1) == -1};
                if (injected) {
                    require(!result.ok() && result.error->code == ldap::LdapErrorCode::ResourceFailure
                        && view(result).empty(), "Output allocation failure escaped or returned bytes");
                } else {
                    require(result.ok() && view(result) == input, "Output allocation success changed bytes");
                    completed = true;
                }
            }
            require(budget->used() == baseline, "Output allocation failure retained reservations");
        }
        require(completed, "Allocation fault sweep never reached successful output");
    };
    verify([](std::string_view source, ldap::FormatOptions config) { return ldap::formatFilter(source, config); },
        [](const ldap::FormatResult& result) { return result.textView(); });
    verify([](std::string_view source, ldap::FormatOptions config) { return ldap::parseSimpleEquality(source, config); },
        [](const ldap::ParseResult& result) { return result.compactView(); });
}

struct StopAt {
    std::stop_source source{};
    ldap::CorePhase phase{};
    bool observed{};
    std::size_t minimumOffset{};
};
void stopAt(ldap::CorePhase phase, std::size_t offset, void* context) noexcept {
    auto& signal{*static_cast<StopAt*>(context)};
    if (phase == signal.phase && offset >= signal.minimumOffset) {
        signal.observed = true; signal.source.request_stop();
    }
}

void failAtCount(ldap::CorePhase phase, std::size_t, void*) noexcept {
    if (phase == ldap::CorePhase::Count) { failNextAllocation.store(true); }
}

template<class Factory, class Text> void resultMoveAssignments(Factory make, Text text) {
    const std::string shortFilter{"(a=)"};
    const std::string longFilter{"(cn=" + std::string(4096, 'x') + ")"};
    for (const std::string_view from : {std::string_view{shortFilter}, std::string_view{longFilter}}) {
        for (const std::string_view to : {std::string_view{shortFilter}, std::string_view{longFilter}}) {
            auto sourceBudget{std::make_shared<ldap::CoreBudget>()};
            auto targetBudget{std::make_shared<ldap::CoreBudget>()};
            const auto sourceBaseline{sourceBudget->used()};
            const auto targetBaseline{targetBudget->used()};
            {
                ldap::FormatOptions sourceOptions{ldap::OutputMode::Compact};
                sourceOptions.budget = sourceBudget;
                auto targetOptions{sourceOptions};
                targetOptions.budget = targetBudget;
                auto source{make(from, sourceOptions)};
                auto target{make(to, targetOptions)};
                require(source.ok() && target.ok() && sourceBudget->used() > sourceBaseline
                    && targetBudget->used() > targetBaseline, "Move assignment fixtures lacked storage");
                const auto sourceCharge{sourceBudget->used()};
                target = std::move(source);
                require(target.ok() && text(target) == from && targetBudget->used() == targetBaseline
                    && sourceBudget->used() == sourceCharge,
                    "Move assignment lost output or retained overwritten reservations");
            }
            require(sourceBudget->used() == sourceBaseline && targetBudget->used() == targetBaseline,
                "Move assignment budgets did not restore their baselines");
        }
    }
}

void quotasAndCancellation() {
    using ldap::LdapErrorCode;
    const ldap::FormatOptions compact{ldap::OutputMode::Compact};
    const std::string maximum{"(cn=" + std::string(ldap::MaxInputBytes - 5, 'x') + ")"};
    formatted(maximum, compact, maximum);
    valid(maximum);
    formatInvalid(maximum + "x", LdapErrorCode::InputTooLarge, ldap::MaxInputBytes);
    invalid(maximum + "x", LdapErrorCode::InputTooLarge, ldap::MaxInputBytes);
    auto invalidOversize{maximum + "x"};
    invalidOversize.front() = static_cast<char>(0xC0);
    formatInvalid(invalidOversize, LdapErrorCode::InputTooLarge, ldap::MaxInputBytes);
    const std::string prettyExact{"(&(cn=" + std::string(ldap::MaxOutputBytes - 14, 'x') + "))"};
    {
        const auto result{ldap::formatFilter(prettyExact)};
        require(result.ok() && result.textView().size() == ldap::MaxOutputBytes, "Pretty exact quota failed");
        if (result.ok()) { formatted(result.textView(), {}, result.textView()); }
    }
    const std::string prettyOverflow{"(&(cn=" + std::string(ldap::MaxOutputBytes - 13, 'x') + "))"};
    formatInvalid(prettyOverflow, LdapErrorCode::OutputTooLarge, prettyOverflow.size() - 1);
    formatted(prettyOverflow, compact, prettyOverflow);
    const ldap::FormatOptions lf{ldap::OutputMode::Pretty, ldap::Eol::Lf};
    const auto lfResult{ldap::formatFilter(prettyOverflow, lf)};
    require(lfResult.ok() && lfResult.textView().size() == ldap::MaxOutputBytes - 1, "LF quota count failed");
    const std::string lfExact{"(&(cn=" + std::string(ldap::MaxOutputBytes - 12, 'x') + "))"};
    const auto lfExactResult{ldap::formatFilter(lfExact, lf)};
    require(lfExactResult.ok() && lfExactResult.textView().size() == ldap::MaxOutputBytes,
        "LF inclusive output maximum failed");
    const std::string lfOverflow{"(&(cn=" + std::string(ldap::MaxOutputBytes - 11, 'x') + "))"};
    const auto lfOverflowResult{ldap::formatFilter(lfOverflow, lf)};
    require(!lfOverflowResult.ok() && lfOverflowResult.textView().empty()
        && lfOverflowResult.error->code == LdapErrorCode::OutputTooLarge
        && lfOverflowResult.error->offset == lfOverflow.size() - 1,
        "LF output maximum plus one lacked exact diagnostic");

    std::string dense{"(&"};
    dense.reserve(ldap::MaxInputBytes);
    while (dense.size() + 5 <= ldap::MaxInputBytes) { dense.append("(a=)"); }
    dense.push_back(')');
    auto budget{std::make_shared<ldap::CoreBudget>()};
    const auto baseline{budget->used()};
    auto options{compact};
    options.budget = budget;
    {
        auto snapshot{budget->reserve(maximum.capacity() + 1)};
        require(snapshot.has_value(), "Snapshot reservation failed");
        options.inputReservation = &*snapshot;
        auto delivery{budget->reserve(ldap::MaxCoreBytes - baseline - snapshot->bytes() - ldap::MaxOutputBytes - 16384)};
        require(delivery.has_value(), "Delivery reservation failed");
        auto result{ldap::formatFilter(maximum, options)};
        require(result.ok(), "Externally reserved input rejected");
        const auto charged{budget->used()};
        require(charged >= maximum.capacity() + 1 + result.text->capacity() + 1,
            "Capacities and terminators were not charged");
        auto moved{std::move(result)};
        require(moved.ok() && moved.textView() == maximum && budget->used() == charged,
            "Moving result released live storage");
        auto textOnly{std::move(moved.text)};
        require(textOnly && *textOnly == maximum && budget->used() == charged,
            "Moving text released its allocation reservation");
    }
    require(budget->used() == baseline, "Results/snapshot leaked reservations");
    options.inputReservation = nullptr;
    {
        std::shared_ptr<ldap::BudgetString> extracted{};
        std::size_t extractedCharge{};
        {
            auto shortResult{ldap::formatFilter("(a=)", options)};
            require(shortResult.ok(), "Short extracted-text fixture failed");
            extractedCharge = budget->used() - baseline - sizeof(ldap::FormatResult);
            extracted = std::move(shortResult.text);
        }
        require(extracted && *extracted == "(a=)" && budget->used() == baseline + extractedCharge,
            "Extracted short text lost its allocation or budget metadata charge");
    }
    require(budget->used() == baseline, "Extracted short text leaked storage");
    resultMoveAssignments(
        [](std::string_view input, ldap::FormatOptions opts) { return ldap::formatFilter(input, opts); },
        [](const ldap::FormatResult& result) { return std::string_view{result.textView()}; });
    resultMoveAssignments(
        [](std::string_view input, ldap::FormatOptions opts) { return ldap::parseSimpleEquality(input, opts); },
        [](const ldap::ParseResult& result) { return std::string_view{result.compactView()}; });
    {
        const std::string_view source{"(cn=x)"};
        auto foreignBudget{std::make_shared<ldap::CoreBudget>()};
        const auto foreignBaseline{foreignBudget->used()};
        auto foreign{foreignBudget->reserve(source.size())};
        auto undersized{budget->reserve(source.size() - 1)};
        const auto localCharge{budget->used()};
        const auto foreignCharge{foreignBudget->used()};
        for (const auto* reservation : {&*foreign, &*undersized}) {
            options.inputReservation = reservation;
            const auto formattedResult{ldap::formatFilter(source, options)};
            const auto parsedResult{ldap::parseSimpleEquality(source, options)};
            require(!formattedResult.ok() && formattedResult.textView().empty()
                && formattedResult.error->code == LdapErrorCode::MemoryExceeded
                && !parsedResult.ok() && parsedResult.compactView().empty()
                && parsedResult.error->code == LdapErrorCode::MemoryExceeded,
                "Foreign/undersized reservation was not rejected by both public operations");
            require(budget->used() == localCharge && foreignBudget->used() == foreignCharge
                && foreign->bytes() == source.size() && undersized->bytes() == source.size() - 1,
                "Rejected input reservation altered one of the budgets");
        }
        options.inputReservation = nullptr;
        foreign.reset();
        require(foreignBudget->used() == foreignBaseline, "Foreign reservation did not release");
    }
    require(budget->used() == baseline, "Undersized reservation did not release");
    {
        failNextAllocation.store(true);
        const auto result{ldap::formatFilter("(cn=x)")};
        require(!failNextAllocation.load() && !result.ok() && result.textView().empty()
            && result.error->code == LdapErrorCode::ResourceFailure,
            "System failure creating default budget was conflated with quota exhaustion");
        failNextAllocation.store(true);
        const auto parsed{ldap::parseSimpleEquality("(cn=x)")};
        require(!failNextAllocation.load() && !parsed.ok() && parsed.compactView().empty()
            && parsed.error->code == LdapErrorCode::ResourceFailure,
            "Simple parser default-budget allocation failure was not ResourceFailure");
        auto faultOptions{options};
        faultOptions.checkpoint = failAtCount;
        const auto outputFailure{ldap::formatFilter("(cn=x)", faultOptions)};
        require(!failNextAllocation.load() && !outputFailure.ok() && outputFailure.textView().empty()
            && outputFailure.error->code == LdapErrorCode::ResourceFailure
            && budget->used() == baseline, "System output allocation failure leaked budget or had wrong error");
    }
    {
        const auto syntax{ldap::formatFilter("(&(cn=x)", options)};
        require(!syntax.ok() && syntax.textView().empty(), "Shared-budget syntax failure produced output");
    }
    require(budget->used() == baseline, "Syntax failure leaked reservations");
    {
        const auto outputError{ldap::formatFilter(prettyOverflow,
            {ldap::OutputMode::Pretty, ldap::Eol::CrLf, {}, budget})};
        require(!outputError.ok() && outputError.textView().empty()
            && outputError.error->code == LdapErrorCode::OutputTooLarge,
            "Shared-budget output overflow produced text");
    }
    require(budget->used() == baseline, "Output overflow leaked reservations");
    {
        const auto result{ldap::formatFilter(dense, options)};
        require(result.ok() && result.textView() == dense, "Dense leaves exhausted memory or changed bytes");
        require(budget->used() <= ldap::MaxCoreBytes, "Memory quota exceeded");
    }
    {
        auto occupied{budget->reserve(budget->remaining())};
        require(occupied.has_value() && !budget->reserve(1), "Budget inclusive maximum failed");
        require(budget->peak() == ldap::MaxCoreBytes, "Peak missed inclusive limit or changed on rejection");
        const auto result{ldap::formatFilter("(cn=x)", options)};
        require(!result.ok() && result.textView().empty() && result.error->code == LdapErrorCode::MemoryExceeded,
            "Exhausted budget did not return typed memory error");
        require(budget->used() == ldap::MaxCoreBytes, "Failure leaked/released external reservation");
    }
    require(budget->used() == baseline && budget->peak() == ldap::MaxCoreBytes,
        "External reservation leaked or release reduced peak");
    {
        auto occupied{budget->reserve(ldap::MaxCoreBytes - maximum.size() - 8192)};
        const auto result{ldap::formatFilter(maximum, options)};
        require(!result.ok() && result.textView().empty() && result.error->code == LdapErrorCode::MemoryExceeded,
            "Output allocation escaped shared budget");
        require(budget->used() == baseline + occupied->bytes(), "Failed allocation leaked reservations");
    }
    {
        auto concurrentBudget{std::make_shared<ldap::CoreBudget>()};
        const auto concurrentBaseline{concurrentBudget->used()};
        require(concurrentBudget->peak() == concurrentBaseline, "Initial peak differs from baseline");
        require(!concurrentBudget->reserve(ldap::MaxCoreBytes)
            && concurrentBudget->peak() == concurrentBaseline, "Rejected acquisition changed initial peak");
        std::barrier rendezvous{3};
        std::atomic<bool> failed{};
        const auto exercise = [&concurrentBudget, &failed, &rendezvous] {
            { auto held{concurrentBudget->reserve(1024)};
              if (!held) { failed.store(true); }
              rendezvous.arrive_and_wait(); }
            for (std::size_t i{}; i < 1000; ++i) {
                auto reservation{concurrentBudget->reserve(1024)};
                if (concurrentBudget->used() > ldap::MaxCoreBytes) { failed.store(true); }
                if (reservation) {
                    auto moved{std::move(*reservation)};
                    if (moved.bytes() != 1024) { failed.store(true); }
                }
            }
        };
        { std::jthread first{exercise}; std::jthread second{exercise}; std::jthread third{exercise}; }
        require(!failed.load() && concurrentBudget->used() == concurrentBaseline
            && concurrentBudget->peak() == concurrentBaseline + 3072, "Concurrent reservations broke accounting/peak");
        const auto stressLimit = [&concurrentBudget, &failed] {
            for (std::size_t i{}; i < 1000; ++i) {
                auto reservation{concurrentBudget->reserve(ldap::MaxCoreBytes / 2)};
                if (concurrentBudget->used() > ldap::MaxCoreBytes) { failed.store(true); }
                if (reservation) {
                    auto moved{std::move(*reservation)};
                    if (moved.bytes() != ldap::MaxCoreBytes / 2) { failed.store(true); }
                }
            }
        };
        { std::jthread first{stressLimit}; std::jthread second{stressLimit}; std::jthread third{stressLimit}; }
        require(!failed.load() && concurrentBudget->used() == concurrentBaseline
            && concurrentBudget->peak() == concurrentBaseline + ldap::MaxCoreBytes / 2,
            "Concurrent limit/rejections broke accounting/peak");
    }

    const std::string attribute{"(" + std::string(8192, 'a') + "=x)"};
    const std::string rule{"(:" + std::string(8192, 'a') + ":=x)"};
    const std::string whitespace{"(&" + std::string(8192, ' ') + "(cn=x))"};
    const std::pair<ldap::CorePhase, std::string_view> cases[]{
        {ldap::CorePhase::Utf8, maximum}, {ldap::CorePhase::Attribute, attribute},
        {ldap::CorePhase::MatchingRule, rule}, {ldap::CorePhase::Value, maximum},
        {ldap::CorePhase::Whitespace, whitespace}, {ldap::CorePhase::Structure, dense},
        {ldap::CorePhase::Count, dense}, {ldap::CorePhase::Copy, maximum},
        {ldap::CorePhase::Publish, "(cn=x)"},
    };
    for (const auto& [phase, source] : cases) {
        StopAt signal{{}, phase, false, phase == ldap::CorePhase::Copy ? 1024U : 0U};
        options.stop = signal.source.get_token();
        options.checkpoint = stopAt;
        options.checkpointContext = &signal;
        const auto result{ldap::formatFilter(source, options)};
        if (!signal.observed) { std::cerr << "Unobserved phase: " << static_cast<int>(phase) << '\n'; }
        require(signal.observed && !result.ok() && result.textView().empty()
            && result.error->code == LdapErrorCode::Cancelled && result.error->offset <= source.size()
            && (phase != ldap::CorePhase::Copy || result.error->offset == 1025),
            "Controlled cancellation phase failed");
        require(budget->used() == baseline, "Cancellation leaked temporary/output reservations");
    }
    {
        const std::string nested{"(&(cn=" + std::string(8192, 'x') + "))"};
        constexpr std::size_t leafBegin{3};
        StopAt nestedSignal{{}, ldap::CorePhase::Copy, false, leafBegin + 1024};
        options.stop = nestedSignal.source.get_token();
        options.checkpointContext = &nestedSignal;
        const auto result{ldap::formatFilter(nested, options)};
        require(nestedSignal.observed && !result.ok() && result.textView().empty()
            && result.error->code == LdapErrorCode::Cancelled
            && result.error->offset == leafBegin + 1024 && budget->used() == baseline,
            "Nested leaf copy lost source offset or leaked reservations");
    }
    StopAt simpleSignal{{}, ldap::CorePhase::Copy, false, 1024};
    options.stop = simpleSignal.source.get_token();
    options.checkpointContext = &simpleSignal;
    const auto simple{ldap::parseSimpleEquality(maximum, options)};
    require(simpleSignal.observed && !simple.ok() && simple.compactView().empty()
        && simple.error->code == LdapErrorCode::Cancelled && simple.error->offset == 1024
        && budget->used() == baseline,
        "Simple equality cancellation leaked or produced output");
    std::stop_source stopped{};
    stopped.request_stop();
    options.stop = stopped.get_token();
    options.checkpoint = nullptr;
    const auto preStopped{ldap::formatFilter("(cn=x)", options)};
    require(!preStopped.ok() && preStopped.error->code == LdapErrorCode::Cancelled
        && preStopped.error->offset == 0 && budget->used() == baseline, "Entry cancellation failed");
    options.stop = {};
    { const auto fresh{ldap::formatFilter("(cn=x)", options)};
      require(fresh.ok(), "Fresh operation after cancellation failed"); }
    require(budget->used() == baseline, "Fresh operation leaked reservations");
}
} // namespace

int main() {
    allocatorLifecycle();
    outputAllocationFailures();
    conformanceProperties();
    quotasAndCancellation();
    using ldap::LdapErrorCode;
    valid("(cn=John Doe)");
    valid("(cn= John Doe  )");
    valid("(cn=)");
    valid("(cn=a=b)");
    valid("(cn;lang-en=Jose)");
    valid("(cn;lang-en;binary=Jose)");
    valid("(cn;9bin=Jose)");
    valid("(cn;-lang=Jose)");
    valid("(2.5.4.3=Jose)");
    valid("(0.0=Jose)");
    valid("(12345678901234567890.0=Jose)");
    valid("(cn=\\28\\29\\2A\\5C\\00)");
    valid("(cn=\\c4\\8d)");
    valid("(cn=\\aF\\0a)");
    valid("(cn=\\ff)"); // Escaped binary octet remains textual UTF-8.
    valid("(cn=x\\29 y\\28z)");
    valid("(cn=Jos\xC3\xA9)");
    valid("(cn=\xC2\x80)");
    valid("(cn=\xE0\xA0\x80)");
    valid("(cn=\xF0\x9F\x98\x80)");
    valid("(cn=\xF4\x8F\xBF\xBF)");
    valid("(cn=John\tDoe)");

    invalid("", LdapErrorCode::InvalidSyntax, 0);
    invalid("cn=a)", LdapErrorCode::InvalidSyntax, 0);
    invalid("(cn=a", LdapErrorCode::InvalidSyntax, 5);
    invalid("(cn=a) ", LdapErrorCode::InvalidSyntax, 6);
    invalid(" (cn=a)", LdapErrorCode::InvalidSyntax, 0);
    invalid("(=a)", LdapErrorCode::InvalidAttribute, 1);
    invalid("(cn =a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(1cn=a)", LdapErrorCode::InvalidAttribute, 1);
    invalid("(2.05.4=a)", LdapErrorCode::InvalidAttribute, 4);
    invalid("(2.5.=a)", LdapErrorCode::InvalidAttribute, 5);
    invalid("(2..5=a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(cn_name=a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(cn;=a)", LdapErrorCode::InvalidAttribute, 4);
    invalid("(cn=a*b)", LdapErrorCode::UnsupportedFilter, 5);
    invalid("(cn=*)", LdapErrorCode::UnsupportedFilter, 4);
    invalid("(&(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(|(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(!(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(cn>=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn<=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn~=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn:=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(:1.2.3:=a)", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(cn=a(b)", LdapErrorCode::InvalidSyntax, 5);
    invalid("(cn=a)b)", LdapErrorCode::InvalidSyntax, 6);
    invalid("(cn=\\2)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\GG)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\0)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\)", LdapErrorCode::InvalidEscape, 4);
    invalid(std::string{"(cn=a\0b)", 8}, LdapErrorCode::RawNul, 5);
    invalid(std::string{"(cn=\xC0\xAF)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xC1\xBF)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xE0\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xF0\x80\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xE2\x82)"}, LdapErrorCode::InvalidUtf8, 6);
    invalid(std::string{"(cn=\xED\xA0\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xF4\x90\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xE2\x28\xA1)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\x80)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xFE)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xC2"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xE2\x82"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xF0\x9F\x98"}, LdapErrorCode::InvalidUtf8, 4);
    // UTF-8 prevalece sobre el error sintáctico anterior de falta de '('.
    invalid(std::string{"bad=\xC0"}, LdapErrorCode::InvalidUtf8, 4);

    const ldap::FormatOptions compact{ldap::OutputMode::Compact, ldap::Eol::CrLf};
    const ldap::FormatOptions prettyCrLf{ldap::OutputMode::Pretty, ldap::Eol::CrLf};
    const ldap::FormatOptions prettyLf{ldap::OutputMode::Pretty, ldap::Eol::Lf};
    const ldap::FormatOptions prettyCr{ldap::OutputMode::Pretty, ldap::Eol::Cr};
    const std::string_view group{"(&(cn=John Doe)(|(sn=\\28x\\29)(uid=a)))"};
    formatted(group, compact, group);
    formatted(group, prettyCrLf,
        "(&\r\n  (cn=John Doe)\r\n  (|\r\n    (sn=\\28x\\29)\r\n    (uid=a)\r\n  )\r\n)");
    formatted(group, prettyLf,
        "(&\n  (cn=John Doe)\n  (|\n    (sn=\\28x\\29)\n    (uid=a)\n  )\n)");
    formatted("(!(cn=\\2A\\5C\\00))", compact, "(!(cn=\\2A\\5C\\00))");
    formatted("(!(cn= a  b ))", prettyCr, "(!\r  (cn= a  b )\r)");
    formatted("(& \t\n (cn= John  Doe )\r\n (sn=\\2A)\t)", compact,
        "(&(cn= John  Doe )(sn=\\2A))");
    formatted("(& \t\n (cn= John  Doe )\r\n (sn=\\2A)\t)", prettyLf,
        "(&\n  (cn= John  Doe )\n  (sn=\\2A)\n)");
    idempotentModes("( & (cn=foo) (sn=bar) )", "(&(cn=foo)(sn=bar))",
        "(&\r\n  (cn=foo)\r\n  (sn=bar)\r\n)", ldap::Eol::CrLf);
    idempotentModes("(\t|\n(cn=John Doe)\r(sn=Jos\xC3\xA9\\2A) )",
        "(|(cn=John Doe)(sn=Jos\xC3\xA9\\2A))",
        "(|\n  (cn=John Doe)\n  (sn=Jos\xC3\xA9\\2A)\n)", ldap::Eol::Lf);
    idempotentModes("( \t!\n(cn=a) )", "(!(cn=a))",
        "(!\n  (cn=a)\n)", ldap::Eol::Lf);
    idempotentModes("(\r&(cn=a)(sn=b))", "(&(cn=a)(sn=b))",
        "(&\r\n  (cn=a)\r\n  (sn=b)\r\n)", ldap::Eol::CrLf);
    idempotentModes("(\n|(cn=a)(sn=b))", "(|(cn=a)(sn=b))",
        "(|\n  (cn=a)\n  (sn=b)\n)", ldap::Eol::Lf);
    formatted("(cn= John  Doe )", prettyLf, "(cn= John  Doe )");
    for (const auto space : {" ", "\t", "\r\n", "\n", "\r", " \t\r\n "}) {
        const std::string leaf{"(" + std::string{space} + "cn= John\r\n Doe \\20 )"};
        idempotentModes(" \t\r\n" + leaf + "\n ", "(cn= John\r\n Doe \\20 )",
            "(cn= John\r\n Doe \\20 )", ldap::Eol::Lf);
        idempotentModes(" \n(&" + leaf + "(" + space + "age>=21))\r\n",
            "(&(cn= John\r\n Doe \\20 )(age>=21))",
            "(&\r\n  (cn= John\r\n Doe \\20 )\r\n  (age>=21)\r\n)", ldap::Eol::CrLf);
        formatted("(" + std::string{space} + ":rule:= value )", compact, "(:rule:= value )");
    }
    formatInvalid(" \r\n( \ncn_name=x) ", LdapErrorCode::InvalidAttribute, 8);
    formatInvalid(" \n( \ncn=\\GG) ", LdapErrorCode::InvalidEscape, 8);
    formatInvalid(" \n( cn=\xC0) ", LdapErrorCode::InvalidUtf8, 7);
    formatInvalid("\v(cn=x)", LdapErrorCode::InvalidSyntax, 0);
    formatInvalid(" (cn=x) text ", LdapErrorCode::InvalidSyntax, 8);
    const auto defaultPretty{ldap::formatFilter("(&(cn=a)(sn=b))")};
    if (!defaultPretty.ok() || defaultPretty.textView() != "(&\r\n  (cn=a)\r\n  (sn=b)\r\n)") {
        std::cerr << "Default mode must be Pretty with CRLF\n";
        ++failures;
    }

    formatInvalid("(&)", LdapErrorCode::EmptyGroup, 2);
    formatInvalid("(|   )", LdapErrorCode::EmptyGroup, 5);
    formatInvalid("(!)", LdapErrorCode::EmptyGroup, 2);
    formatInvalid("(!(cn=a)(sn=b))", LdapErrorCode::InvalidGroupArity, 8);
    formatInvalid("(&(cn=a)", LdapErrorCode::UnbalancedParenthesis, 8);
    formatInvalid("(&(cn=a", LdapErrorCode::UnbalancedParenthesis, 7);
    formatInvalid("(&(cn=a)))", LdapErrorCode::UnbalancedParenthesis, 9);
    formatted("(&(cn=*))", compact, "(&(cn=*))");
    formatted("(cn~=Ana)", compact, "(cn~=Ana)");
    formatted("(age>=21)", compact, "(age>=21)");
    formatted("(age<=65)", compact, "(age<=65)");
    formatted("(cn=foo*bar*baz)", compact, "(cn=foo*bar*baz)");
    formatted("(cn=*foo*)", compact, "(cn=*foo*)");
    formatted("(cn=foo**bar)", compact, "(cn=foo**bar)");
    formatted("(cn=\\2a*)", compact, "(cn=\\2a*)");
    formatted("(cn=\\2A)", compact, "(cn=\\2A)");
    formatted("(cn=*)", prettyLf, "(cn=*)");
    formatted("(:dn:rule:=x)", compact, "(:dn:rule:=x)");
    formatted("(cn:dn:rule:=x)", compact, "(cn:dn:rule:=x)");
    formatted("(cn:=x)", compact, "(cn:=x)");
    formatted("(:rule:=x)", compact, "(:rule:=x)");
    formatted("(:DN:2.4.6.8.10:=x)", compact, "(:DN:2.4.6.8.10:=x)");
    formatted("(cn:dn:=x)", compact, "(cn:dn:=x)");
    formatted("(cn:caseExactMatch:=Fred Flintstone)", compact,
        "(cn:caseExactMatch:=Fred Flintstone)");
    formatted("(cn:rule:=\\2a)", compact, "(cn:rule:=\\2a)");
    formatted("(:1.2.3:=Wilma Flintstone)", compact,
        "(:1.2.3:=Wilma Flintstone)");
    formatted("(cn= Ana \\28x\\29\\2a\\5c\\00 )", compact,
        "(cn= Ana \\28x\\29\\2a\\5c\\00 )");
    formatted("(&(cn~= Ana )(sn=foo*bar)(:dn:rule:=x))", prettyLf,
        "(&\n  (cn~= Ana )\n  (sn=foo*bar)\n  (:dn:rule:=x)\n)");
    const std::string_view mixedAssertions{
        "(&(|(cn=foo*bar)(age>=21))(!(cn:dn:rule:= Ana \\2a )))"};
    const std::string_view mixedPretty{
        "(&\r\n  (|\r\n    (cn=foo*bar)\r\n    (age>=21)\r\n  )\r\n"
        "  (!\r\n    (cn:dn:rule:= Ana \\2a )\r\n  )\r\n)"};
    formatted(mixedAssertions, prettyCrLf, mixedPretty);
    formatted(mixedPretty, compact, mixedAssertions);
    formatInvalid("(cn>21)", LdapErrorCode::InvalidSyntax, 3);
    formatInvalid("(cn~=a*b)", LdapErrorCode::InvalidSyntax, 6);
    formatInvalid("(cn>=a*b)", LdapErrorCode::InvalidSyntax, 6);
    formatInvalid("(cn<=a*b)", LdapErrorCode::InvalidSyntax, 6);
    formatInvalid("(cn:rule:=a*b)", LdapErrorCode::InvalidSyntax, 11);
    formatInvalid("(:dn:=x)", LdapErrorCode::InvalidAttribute, 4);
    formatInvalid("(:=x)", LdapErrorCode::InvalidAttribute, 1);
    formatInvalid("(cn:dn:)", LdapErrorCode::InvalidAttribute, 7);
    formatInvalid("(cn:rule=x)", LdapErrorCode::InvalidSyntax, 8);
    formatInvalid("(cn:rule:dn:=x)", LdapErrorCode::InvalidSyntax, 8);
    formatInvalid("(cn:bad;option:=x)", LdapErrorCode::InvalidAttribute, 7);
    formatInvalid("(cn:1..2:=x)", LdapErrorCode::InvalidAttribute, 6);
    formatInvalid("(cn=abc\\2)", LdapErrorCode::InvalidEscape, 7);
    formatInvalid("(cn=\\GG)", LdapErrorCode::InvalidEscape, 4);
    formatInvalid("(cn>=\\GG)", LdapErrorCode::InvalidEscape, 5);
    formatInvalid("(cn<=\\2)", LdapErrorCode::InvalidEscape, 5);
    formatInvalid("(cn~=\\)", LdapErrorCode::InvalidEscape, 5);
    formatInvalid("(cn:rule:=\\GG)", LdapErrorCode::InvalidEscape, 10);
    formatInvalid("(cn:rule:=\\2)", LdapErrorCode::InvalidEscape, 10);
    formatInvalid(std::string{"(cn>=\0)", 7}, LdapErrorCode::RawNul, 5);
    formatInvalid(std::string{"(cn:rule:=\0)", 12}, LdapErrorCode::RawNul, 10);
    formatted("(cn>=\\2A)", compact, "(cn>=\\2A)");
    formatted("(cn<=\\2a)", compact, "(cn<=\\2a)");
    formatted("(cn~=\\2A)", compact, "(cn~=\\2A)");
    formatInvalid(std::string{"(cn=\xC0\xAF)"}, LdapErrorCode::InvalidUtf8, 4);
    formatInvalid("(&(cn_name=a))", LdapErrorCode::InvalidAttribute, 5);
    formatInvalid("(&(cn=\\GG))", LdapErrorCode::InvalidEscape, 6);
    formatInvalid(std::string{"(&(cn=a\0b))", 11}, LdapErrorCode::RawNul, 7);
    formatted(" (cn=a)", compact, "(cn=a)");
    formatted("(cn=a) ", compact, "(cn=a)");
    formatInvalid("( cn = foo )", LdapErrorCode::InvalidAttribute, 4);
    formatInvalid("( \t\r\n", LdapErrorCode::UnbalancedParenthesis, 5);
    formatInvalid("( \t)", LdapErrorCode::InvalidSyntax, 3);
    formatted("( \nX=foo)", compact, "(X=foo)");
    formatInvalid(std::string{"bad=\xC0"}, LdapErrorCode::InvalidUtf8, 4);

    formatted(nestedNot(5), compact, nestedNot(5));
    const auto maxDepth{nestedNot(128)};
    formatted(maxDepth, compact, maxDepth);
    const auto maxPretty{ldap::formatFilter(maxDepth, prettyLf)};
    if (!maxPretty.ok()) {
        std::cerr << "Pretty rejected 128 filter levels\n";
        ++failures;
    } else {
        formatted(maxPretty.textView(), prettyLf, maxPretty.textView());
        formatted(maxPretty.textView(), compact, maxDepth);
    }
    const auto tooDeep{nestedNot(129)};
    formatInvalid(tooDeep, LdapErrorCode::DepthExceeded, 2 * 128);

    if (failures != 0) { return EXIT_FAILURE; }
    std::cout << "LDAP core conformance: all cases passed\n";
    return EXIT_SUCCESS;
}
