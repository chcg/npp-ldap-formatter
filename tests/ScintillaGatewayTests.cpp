#include "ScintillaGateway.hpp"
#include "PluginDefinition.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <cstdlib>
#include <new>
#include <atomic>

// Test executable only: fail the next allocation of an explicitly selected size.
// The shared budget exists before arming this fault, so it targets snapshot storage.
namespace { std::atomic<std::size_t> failAllocationSize{}; }
_Ret_notnull_ _Post_writable_byte_size_(bytes) void* operator new(std::size_t bytes) {
    auto expected{bytes};
    if (failAllocationSize.compare_exchange_strong(expected, 0)) { throw std::bad_alloc{}; }
    if (auto* allocation{std::malloc(bytes == 0 ? 1 : bytes)}) { return allocation; }
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

namespace {
struct State {
    std::string text{"(cn=Jos\xc3\xa9)\r\n"};
    Sci_Position anchor{}, caret{};
    int active{}, codepage{SC_CP_UTF8}, eol{SC_EOL_CRLF}, selections{1}, selectionMode{SC_SEL_STREAM};
    bool readonly{}, undo{true}, virtualSpace{}, badCount{}, badNul{}, changed{}, statusFailure{}, changeOnRead{};
    int changeIdentity{};
    bool copied{};
    std::optional<Sci_Position> lengthOverride{}, beginOverride{}, endOverride{};
    bool nullDocument{}, nullBuffer{}, wrongTextCapacity{};
    sptr_t status{};
    std::size_t mainQueries{}, secondQueries{}, mutations{};
    bool hostSuccess{true};
    HWND host{}, main{}, second{};
} state;
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == NPPM_GETCURRENTSCINTILLA) {
        *reinterpret_cast<int*>(l) = state.active + (state.copied && state.changeIdentity == 3 ? 1 : 0);
        return state.hostSuccess ? TRUE : FALSE;
    }
    if (message == NPPM_GETCURRENTBUFFERID) { return state.nullBuffer ? 0 : 42 + (state.copied && state.changeIdentity == 2 ? 1 : 0); }
    if (message < 2000 || message > 3000) { return DefWindowProcW(hwnd, message, w, l); }
    if (hwnd == state.main) { ++state.mainQueries; } else { ++state.secondQueries; }
    switch (message) {
    case SCI_SETSTATUS: state.status = static_cast<sptr_t>(w); return 0;
    case SCI_GETSTATUS: return state.status;
    case SCI_GETDOCPOINTER: return state.nullDocument ? 0 : 123 + (state.copied && state.changeIdentity == 1 ? 1 : 0);
    case SCI_GETLENGTH: return state.lengthOverride.value_or(static_cast<Sci_Position>(state.text.size()) + (state.changed ? 1 : 0));
    case SCI_GETANCHOR: return state.anchor;
    case SCI_GETCURRENTPOS: return state.caret;
    case SCI_GETSELECTIONSTART: return state.beginOverride.value_or(std::min(state.anchor, state.caret));
    case SCI_GETSELECTIONEND: return state.endOverride.value_or(std::max(state.anchor, state.caret));
    case SCI_GETCODEPAGE: return state.codepage;
    case SCI_GETEOLMODE: return state.eol;
    case SCI_GETREADONLY: return state.readonly;
    case SCI_GETUNDOCOLLECTION: return state.undo;
    case SCI_GETSELECTIONS: return state.selections;
    case SCI_GETSELECTIONMODE: return state.selectionMode;
    case SCI_GETSELECTIONNANCHORVIRTUALSPACE:
    case SCI_GETSELECTIONNCARETVIRTUALSPACE: return state.virtualSpace;
    case SCI_GETSELTEXT:
    case SCI_GETTEXT: {
        const auto begin{message == SCI_GETTEXT ? std::size_t{} : static_cast<std::size_t>(std::min(state.anchor, state.caret))};
        const auto length{message == SCI_GETTEXT ? state.text.size() : static_cast<std::size_t>(std::max(state.anchor, state.caret)) - begin};
        const auto count{message == SCI_GETTEXT ? std::min(length, w == 0 ? std::size_t{} : static_cast<std::size_t>(w) - 1) : length};
        if (message == SCI_GETTEXT && w != length + 1) { state.wrongTextCapacity = true; }
        if (l) {
            auto* output{reinterpret_cast<char*>(l)};
            std::memcpy(output, state.text.data() + begin, count);
            if (message != SCI_GETTEXT || w != 0) { output[count] = state.badNul ? 'x' : '\0'; }
            if (state.statusFailure) { state.status = SC_STATUS_FAILURE; }
            if (state.changeOnRead) { state.changed = true; }
            state.copied = true;
        }
        return static_cast<LRESULT>(count + (message == SCI_GETSELTEXT ? 1 : 0)) + (state.badCount ? 1 : 0);
    }
    default: ++state.mutations; return 0;
    }
}
struct Window {
    HWND handle{};
    Window() : handle{CreateWindowExW(0, L"LdapGatewayTest", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr)} {}
    ~Window() { if (handle) { DestroyWindow(handle); } }
};
}
int main() {
    WNDCLASSW klass{};
    klass.lpfnWndProc = procedure;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"LdapGatewayTest";
    if (!RegisterClassW(&klass)) { return 1; }
    Window host{}, main{}, second{};
    if (!host.handle || !main.handle || !second.handle) { return 2; }
    state.host = host.handle; state.main = main.handle; state.second = second.handle;
    ldap::ScintillaGateway gateway{};
    gateway.initialize({host.handle, main.handle, second.handle});
    auto budget{std::make_shared<ldap::CoreBudget>()};
    const auto baseline{budget->used()};
    int failures{};
    const auto check = [&](bool passed) { if (!passed) { ++failures; std::cerr << "Case failed\n"; } };
    for (int view{}; view != 2; ++view) {
        state.active = view;
        state.mainQueries = state.secondQueries = 0;
        auto result{gateway.capture(ldap::OutputMode::Compact, budget)};
        check(result.ok());
        if (result.ok()) { check(result.snapshot->text() == state.text && result.snapshot->mode == ldap::OutputMode::Compact); }
        check(view == 0 ? state.secondQueries == 0 : state.mainQueries == 0);
    }
    state.anchor = 9; state.caret = 1;
    { auto result{gateway.capture(ldap::OutputMode::Pretty, budget)};
      check(result.ok() && result.snapshot->metadata.anchor == 9 && result.snapshot->metadata.caret == 1
          && result.snapshot->text() == std::string_view{state.text}.substr(1, 8)); }
    const auto rejected = [&](ldap::CaptureErrorCode code) {
        const auto usedBefore{budget->used()};
        auto result{gateway.capture(ldap::OutputMode::Pretty, budget)};
        check(!result.ok() && result.error && result.error->code == code);
        check(budget->used() == usedBefore);
    };
    state.anchor = state.caret = 0;
    const auto malformed = [&] {
        state.copied = false;
        rejected(ldap::CaptureErrorCode::Api);
        check(!state.copied && budget->used() == baseline);
    };
    state.lengthOverride = -1; malformed(); state.lengthOverride.reset();
    state.beginOverride = -1; malformed(); state.beginOverride.reset();
    state.beginOverride = 2; state.endOverride = 1; malformed();
    state.beginOverride.reset(); state.endOverride.reset();
    state.endOverride = static_cast<Sci_Position>(state.text.size() + 1); malformed(); state.endOverride.reset();
    state.beginOverride = 1; state.endOverride = 2; malformed();
    state.beginOverride.reset(); state.endOverride.reset();
    state.anchor = -1; malformed(); state.anchor = 0;
    state.caret = -1; malformed(); state.caret = 0;
    state.anchor = static_cast<Sci_Position>(state.text.size() + 1); malformed(); state.anchor = 0;
    state.caret = static_cast<Sci_Position>(state.text.size() + 1); malformed(); state.caret = 0;
    state.eol = -1; malformed(); state.eol = 3; malformed(); state.eol = SC_EOL_CRLF;
    state.nullDocument = true; malformed(); state.nullDocument = false;
    state.nullBuffer = true; malformed(); state.nullBuffer = false;
    for (const auto caret : {std::size_t{}, state.text.size() / 2, state.text.size()}) {
        state.anchor = state.caret = static_cast<Sci_Position>(caret);
        auto result{gateway.capture(ldap::OutputMode::Pretty, budget)};
        check(result.ok() && result.snapshot->text() == state.text
            && result.snapshot->metadata.begin == 0 && result.snapshot->metadata.end == static_cast<Sci_Position>(state.text.size()));
    }
    state.anchor = state.caret = 0;
    gateway.initialize({host.handle, nullptr, second.handle});
    state.active = 0; rejected(ldap::CaptureErrorCode::Api);
    gateway.initialize({host.handle, main.handle, second.handle});
    state.hostSuccess = false; rejected(ldap::CaptureErrorCode::Api); state.hostSuccess = true;
    state.active = 2; rejected(ldap::CaptureErrorCode::Api); state.active = 0;
    state.readonly = true; rejected(ldap::CaptureErrorCode::UnsupportedState); state.readonly = false;
    state.undo = false; rejected(ldap::CaptureErrorCode::UnsupportedState); state.undo = true;
    state.selections = 2; rejected(ldap::CaptureErrorCode::UnsupportedState); state.selections = 1;
    state.selectionMode = SC_SEL_RECTANGLE; rejected(ldap::CaptureErrorCode::UnsupportedState); state.selectionMode = SC_SEL_STREAM;
    state.virtualSpace = true; rejected(ldap::CaptureErrorCode::UnsupportedState); state.virtualSpace = false;
    state.codepage = 0; rejected(ldap::CaptureErrorCode::Encoding); state.codepage = SC_CP_UTF8;
    for (const int eol : {SC_EOL_CRLF, SC_EOL_LF, SC_EOL_CR}) {
        state.eol = eol;
        state.text = eol == SC_EOL_CRLF ? "a\r\nb" : eol == SC_EOL_LF ? "a\nb" : "a\rb";
        auto result{gateway.capture(ldap::OutputMode::Pretty, budget)};
        check(result.ok() && result.snapshot->metadata.eol == eol && result.snapshot->text() == state.text);
    }
    state.badCount = true; rejected(ldap::CaptureErrorCode::Api); state.badCount = false;
    state.badNul = true; rejected(ldap::CaptureErrorCode::Api); state.badNul = false;
    state.anchor = 0; state.caret = 1;
    { auto result{gateway.capture(ldap::OutputMode::Pretty, budget)}; check(result.ok() && result.snapshot->text() == "a"); }
    state.badCount = true; rejected(ldap::CaptureErrorCode::Api); state.badCount = false;
    state.badNul = true; rejected(ldap::CaptureErrorCode::Api); state.badNul = false;
    state.anchor = state.caret = 0;
    state.statusFailure = true; rejected(ldap::CaptureErrorCode::Api); state.statusFailure = false;
    state.changeOnRead = true; rejected(ldap::CaptureErrorCode::DocumentChanged);
    state.changeOnRead = state.changed = false;
    for (int identity{1}; identity != 4; ++identity) {
        state.changeIdentity = identity; state.copied = false;
        rejected(ldap::CaptureErrorCode::DocumentChanged);
    }
    state.changeIdentity = 0;
    { ldap::CaptureResult result{};
      std::thread worker{[&] { result = gateway.capture(ldap::OutputMode::Pretty, budget); }};
      worker.join(); check(!result.ok() && result.error->code == ldap::CaptureErrorCode::Api); }
    state.text = "ab\xc0\xaf"; rejected(ldap::CaptureErrorCode::InvalidUtf8);
    { auto result{gateway.capture(ldap::OutputMode::Pretty, budget)}; check(result.error && result.error->offset == 2); }
    state.text = std::string{"a\0b", 3}; rejected(ldap::CaptureErrorCode::RawNul);
    { auto result{gateway.capture(ldap::OutputMode::Pretty, budget)}; check(result.error && result.error->offset == 1); }
    state.text.assign(77, 'a');
    failAllocationSize = 78;
    rejected(ldap::CaptureErrorCode::Resources);
    check(failAllocationSize == 0);
    state.text.assign(ldap::MaxInputBytes + 1, 'a'); rejected(ldap::CaptureErrorCode::Length);
    state.anchor = 0; state.caret = static_cast<Sci_Position>(state.text.size());
    rejected(ldap::CaptureErrorCode::Length);
    state.anchor = 1; state.caret = 2;
    { auto result{gateway.capture(ldap::OutputMode::Pretty, budget)}; check(result.ok() && result.snapshot->text() == "a"); }
    { auto reservation{budget->reserve(budget->remaining())}; rejected(ldap::CaptureErrorCode::Resources); }
    check(budget->used() == baseline);
    check(state.mutations == 0);
    state.text = "(cn=value)"; state.anchor = state.caret = 0;
    commandMenuInit();
    pluginInit({host.handle, main.handle, second.handle});
    for (int command{}; command != 2; ++command) {
        const auto originalText{state.text};
        const auto originalAnchor{state.anchor}, originalCaret{state.caret};
        state.copied = false; state.mainQueries = 0;
        pluginFunctions()[command]._pFunc();
        check(state.copied && state.mainQueries != 0 && state.mutations == 0);
        check(state.text == originalText && state.anchor == originalAnchor && state.caret == originalCaret);
    }
    pluginShutdown();
    state.copied = false;
    pluginFunctions()[0]._pFunc(); pluginFunctions()[1]._pFunc();
    check(!state.copied);
    check(!state.wrongTextCapacity);
    gateway.reset(); rejected(ldap::CaptureErrorCode::Api);
    std::cout << "Gateway simulated host: " << failures << " failures\n";
    return failures == 0 ? 0 : 3;
}
