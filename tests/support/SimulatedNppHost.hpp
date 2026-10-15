#pragma once
#include <windows.h>
#include "PluginInterface.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace e2e {
struct Image {
    std::string bytes{};
    Sci_Position anchor{}, caret{};
    bool operator==(const Image&) const = default;
};
struct Action {
    Image before{}, after{};
    int token{};
    bool operator==(const Action&) const = default;
};
struct Document {
    Image image{};
    std::vector<Action> history{};
    std::size_t cursor{};
    int codepage{SC_CP_UTF8}, eol{SC_EOL_CRLF}, selections{1}, mode{SC_SEL_STREAM};
    bool readonly{}, collect{true}, virtualSpace{}, badCount{}, badNul{}, nullDocument{};
    sptr_t status{};
    std::size_t begins{}, ends{}, replacements{}, undoCalls{}, sets{}, reads{};
    bool group{}, recorded{}, containerFirst{}, earlyFailure{}, lateFailure{}, deleteFailure{}, selectionFailure{}, tokenFailure{};
    Image groupBefore{};
    int token{};
    std::function<void()> onBegin{}, onToken{}, onSelect{}, onEnd{}, onRead{};
};
class SimulatedNppHost {
public:
    std::array<Document, 2> documents{};
    HWND host{};
    std::array<HWND, 2> views{};
    int active{};
    bool hostSuccess{true}, registerSuccess{true};
    std::size_t registrations{};
    int flags{};
    PBENOTIFIED notify{};
    DWORD thread{GetCurrentThreadId()};
    bool wrongThread{};
    inline static std::size_t workerMessages{};
    SimulatedNppHost() {
        WNDCLASSW klass{}; klass.lpfnWndProc = procedure; klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"LdapDllE2EHost";
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { throw std::runtime_error{"window class"}; }
        host = create(); views[0] = create(); views[1] = create();
        if (!host || !views[0] || !views[1]) {
            for (auto view : views) { if (view) { DestroyWindow(view); } } if (host) { DestroyWindow(host); }
            throw std::runtime_error{"host windows"};
        }
    }
    ~SimulatedNppHost() { for (auto view : views) { if (view) { DestroyWindow(view); } } if (host) { DestroyWindow(host); } }
    SimulatedNppHost(const SimulatedNppHost&) = delete;
    SimulatedNppHost& operator=(const SimulatedNppHost&) = delete;
    NppData data() const { return {host, views[0], views[1]}; }
    void notification(unsigned int code, HWND from = nullptr, int modification = 0, int token = 0) const {
        SCNotification event{}; event.nmhdr.hwndFrom = from ? from : host; event.nmhdr.code = code;
        event.modificationType = modification; event.token = token;
        if (notify) { notify(&event); }
    }
    static void drain() { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP + 37) { ++workerMessages; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    } }
    static bool pumpUntil(const std::function<bool()>& done, DWORD timeout = 5000) {
        const auto deadline{GetTickCount64() + timeout};
        for (;;) {
            drain(); if (done()) { return true; }
            const auto now{GetTickCount64()}; if (now >= deadline) { return false; }
            if (MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT) == WAIT_FAILED) { return false; }
        }
    }
    void undo(std::size_t view, bool redo = false) { SendMessageW(views.at(view), redo ? SCI_REDO : SCI_UNDO, 0, 0); drain(); }
