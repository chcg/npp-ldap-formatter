#include "ScintillaGateway.hpp"
#include <algorithm>

namespace ldap {
void ScintillaGateway::initialize(NppData data) noexcept { data_ = data; uiThread_ = GetCurrentThreadId(); }
void ScintillaGateway::reset() noexcept { data_ = NppData{}; uiThread_ = 0; }
const NppData& ScintillaGateway::host() const noexcept { return data_; }
std::optional<SnapshotMetadata> ScintillaGateway::identity() const noexcept {
    if (!uiThread_ || GetCurrentThreadId() != uiThread_ || !IsWindow(data_._nppHandle)
        || GetWindowThreadProcessId(data_._nppHandle, nullptr) != uiThread_) { return {}; }
    int index{-1};
    if (SendMessageW(data_._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&index)) != TRUE
        || (index != 0 && index != 1)) { return {}; }
    const auto view{index == 0 ? data_._scintillaMainHandle : data_._scintillaSecondHandle};
    if (!IsWindow(view) || GetWindowThreadProcessId(view, nullptr) != uiThread_) { return {}; }
    const auto q = [view](UINT message) { return static_cast<sptr_t>(SendMessageW(view, message, 0, 0)); };
    (void)SendMessageW(view, SCI_SETSTATUS, SC_STATUS_OK, 0);
    SnapshotMetadata m{};
    m.view = view;
    m.buffer = static_cast<uptr_t>(SendMessageW(data_._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0));
    m.document = q(SCI_GETDOCPOINTER); m.length = q(SCI_GETLENGTH);
    m.begin = q(SCI_GETSELECTIONSTART); m.end = q(SCI_GETSELECTIONEND);
    m.anchor = q(SCI_GETANCHOR); m.caret = q(SCI_GETCURRENTPOS);
    m.codepage = q(SCI_GETCODEPAGE); m.eol = q(SCI_GETEOLMODE);
    if (!m.buffer || !m.document || m.length < 0 || m.begin < 0 || m.end < m.begin || m.end > m.length
        || m.anchor < 0 || m.caret < 0 || m.anchor > m.length || m.caret > m.length
        || m.begin != std::min(m.anchor, m.caret) || m.end != std::max(m.anchor, m.caret)
        || m.eol < SC_EOL_CRLF || m.eol > SC_EOL_LF || q(SCI_GETSTATUS) != SC_STATUS_OK) { return {}; }
    return m;
}
CaptureResult ScintillaGateway::capture(OutputMode mode, std::shared_ptr<CoreBudget> budget) const noexcept {
    const auto fail = [](CaptureErrorCode code, std::size_t offset = 0) -> CaptureResult {
        return {{}, CaptureError{code, offset}};
    };
    try {
        if (uiThread_ == 0 || GetCurrentThreadId() != uiThread_ || !IsWindow(data_._nppHandle)
            || GetWindowThreadProcessId(data_._nppHandle, nullptr) != uiThread_) { return fail(CaptureErrorCode::Api); }
        const auto active = [&]() -> HWND {
            int index{-1};
            if (SendMessageW(data_._nppHandle, NPPM_GETCURRENTSCINTILLA, 0,
                reinterpret_cast<LPARAM>(&index)) != TRUE || (index != 0 && index != 1)) { return nullptr; }
            return index == 0 ? data_._scintillaMainHandle : data_._scintillaSecondHandle;
        };
        const HWND view{active()};
        if (!IsWindow(view) || GetWindowThreadProcessId(view, nullptr) != uiThread_) { return fail(CaptureErrorCode::Api); }
        const auto query = [&](UINT message, uptr_t w = 0, sptr_t l = 0) -> sptr_t {
            return static_cast<sptr_t>(SendMessageW(view, message, static_cast<WPARAM>(w), static_cast<LPARAM>(l)));
        };
        const auto read = [&]() -> SnapshotMetadata {
            SnapshotMetadata m{};
            m.view = view;
            m.buffer = static_cast<uptr_t>(SendMessageW(data_._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0));
            m.document = query(SCI_GETDOCPOINTER);
            m.length = query(SCI_GETLENGTH);
            m.anchor = query(SCI_GETANCHOR);
            m.caret = query(SCI_GETCURRENTPOS);
            m.begin = query(SCI_GETSELECTIONSTART);
            m.end = query(SCI_GETSELECTIONEND);
            m.codepage = query(SCI_GETCODEPAGE);
            m.eol = query(SCI_GETEOLMODE);
            return m;
        };
        (void)query(SCI_SETSTATUS, SC_STATUS_OK);
        const auto before{read()};
        if (before.buffer == 0 || before.document == 0 || before.length < 0 || before.begin < 0
            || before.end < before.begin || before.end > before.length || before.anchor < 0
            || before.caret < 0 || before.anchor > before.length || before.caret > before.length
            || before.begin != std::min(before.anchor, before.caret)
            || before.end != std::max(before.anchor, before.caret)
            || before.eol < SC_EOL_CRLF || before.eol > SC_EOL_LF) { return fail(CaptureErrorCode::Api); }
        if (query(SCI_GETREADONLY) != 0 || query(SCI_GETUNDOCOLLECTION) != 1
            || query(SCI_GETSELECTIONS) != 1 || query(SCI_GETSELECTIONMODE) != SC_SEL_STREAM
            || query(SCI_GETSELECTIONNANCHORVIRTUALSPACE) != 0
            || query(SCI_GETSELECTIONNCARETVIRTUALSPACE) != 0) { return fail(CaptureErrorCode::UnsupportedState); }
        if (before.codepage != SC_CP_UTF8) { return fail(CaptureErrorCode::Encoding); }
        const bool selected{before.begin != before.end};
        const auto amount{selected ? before.end - before.begin : before.length};
        if (static_cast<uptr_t>(amount) > MaxInputBytes) { return fail(CaptureErrorCode::Length, MaxInputBytes); }
        const auto size{static_cast<std::size_t>(amount)};
        if (selected && query(SCI_GETSELTEXT) != static_cast<sptr_t>(size + 1)) { return fail(CaptureErrorCode::Api); }
        if (!budget) { budget = std::make_shared<CoreBudget>(); }
        auto reservation{budget->reserve(size + 1 + sizeof(ScintillaSnapshot))};
        if (!reservation) { return fail(CaptureErrorCode::Resources); }
        ScintillaSnapshot snapshot{std::move(*reservation), before, mode, std::vector<char>(size + 1, '\x7f')};
        const auto count{selected
            ? query(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(snapshot.bytes.data()))
            : query(SCI_GETTEXT, static_cast<uptr_t>(size + 1), reinterpret_cast<sptr_t>(snapshot.bytes.data()))};
        if (count != static_cast<sptr_t>(selected ? size + 1 : size) || snapshot.bytes[size] != '\0') { return fail(CaptureErrorCode::Api); }
        const auto after{read()};
        if (query(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::Api); }
        if (active() != view || before != after) { return fail(CaptureErrorCode::DocumentChanged); }
        if (const auto invalid{validateUtf8Bytes(snapshot.text())}; invalid) { return fail(CaptureErrorCode::InvalidUtf8, invalid->offset); }
        const auto nul{snapshot.text().find('\0')};
        if (nul != std::string_view::npos) { return fail(CaptureErrorCode::RawNul, nul); }
        if (!selected) { snapshot.metadata.begin = 0; snapshot.metadata.end = before.length; }
        return {std::move(snapshot), {}};
    } catch (...) { return fail(CaptureErrorCode::Resources); }
}
}
