#include "ScintillaGateway.hpp"
#include <algorithm>
#include <limits>

namespace ldap {
ScintillaUndoGuard::ScintillaUndoGuard(HWND view) noexcept : view_{view} {
    if (!IsWindow(view_) || GetWindowThreadProcessId(view_, nullptr) != GetCurrentThreadId()) { return; }
    // BEGIN/END are void messages. Once BEGIN is sent it must be balanced even
    // when the host reports an error; no return value proves success.
    opened_ = true;
    (void)SendMessageW(view_, SCI_BEGINUNDOACTION, 0, 0);
}
void ScintillaUndoGuard::close() noexcept {
    if (!opened_) { return; }
    opened_ = false;
    (void)SendMessageW(view_, SCI_ENDUNDOACTION, 0, 0);
}
void ScintillaGateway::initialize(NppData data) noexcept { data_ = data; uiThread_ = GetCurrentThreadId(); }
void ScintillaGateway::reset() noexcept { data_ = NppData{}; uiThread_ = 0; }
const NppData& ScintillaGateway::host() const noexcept { return data_; }
sptr_t ScintillaGateway::message(HWND view, UINT code, uptr_t w, sptr_t l) noexcept {
    return static_cast<sptr_t>(SendMessageW(view, code, static_cast<WPARAM>(w), static_cast<LPARAM>(l)));
}
bool ScintillaGateway::validMetadata(const SnapshotMetadata& m) noexcept {
    return m.buffer && m.document && m.length >= 0 && m.begin >= 0 && m.end >= m.begin && m.end <= m.length
        && m.anchor >= 0 && m.caret >= 0 && m.anchor <= m.length && m.caret <= m.length
        && m.begin == std::min(m.anchor, m.caret) && m.end == std::max(m.anchor, m.caret)
        && m.eol >= SC_EOL_CRLF && m.eol <= SC_EOL_LF;
}
bool ScintillaGateway::canRestoreSelection(const SnapshotMetadata& observed, const SnapshotMetadata& original) noexcept {
    return observed.view == original.view && observed.buffer == original.buffer
        && observed.document == original.document && observed.length == original.length
        && observed.anchor == original.begin && observed.caret == original.end;
}
HWND ScintillaGateway::activeView() const noexcept {
    int index{-1};
    if (SendMessageW(data_._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&index)) != TRUE
        || (index != 0 && index != 1)) { return nullptr; }
    return index == 0 ? data_._scintillaMainHandle : data_._scintillaSecondHandle;
}
std::optional<SnapshotMetadata> ScintillaGateway::identity() const noexcept {
    if (!uiThread_ || GetCurrentThreadId() != uiThread_ || !IsWindow(data_._nppHandle)
        || GetWindowThreadProcessId(data_._nppHandle, nullptr) != uiThread_) { return {}; }
    const auto view{activeView()};
    if (!view) { return {}; }
    if (!IsWindow(view) || GetWindowThreadProcessId(view, nullptr) != uiThread_) { return {}; }
    const auto q = [view](UINT message) { return ScintillaGateway::message(view, message); };
    (void)SendMessageW(view, SCI_SETSTATUS, SC_STATUS_OK, 0);
    SnapshotMetadata m{};
    m.view = view;
    m.buffer = static_cast<uptr_t>(SendMessageW(data_._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0));
    m.document = q(SCI_GETDOCPOINTER); m.length = q(SCI_GETLENGTH);
    m.begin = q(SCI_GETSELECTIONSTART); m.end = q(SCI_GETSELECTIONEND);
    m.anchor = q(SCI_GETANCHOR); m.caret = q(SCI_GETCURRENTPOS);
    m.codepage = q(SCI_GETCODEPAGE); m.eol = q(SCI_GETEOLMODE);
    if (!validMetadata(m) || q(SCI_GETSTATUS) != SC_STATUS_OK) { return {}; }
    return m;
}
CaptureResult ScintillaGateway::capture(OutputMode mode, std::shared_ptr<CoreBudget> budget) const noexcept {
    std::optional<SnapshotMetadata> scope{};
    const auto fail = [&](CaptureErrorCode code, std::size_t offset = 0) -> CaptureResult {
        const bool input{code == CaptureErrorCode::InvalidUtf8 || code == CaptureErrorCode::RawNul || code == CaptureErrorCode::Length};
        return {{}, CaptureError{code, offset, {}, scope, input ? ErrorPosition::Input : ErrorPosition::Unavailable}};
    };
    try {
        if (uiThread_ == 0 || GetCurrentThreadId() != uiThread_ || !IsWindow(data_._nppHandle)
            || GetWindowThreadProcessId(data_._nppHandle, nullptr) != uiThread_) { return fail(CaptureErrorCode::Api); }
        const HWND view{activeView()};
        if (!IsWindow(view) || GetWindowThreadProcessId(view, nullptr) != uiThread_) { return fail(CaptureErrorCode::Api); }
        const auto query = [&](UINT message, uptr_t w = 0, sptr_t l = 0) -> sptr_t {
            return ScintillaGateway::message(view, message, w, l);
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
        if (!validMetadata(before)) { return fail(CaptureErrorCode::Api); }
        if (query(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::Api); }
        scope = before;
        if (scope->anchor == scope->caret) { scope->begin = 0; scope->end = scope->length; }
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
        if (activeView() != view || before != after) { return fail(CaptureErrorCode::DocumentChanged); }
        if (const auto invalid{validateUtf8Bytes(snapshot.text())}; invalid) { return fail(CaptureErrorCode::InvalidUtf8, invalid->offset); }
        const auto nul{snapshot.text().find('\0')};
        if (nul != std::string_view::npos) { return fail(CaptureErrorCode::RawNul, nul); }
        if (!selected) { snapshot.metadata.begin = 0; snapshot.metadata.end = before.length; }
        return {std::move(snapshot), {}};
    } catch (...) { return fail(CaptureErrorCode::Resources); }
}
CommitResult ScintillaGateway::commit(const ScintillaSnapshot& snapshot, std::string_view output,
    std::shared_ptr<CoreBudget> budget, bool (*current)(void*) noexcept, void* context, int undoToken) const noexcept {
    bool mutated{};
    const auto fail = [&](CaptureErrorCode code, std::size_t offset = 0) -> CommitResult {
        const bool output{code == CaptureErrorCode::InvalidUtf8 || code == CaptureErrorCode::RawNul || code == CaptureErrorCode::Length};
        return {false, CaptureError{mutated ? CaptureErrorCode::CommitIndeterminate : code, offset, {}, snapshot.metadata,
            output ? ErrorPosition::Output : ErrorPosition::Unavailable}};
    };
    try {
        const auto& m{snapshot.metadata};
        const auto matches = [&]() {
            if (current && !current(context)) { return false; }
            auto identityNow{identity()};
            if (!identityNow) { return false; }
            if (m.anchor == m.caret) { identityNow->begin = 0; identityNow->end = identityNow->length; }
            return *identityNow == m;
        };
        if (!matches()) { return fail(CaptureErrorCode::DocumentChanged); }
        const auto q = [&](UINT message, uptr_t w = 0, sptr_t l = 0) {
            return ScintillaGateway::message(m.view, message, w, l);
        };
        const auto supported = [&] {
            return q(SCI_GETREADONLY) == 0 && q(SCI_GETUNDOCOLLECTION) == 1
                && q(SCI_GETSELECTIONS) == 1 && q(SCI_GETSELECTIONMODE) == SC_SEL_STREAM
                && q(SCI_GETSELECTIONNANCHORVIRTUALSPACE) == 0 && q(SCI_GETSELECTIONNCARETVIRTUALSPACE) == 0
                && q(SCI_GETSTATUS) == SC_STATUS_OK;
        };
        if (!supported()) {
            return fail(CaptureErrorCode::UnsupportedState);
        }
        if (m.codepage != SC_CP_UTF8) { return fail(CaptureErrorCode::Encoding); }
        if (output.size() > MaxOutputBytes) { return fail(CaptureErrorCode::Length, MaxOutputBytes); }
        if (const auto invalid{validateUtf8Bytes(output)}) { return fail(CaptureErrorCode::InvalidUtf8, invalid->offset); }
        if (const auto nul{output.find('\0')}; nul != std::string_view::npos) { return fail(CaptureErrorCode::RawNul, nul); }
        const auto oldSize{snapshot.text().size()};
        if (m.begin < 0 || m.end < m.begin || m.end > m.length
            || static_cast<uptr_t>(m.end - m.begin) != oldSize) { return fail(CaptureErrorCode::Api); }
        const auto maximum{std::numeric_limits<Sci_Position>::max()};
        if (output.size() > static_cast<uptr_t>(maximum - (m.length - (m.end - m.begin)))) { return fail(CaptureErrorCode::Length); }
        const auto postLength{m.length - (m.end - m.begin) + static_cast<Sci_Position>(output.size())};
        const auto postEnd{m.begin + static_cast<Sci_Position>(output.size())};
        if (!budget || snapshot.reservation().budget() != budget.get()) { return fail(CaptureErrorCode::Resources); }
        auto reservation{budget->reserve(oldSize + 1)};
        if (!reservation) { return fail(CaptureErrorCode::Resources); }
        std::vector<char> before(oldSize + 1, '\x7f');
        const auto read = [&](Sci_Position begin, Sci_Position end, std::vector<char>& bytes) {
            Sci_TextRangeFull range{{begin, end}, bytes.data()};
            (void)q(SCI_SETSTATUS, SC_STATUS_OK);
            const auto count{q(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range))};
            return count == end - begin && bytes.back() == '\0' && q(SCI_GETSTATUS) == SC_STATUS_OK;
        };
        if (!read(m.begin, m.end, before)) { return fail(CaptureErrorCode::Api); }
        if (std::string_view{before.data(), oldSize} != snapshot.text() || !matches()) { return fail(CaptureErrorCode::DocumentChanged); }
        if (output == snapshot.text()) { return {}; }
        auto outputReservation{budget->reserve(2 * (output.size() + 1))};
        if (!outputReservation) { return fail(CaptureErrorCode::Resources); }
        std::vector<char> terminated(output.size() + 1, '\0');
        std::copy(output.begin(), output.end(), terminated.begin());
        std::vector<char> after(output.size() + 1, '\x7f');
        (void)q(SCI_SETSTATUS, SC_STATUS_OK);
        ScintillaUndoGuard undo{m.view};
        if (!undo.opened() || q(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::Api); }
        if (!matches()) { return fail(CaptureErrorCode::DocumentChanged); }
        // No allocation or formatting remains beyond this point.
        (void)q(SCI_SETSEL, static_cast<uptr_t>(m.begin), m.end);
        if (q(SCI_GETSTATUS) != SC_STATUS_OK || q(SCI_GETANCHOR) != m.begin || q(SCI_GETCURRENTPOS) != m.end) {
            const auto observed{identity()};
            if (observed && canRestoreSelection(*observed, m)) {
                (void)q(SCI_SETSEL, static_cast<uptr_t>(m.anchor), m.caret);
            }
            return fail(CaptureErrorCode::Api);
        }
        auto selectedIdentity{identity()};
        auto expectedIdentity{m};
        expectedIdentity.anchor = m.begin; expectedIdentity.caret = m.end;
        const bool stillOriginal{selectedIdentity && *selectedIdentity == expectedIdentity && supported()
            && (!current || current(context)) && read(m.begin, m.end, before)
            && std::string_view{before.data(), oldSize} == snapshot.text()};
        selectedIdentity = identity();
        if (!stillOriginal || !selectedIdentity || *selectedIdentity != expectedIdentity || !supported() || (current && !current(context))) {
            // Restore only the original document; another buffer now owns its
            // own selection and must not receive a compensating SETSEL.
            if (selectedIdentity && canRestoreSelection(*selectedIdentity, m)) {
                (void)q(SCI_SETSEL, static_cast<uptr_t>(m.anchor), m.caret);
            }
            return fail(CaptureErrorCode::DocumentChanged);
        }
        // Container action is deliberately non-coalescing and belongs to this
        // same undo group. All preflight and storage preparation precede it.
        if (undoToken > 0) {
            (void)q(SCI_ADDUNDOACTION, static_cast<uptr_t>(undoToken), UNDO_NONE);
            if (q(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::CommitIndeterminate); }
            if (identity() != expectedIdentity || !supported() || (current && !current(context))) {
                return fail(CaptureErrorCode::CommitIndeterminate);
            }
        }
        mutated = true;
        (void)q(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(terminated.data()));
        if (q(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::CommitIndeterminate); }
        const auto postIdentity{identity()};
        if (!postIdentity || postIdentity->view != m.view || postIdentity->buffer != m.buffer
            || postIdentity->document != m.document || postIdentity->length != postLength
            || postIdentity->codepage != m.codepage || postIdentity->eol != m.eol
            || !read(m.begin, postEnd, after) || std::string_view{after.data(), output.size()} != output) {
            return fail(CaptureErrorCode::CommitIndeterminate);
        }
        if (identity() != postIdentity) { return fail(CaptureErrorCode::CommitIndeterminate); }
        const bool reverse{m.anchor != m.caret && m.anchor > m.caret};
        (void)q(SCI_SETSEL, static_cast<uptr_t>(reverse ? postEnd : m.begin), reverse ? m.begin : postEnd);
        if (q(SCI_GETSTATUS) != SC_STATUS_OK || q(SCI_GETANCHOR) != (reverse ? postEnd : m.begin)
            || q(SCI_GETCURRENTPOS) != (reverse ? m.begin : postEnd)) { return fail(CaptureErrorCode::CommitIndeterminate); }
        undo.close();
        if (q(SCI_GETSTATUS) != SC_STATUS_OK) { return fail(CaptureErrorCode::CommitIndeterminate); }
        auto finalIdentity{m};
        finalIdentity.length = postLength; finalIdentity.begin = m.begin; finalIdentity.end = postEnd;
        finalIdentity.anchor = reverse ? postEnd : m.begin; finalIdentity.caret = reverse ? m.begin : postEnd;
        if (identity() != finalIdentity || !read(m.begin, postEnd, after)
            || std::string_view{after.data(), output.size()} != output || identity() != finalIdentity) {
            return fail(CaptureErrorCode::CommitIndeterminate);
        }
        return {true, {}};
    } catch (...) { return fail(CaptureErrorCode::Resources); }
}
}