private:
    HWND create() { return CreateWindowExW(0, L"LdapDllE2EHost", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this); }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            const auto* creation{reinterpret_cast<const CREATESTRUCTW*>(l)};
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self{reinterpret_cast<SimulatedNppHost*>(GetWindowLongPtrW(window, GWLP_USERDATA))};
        if (!self) { return DefWindowProcW(window, message, w, l); }
        return self->message(window, message, w, l);
    }
    LRESULT message(HWND window, UINT message, WPARAM w, LPARAM l) {
        wrongThread = wrongThread || GetCurrentThreadId() != thread;
        if (window == host) {
            if (message == NPPM_GETCURRENTSCINTILLA) { *reinterpret_cast<int*>(l) = active; return hostSuccess ? TRUE : FALSE; }
            if (message == NPPM_GETCURRENTBUFFERID) { return active == 0 ? 41 : 42; }
            if (message == NPPM_ADDSCNMODIFIEDFLAGS) { ++registrations; flags = static_cast<int>(l); return registerSuccess ? TRUE : FALSE; }
        }
        if (window != views[0] && window != views[1]) { return DefWindowProcW(window, message, w, l); }
        auto& d{documents[window == views[0] ? 0 : 1]};
        const auto callback = [](std::function<void()>& slot) { auto fn{std::move(slot)}; slot = {}; if (fn) { fn(); } };
        switch (message) {
        case SCI_SETSTATUS: d.status = static_cast<sptr_t>(w); return 0;
        case SCI_GETSTATUS: return d.status;
        case SCI_GETDOCPOINTER: return d.nullDocument ? 0 : reinterpret_cast<LRESULT>(&d);
        case SCI_GETLENGTH: return static_cast<LRESULT>(d.image.bytes.size());
        case SCI_GETANCHOR: return d.image.anchor;
        case SCI_GETCURRENTPOS: return d.image.caret;
        case SCI_GETSELECTIONSTART: return std::min(d.image.anchor, d.image.caret);
        case SCI_GETSELECTIONEND: return std::max(d.image.anchor, d.image.caret);
        case SCI_GETCODEPAGE: return d.codepage;
        case SCI_GETEOLMODE: return d.eol;
        case SCI_GETREADONLY: return d.readonly;
        case SCI_GETUNDOCOLLECTION: return d.collect;
        case SCI_GETSELECTIONS: return d.selections;
        case SCI_GETSELECTIONMODE: return d.mode;
        case SCI_GETSELECTIONNANCHORVIRTUALSPACE:
        case SCI_GETSELECTIONNCARETVIRTUALSPACE: return d.virtualSpace;
        case SCI_GETUNDOACTIONS: return static_cast<LRESULT>(d.history.size());
        case SCI_GETUNDOCURRENT: return static_cast<LRESULT>(d.cursor);
        case SCI_GETTEXTRANGEFULL: {
            auto& range{*reinterpret_cast<Sci_TextRangeFull*>(l)};
            if (range.chrg.cpMin < 0 || range.chrg.cpMax < range.chrg.cpMin || static_cast<uptr_t>(range.chrg.cpMax) > d.image.bytes.size()) {
                d.status = SC_STATUS_FAILURE; return 0;
            }
            const auto size{static_cast<std::size_t>(range.chrg.cpMax - range.chrg.cpMin)};
            std::memcpy(range.lpstrText, d.image.bytes.data() + range.chrg.cpMin, size);
            range.lpstrText[size] = d.badNul ? 'x' : '\0'; ++d.reads; callback(d.onRead);
            return static_cast<LRESULT>(size) + (d.badCount ? 1 : 0);
        }
        case SCI_GETTEXT:
        case SCI_GETSELTEXT: {
            const auto begin{message == SCI_GETTEXT ? Sci_Position{} : std::min(d.image.anchor, d.image.caret)};
            const auto length{message == SCI_GETTEXT ? d.image.bytes.size() : static_cast<std::size_t>(std::max(d.image.anchor, d.image.caret) - begin)};
            const auto count{message == SCI_GETTEXT ? std::min(length, w ? static_cast<std::size_t>(w - 1) : 0) : length};
            if (l) { auto* output{reinterpret_cast<char*>(l)}; std::memcpy(output, d.image.bytes.data() + begin, count);
                output[count] = d.badNul ? 'x' : '\0'; ++d.reads; callback(d.onRead); }
            return static_cast<LRESULT>(count) + (d.badCount ? 1 : 0);
        }
        case SCI_BEGINUNDOACTION: ++d.begins; d.group = true; d.recorded = false; d.groupBefore = d.image; d.token = 0; callback(d.onBegin); return 0;
        case SCI_ADDUNDOACTION: d.token = static_cast<int>(w); if (d.tokenFailure) { d.status = SC_STATUS_FAILURE; } callback(d.onToken); return 0;
        case SCI_SETSEL: ++d.sets; d.image.anchor = static_cast<Sci_Position>(w); d.image.caret = l;
            callback(d.onSelect); if (d.selectionFailure) { d.status = SC_STATUS_FAILURE; } return 0;
        case SCI_ENDUNDOACTION:
            ++d.ends; callback(d.onEnd);
            if (d.recorded) { d.history.back().after = d.image; }
            d.group = false; return 0;
        case SCI_REPLACESEL: {
            ++d.replacements;
            if (d.earlyFailure) { d.status = SC_STATUS_FAILURE; return 0; }
            const auto begin{std::min(d.image.anchor, d.image.caret)};
            const auto length{std::max(d.image.anchor, d.image.caret) - begin};
            if (begin < 0 || length < 0 || static_cast<uptr_t>(begin + length) > d.image.bytes.size()) { d.status = SC_STATUS_FAILURE; return 0; }
            d.history.resize(d.cursor);
            d.history.push_back({d.group ? d.groupBefore : d.image, {}, d.token}); ++d.cursor; d.recorded = true;
            d.image.bytes.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(length), d.deleteFailure ? "" : reinterpret_cast<const char*>(l));
            d.image.anchor = d.image.caret = begin + (d.deleteFailure ? 0 : static_cast<Sci_Position>(std::strlen(reinterpret_cast<const char*>(l))));
            d.history.back().after = d.image;
            if (d.lateFailure || d.deleteFailure) { d.status = SC_STATUS_FAILURE; } return 0;
        }
        case SCI_UNDO:
        case SCI_REDO: {
            const bool undo{message == SCI_UNDO}; ++d.undoCalls;
            if ((undo && !d.cursor) || (!undo && d.cursor == d.history.size())) { return 0; }
            const auto action{d.history[undo ? d.cursor - 1 : d.cursor]};
            const auto send = [&](int flags, int token) { notification(SCN_MODIFIED, window, flags | (undo ? SC_PERFORMED_UNDO : SC_PERFORMED_REDO), token); };
            if (d.containerFirst && action.token) { send(SC_MOD_CONTAINER, action.token); }
            d.image.bytes = undo ? action.before.bytes : action.after.bytes;
            if (undo) { --d.cursor; } else { ++d.cursor; }
            d.image.anchor = d.image.caret = 0;
            send(SC_MOD_INSERTTEXT | SC_LASTSTEPINUNDOREDO, 0);
            if (!d.containerFirst && action.token) { send(SC_MOD_CONTAINER, action.token); }
            d.image.anchor = d.image.caret = static_cast<Sci_Position>(d.image.bytes.size()); return 0;
        }
        default: return DefWindowProcW(window, message, w, l);
        }
    }
};
}
