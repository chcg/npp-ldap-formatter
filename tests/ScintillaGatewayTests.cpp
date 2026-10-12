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
namespace { std::atomic<std::size_t> failAllocationSize{}, failAllocationSkip{}; }
_Ret_notnull_ _Post_writable_byte_size_(bytes) void* operator new(std::size_t bytes) {
    auto expected{bytes};
    if (failAllocationSize.load() == bytes && failAllocationSkip.load() != 0) { --failAllocationSkip; }
    else if (failAllocationSize.compare_exchange_strong(expected, 0)) { throw std::bad_alloc{}; }
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
    std::size_t copies{};
    std::optional<Sci_Position> lengthOverride{}, beginOverride{}, endOverride{};
    bool nullDocument{}, nullBuffer{}, wrongTextCapacity{};
    sptr_t status{};
    std::size_t mainQueries{}, secondQueries{}, mutations{};
    bool hostSuccess{true};
    std::size_t begins{}, ends{}, replacements{};
    std::vector<std::string> undoTexts{}, redoTexts{};
    std::vector<int> undoTokens{}, redoTokens{};
    int pendingToken{};
    bool containerFirst{}, registerFlags{true};
    std::size_t registrations{}, addedTokens{};
    std::size_t selectionSets{};
    bool pumpContainer{};
    bool addTokenFailure{}, historyStatusFailure{};
    std::optional<sptr_t> undoCountOverride{}, undoCursorOverride{};
    sptr_t secondDocument{123};
    void (*tokenReenter)(){};
    void (*documentReenter)(){};
    std::size_t documentReenterSkip{};
    void (*statusReenter)(){};
    void (*pump)(){};
    int registeredFlags{};
    void (*notify)(const SCNotification*){};
    bool replaceFailure{}, postFailure{};
    bool undoOpen{}, recordedGroup{};
    void (*reenter)(){};
    void (*selectionReenter)(){};
    void (*finalSelectionReenter)(){};
    void (*endReenter)(){};
    HWND host{}, main{}, second{};
} state;
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == NPPM_GETCURRENTSCINTILLA) {
        *reinterpret_cast<int*>(l) = state.active + (state.copied && state.changeIdentity == 3 ? 1 : 0);
        return state.hostSuccess ? TRUE : FALSE;
    }
    if (message == NPPM_GETCURRENTBUFFERID) { return state.nullBuffer ? 0 : 42 + (state.copied && state.changeIdentity == 2 ? 1 : 0); }
    if (message == NPPM_ADDSCNMODIFIEDFLAGS) { ++state.registrations; state.registeredFlags = static_cast<int>(l); return state.registerFlags ? TRUE : FALSE; }
    if (message < 2000 || message > 3000) { return DefWindowProcW(hwnd, message, w, l); }
    if (hwnd == state.main) { ++state.mainQueries; } else { ++state.secondQueries; }
    switch (message) {
    case SCI_GETUNDOACTIONS:
        if (state.historyStatusFailure) { state.status = SC_STATUS_FAILURE; }
        return state.undoCountOverride.value_or(static_cast<sptr_t>(state.undoTexts.size() + state.redoTexts.size()));
    case SCI_GETUNDOCURRENT: return state.undoCursorOverride.value_or(static_cast<sptr_t>(state.undoTexts.size()));
    case SCI_ADDUNDOACTION:
        state.pendingToken = static_cast<int>(w); ++state.addedTokens;
        if (state.addTokenFailure) { state.status = SC_STATUS_FAILURE; }
        if (state.tokenReenter) { const auto callback{state.tokenReenter}; state.tokenReenter = nullptr; callback(); }
        return 0;
    case SCI_BEGINUNDOACTION:
        ++state.begins; state.undoOpen = true; state.recordedGroup = false; state.pendingToken = 0;
        if (state.reenter) { const auto callback{state.reenter}; state.reenter = nullptr; callback(); }
        return 0;
    case SCI_ENDUNDOACTION:
        ++state.ends; state.undoOpen = false;
        if (state.endReenter) { const auto callback{state.endReenter}; state.endReenter = nullptr; callback(); }
        return 0;
    case SCI_SETSEL:
        ++state.selectionSets;
        state.anchor = static_cast<Sci_Position>(w); state.caret = l;
        if (state.selectionReenter) { const auto callback{state.selectionReenter}; state.selectionReenter = nullptr; callback(); }
        if (state.recordedGroup && state.finalSelectionReenter) { const auto callback{state.finalSelectionReenter}; state.finalSelectionReenter = nullptr; callback(); }
        return 0;
    case SCI_GETTEXTRANGEFULL: {
        auto& range{*reinterpret_cast<Sci_TextRangeFull*>(l)};
        const auto size{static_cast<std::size_t>(range.chrg.cpMax - range.chrg.cpMin)};
        std::memcpy(range.lpstrText, state.text.data() + range.chrg.cpMin, size);
        range.lpstrText[size] = '\0';
        if (state.statusFailure) { state.status = SC_STATUS_FAILURE; }
        if (state.changeOnRead) { state.changed = true; }
        return static_cast<LRESULT>(size) + (state.badCount ? 1 : 0);
    }
    case SCI_REPLACESEL: {
        ++state.replacements;
        // Without an explicit group an adjacent edit coalesces with the preceding
        // action; BEGIN creates the boundary that this story must preserve.
        if ((state.undoOpen && !state.recordedGroup) || state.undoTexts.empty()) {
            state.undoTokens.resize(state.undoTexts.size()); state.undoTexts.push_back(state.text); state.undoTokens.push_back(state.pendingToken);
        }
        state.recordedGroup = true; state.redoTexts.clear(); state.redoTokens.clear();
        const auto begin{std::min(state.anchor, state.caret)};
        const auto size{std::max(state.anchor, state.caret) - begin};
        state.text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(size), reinterpret_cast<const char*>(l));
        state.anchor = state.caret = begin + static_cast<Sci_Position>(std::strlen(reinterpret_cast<const char*>(l)));
        if (state.replaceFailure) { state.status = SC_STATUS_FAILURE; }
        if (state.postFailure) { state.text[static_cast<std::size_t>(begin)] = 'X'; }
        return 0;
    }
    case SCI_UNDO:
    case SCI_REDO: {
        const bool undo{message == SCI_UNDO};
        auto& source{undo ? state.undoTexts : state.redoTexts};
        auto& destination{undo ? state.redoTexts : state.undoTexts};
        auto& tokens{undo ? state.undoTokens : state.redoTokens};
        auto& destinationTokens{undo ? state.redoTokens : state.undoTokens};
        if (source.empty()) { return 0; }
        tokens.resize(source.size()); const auto token{tokens.back()};
        const auto callback = [&](int flags, int callbackToken) {
            if (!state.notify) { return; }
            SCNotification notification{}; notification.nmhdr.hwndFrom = hwnd; notification.nmhdr.code = SCN_MODIFIED;
            notification.modificationType = flags | (undo ? SC_PERFORMED_UNDO : SC_PERFORMED_REDO);
            notification.token = callbackToken; state.notify(&notification);
        };
        if (state.containerFirst) { callback(SC_MOD_CONTAINER, token); }
        destination.push_back(state.text); destinationTokens.resize(destination.size() - 1); destinationTokens.push_back(token);
        state.text = source.back(); source.pop_back(); tokens.pop_back();
        // Simulate the host's native caret adjustment after its callbacks.
        state.anchor = state.caret = 0;
        callback(SC_MOD_INSERTTEXT | SC_LASTSTEPINUNDOREDO, 0);
        if (!state.containerFirst) {
            if (state.pumpContainer) { state.statusReenter = state.pump; }
            callback(SC_MOD_CONTAINER, token);
        }
        state.anchor = state.caret = static_cast<Sci_Position>(state.text.size());
        return 0;
    }
    case SCI_SETSTATUS: state.status = static_cast<sptr_t>(w); return 0;
    case SCI_GETSTATUS:
        if (state.statusReenter) { const auto callback{state.statusReenter}; state.statusReenter = nullptr; callback(); }
        return state.status;
    case SCI_GETDOCPOINTER:
        if (state.documentReenter && !state.documentReenterSkip) {
            const auto callback{state.documentReenter}; state.documentReenter = nullptr; callback();
        } else if (state.documentReenter) { --state.documentReenterSkip; }
        return state.nullDocument ? 0 : (hwnd == state.second ? state.secondDocument : 123) + (state.copied && state.changeIdentity == 1 ? 1 : 0);
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
            ++state.copies;
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
        pluginShutdown();
        pluginInit({host.handle, main.handle, second.handle});
        SCNotification ready{}; ready.nmhdr.hwndFrom = host.handle; ready.nmhdr.code = NPPN_READY; pluginNotified(&ready);
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
    state.active = 0;
    state.text = "prefix (&(cn=a)(sn=b)) suffix";
    state.anchor = 7; state.caret = 22;
    state.undoTexts = {"previous independent edit"};
    const auto original{state.text};
    { auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
      check(captured.ok());
      if (captured.ok()) {
        const auto formatted{ldap::formatFilter(captured.snapshot->text())};
        const auto commit{gateway.commit(*captured.snapshot, formatted.text, budget)};
        check(commit.ok() && commit.applied && state.replacements == 1 && state.begins == 1 && state.ends == 1);
        const auto post{state.text};
        SendMessageW(main.handle, SCI_UNDO, 0, 0); check(state.text == original);
        SendMessageW(main.handle, SCI_UNDO, 0, 0); check(state.text == "previous independent edit");
        SendMessageW(main.handle, SCI_REDO, 0, 0); check(state.text == original);
        SendMessageW(main.handle, SCI_REDO, 0, 0); check(state.text == post);
      } }
    for (int view{}; view != 2; ++view) {
      for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
        for (int direction{}; direction != 3; ++direction) {
          state.active = view;
          state.text = direction == 2 ? "( & (cn=a) (sn=b) )" : "pre ( & (cn=a) (sn=b) ) post";
          const auto begin{direction == 2 ? Sci_Position{} : Sci_Position{4}};
          const auto end{direction == 2 ? static_cast<Sci_Position>(state.text.size()) : static_cast<Sci_Position>(state.text.size() - 5)};
          state.anchor = direction == 0 ? begin : direction == 1 ? end : 3;
          state.caret = direction == 0 ? end : direction == 1 ? begin : 3;
          auto captured{gateway.capture(mode, budget)};
          check(captured.ok());
          if (!captured.ok()) { continue; }
          ldap::FormatOptions options{}; options.mode = mode;
          auto formatted{ldap::formatFilter(captured.snapshot->text(), options)};
          const auto outside{state.text.substr(0, static_cast<std::size_t>(begin)) + std::string{formatted.text} + state.text.substr(static_cast<std::size_t>(end))};
          const auto replacements{state.replacements}, begins{state.begins}, ends{state.ends};
          const auto result{gateway.commit(*captured.snapshot, formatted.text, budget)};
          check(result.ok() && result.applied && state.text == outside && state.replacements == replacements + 1
            && state.begins == begins + 1 && state.ends == ends + 1);
          const auto outputEnd{begin + static_cast<Sci_Position>(formatted.text.size())};
          check(state.anchor == (direction == 1 ? outputEnd : begin) && state.caret == (direction == 1 ? begin : outputEnd));
        }
      }
    }
    state.active = 0;
    const auto resetCommit = [&] { state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3; };
    const auto rejectedCommit = [&](auto change, ldap::CaptureErrorCode code) {
        resetCommit(); auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
        change(); const auto prior{state.text}; const auto anchor{state.anchor}, caret{state.caret};
        const auto count{state.replacements}, begins{state.begins};
        auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget)};
        check(!result.ok() && result.error->code == code && state.text == prior && state.anchor == anchor && state.caret == caret
            && state.replacements == count && state.begins == begins);
    };
    rejectedCommit([] { state.text[7] = 'z'; }, ldap::CaptureErrorCode::DocumentChanged);
    rejectedCommit([] { state.anchor = 4; }, ldap::CaptureErrorCode::DocumentChanged);
    rejectedCommit([] { state.active = 1; }, ldap::CaptureErrorCode::DocumentChanged); state.active = 0;
    rejectedCommit([] { state.readonly = true; }, ldap::CaptureErrorCode::UnsupportedState); state.readonly = false;
    rejectedCommit([] { state.undo = false; }, ldap::CaptureErrorCode::UnsupportedState); state.undo = true;
    rejectedCommit([] { state.selections = 2; }, ldap::CaptureErrorCode::UnsupportedState); state.selections = 1;
    rejectedCommit([] { state.selectionMode = SC_SEL_RECTANGLE; }, ldap::CaptureErrorCode::UnsupportedState); state.selectionMode = SC_SEL_STREAM;
    rejectedCommit([] { state.virtualSpace = true; }, ldap::CaptureErrorCode::UnsupportedState); state.virtualSpace = false;
    rejectedCommit([] { state.badCount = true; }, ldap::CaptureErrorCode::Api); state.badCount = false;
    rejectedCommit([] { state.statusFailure = true; }, ldap::CaptureErrorCode::Api); state.statusFailure = false;
    rejectedCommit([] { state.changeOnRead = true; }, ldap::CaptureErrorCode::DocumentChanged); state.changeOnRead = state.changed = false;
    resetCommit();
    { auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
      const auto count{state.replacements}, begins{state.begins};
      auto noop{gateway.commit(*captured.snapshot, captured.snapshot->text(), budget)};
      check(noop.ok() && !noop.applied && state.replacements == count && state.begins == begins);
      { auto constrained{budget->reserve(budget->remaining() - (captured.snapshot->text().size() + 1))};
        auto limitedNoop{gateway.commit(*captured.snapshot, captured.snapshot->text(), budget)};
        check(limitedNoop.ok() && !limitedNoop.applied && state.replacements == count && state.begins == begins); }
      for (const auto output : {std::string{"bad\xc0\xaf"}, std::string{"a\0b", 3}, std::string(ldap::MaxOutputBytes + 1, 'a')}) {
        check(!gateway.commit(*captured.snapshot, output, budget).ok());
      }
      { auto exhausted{budget->reserve(budget->remaining())}; check(!gateway.commit(*captured.snapshot, "(cn=a)", budget).ok()); }
      check(state.replacements == count && state.begins == begins); }
    for (int fault{}; fault != 2; ++fault) {
      resetCommit(); auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
      state.replaceFailure = fault == 0; state.postFailure = fault == 1;
      const auto count{state.replacements}, begins{state.begins}, ends{state.ends};
      const auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget)};
      check(result.error && result.error->code == ldap::CaptureErrorCode::CommitIndeterminate
          && state.replacements == count + 1 && state.begins == begins + 1 && state.ends == ends + 1);
      state.replaceFailure = state.postFailure = false;
    }
    // Positive container insertion itself can fail or change the document.
    for (int fault{}; fault != 2; ++fault) {
        resetCommit(); auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
        const auto originalText{state.text}; const auto edits{state.replacements}, begins{state.begins}, ends{state.ends};
        state.addTokenFailure = fault == 0;
        if (fault == 1) { state.tokenReenter = [] { state.nullDocument = true; }; }
        const auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget, nullptr, nullptr, 101 + fault)};
        check(result.error && result.error->code == ldap::CaptureErrorCode::CommitIndeterminate
            && state.text == originalText && state.replacements == edits && state.begins == begins + 1 && state.ends == ends + 1);
        state.addTokenFailure = false; state.nullDocument = false;
    }
    const auto guardBegins{state.begins}, guardEnds{state.ends};
    { ldap::ScintillaUndoGuard guard{main.handle}; guard.close(); guard.close(); }
    try { ldap::ScintillaUndoGuard guard{main.handle}; throw 1; } catch (int) {}
    [&] { ldap::ScintillaUndoGuard guard{main.handle}; return; }();
    check(state.begins == guardBegins + 3 && state.ends == guardEnds + 3);
    resetCommit();
    { auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
      state.selectionReenter = [] { state.text[7] = 'z'; };
      const auto edits{state.replacements};
      const auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget)};
      check(result.error && result.error->code == ldap::CaptureErrorCode::DocumentChanged && state.replacements == edits
          && state.anchor == 3 && state.caret == 3); }
    for (int stage{}; stage != 2; ++stage) {
      for (int changed{}; changed != 3; ++changed) {
        resetCommit(); auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
        void (*callback)(){changed == 0 ? +[] { state.text[7] = 'z'; }
            : changed == 1 ? +[] { state.nullDocument = true; } : +[] { state.anchor = 1; }};
        if (stage == 0) { state.finalSelectionReenter = callback; } else { state.endReenter = callback; }
        const auto edits{state.replacements}, begins{state.begins}, ends{state.ends};
        const auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget)};
        check(result.error && result.error->code == ldap::CaptureErrorCode::CommitIndeterminate
            && state.replacements == edits + 1 && state.begins == begins + 1 && state.ends == ends + 1);
        state.nullDocument = false;
      }
    }
    for (int allocation{}; allocation != 3; ++allocation) {
      resetCommit(); auto captured{gateway.capture(ldap::OutputMode::Pretty, budget)};
      const auto owned{budget->used()}, edits{state.replacements}, begins{state.begins}, ends{state.ends};
      const auto originalText{state.text}; const auto undo{state.undoTexts}, redo{state.redoTexts};
      failAllocationSize = allocation == 0 ? captured.snapshot->text().size() + 1 : std::string_view{"(&(cn=a)(sn=b))"}.size() + 1;
      failAllocationSkip = allocation == 2 ? 1 : 0;
      const auto result{gateway.commit(*captured.snapshot, "(&(cn=a)(sn=b))", budget)};
      check(result.error && result.error->code == ldap::CaptureErrorCode::Resources && failAllocationSize == 0
          && state.text == originalText && state.anchor == 3 && state.caret == 3 && state.undoTexts == undo && state.redoTexts == redo
          && state.replacements == edits && state.begins == begins && state.ends == ends && budget->used() == owned);
    }
    gateway.reset(); rejected(ldap::CaptureErrorCode::Api);
    std::cout << "Gateway simulated host: " << failures << " failures\n";
    return failures == 0 ? 0 : 3;
}
