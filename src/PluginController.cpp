#include "PluginController.hpp"
#include <algorithm>
#include <limits>
#include "PluginMessages.hpp"

namespace ldap {
namespace {
bool readableNotification(const SCNotification* notification) noexcept {
    auto address{reinterpret_cast<std::uintptr_t>(notification)};
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - sizeof(SCNotification)) { return false; }
    const auto end{address + sizeof(SCNotification)};
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))
            || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            || !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) { return false; }
        const auto base{reinterpret_cast<std::uintptr_t>(region.BaseAddress)};
        if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base || base + region.RegionSize <= address) { return false; }
        address = std::min(end, base + region.RegionSize);
    }
    return true;
}
}
LRESULT CALLBACK PluginController::procedure(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create{reinterpret_cast<const CREATESTRUCTW*>(l)};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self{reinterpret_cast<PluginController*>(GetWindowLongPtrW(window, GWLP_USERDATA))};
    if (self && message == showDiagnostic) { self->presentDiagnostic(); return 0; }
    if (self && message == restoreSelection) { self->restore(); return 0; }
    if (self && (message == completed || message == WM_TIMER)) {
        self->poll();
        if (message == WM_TIMER) { self->presentDiagnostic(); }
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void PluginController::initialize(NppData data) noexcept {
    shutdown();
    error_.reset();
    try {
        uiThread_ = GetCurrentThreadId(); gateway_.initialize(data);
        budget_ = std::make_shared<CoreBudget>();
        // Reserve the fixed queue and peak formatter/presenter scratch up front
        // so diagnostics need no allocation even when the shared quota is full.
        auto storage{budget_->reserve(sizeof(records_) + sizeof(probe_) + sizeof(RestoreBatch)
            + sizeof(QueuedDiagnostic) + 2 * sizeof(VisibleDiagnostic) + 192 * sizeof(wchar_t))};
        if (!storage) { throw std::bad_alloc{}; }
        historyStorage_ = std::move(*storage);
        readyAttempted_ = flagsReady_ = undoRedo_ = restoring_ = hostGateFailed_ = false;
        probeCount_ = 0; ordinal_ = 0;
        WNDCLASSW klass{}; klass.lpfnWndProc = procedure;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PluginController::procedure), &module_)) { throw std::bad_alloc{}; }
        klass.hInstance = module_; klass.lpszClassName = L"LdapFormatterWorker";
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { throw std::bad_alloc{}; }
        window_ = CreateWindowExW(0, klass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, klass.hInstance, this);
        if (!window_ || !SetTimer(window_, fallback, 20, nullptr)) { throw std::bad_alloc{}; }
        closing_ = false;
        if (failThread_) { throw std::bad_alloc{}; }
        worker_ = std::jthread{[this](std::stop_token stop) { work(stop); }};
    } catch (...) { shutdown(); error_ = CaptureError{CaptureErrorCode::Resources, 0}; }
}
void PluginController::setCheckpoint(CheckpointObserver observer, void* context) noexcept {
    if (!running_) { checkpoint_ = observer; checkpointContext_ = context; }
}
void PluginController::submit(OutputMode mode) noexcept {
    if (closing_ || GetCurrentThreadId() != uiThread_) { return; }
    ++generation_; requested_ = true; diagnostic_.reset();
    if (!flagsReady_ || hostGateFailed_ || undoRedo_ || restoring_) {
        if (undoRedo_ || restoring_ || (!hostGateFailed_ && flagsReady_) || !error_) {
            error_ = CaptureError{CaptureErrorCode::UnsupportedState, 0};
        }
        queueDiagnostic(); return;
    }
    if (!observeHistory()) { requested_ = true; queueDiagnostic(); return; }
    pending_.reset(); error_.reset(); poll();
    if (running_ || applying_ || presenting_) {
        cancellation_.request_stop();
        const auto identity{gateway_.identity()};
        if (!identity) { error_ = CaptureError{CaptureErrorCode::Api, 0}; }
        pending_ = identity ? std::optional<Intent>{Intent{mode, *identity}} : std::nullopt;
        if (pending_ && applyingIdentity_) {
            auto temporary{*applyingIdentity_};
            if (temporary.anchor == temporary.caret) { temporary.begin = 0; temporary.end = temporary.length; }
            temporary.anchor = temporary.begin; temporary.caret = temporary.end;
            if (*identity == temporary) { pending_->identity = *applyingIdentity_; }
        }
    } else { start(mode); }
    queueDiagnostic();
}
void PluginController::poll() noexcept {
    if (closing_ || applying_ || presenting_ || undoRedo_ || restoring_ || notificationDepth_ || GetCurrentThreadId() != uiThread_) { return; }
    std::unique_ptr<FormatDelivery> delivery{};
    {
        std::lock_guard lock{mutex_};
        if (!finished_ && (!pending_ || running_)) { return; }
        finished_ = false; running_ = false;
        if (resourceFailure_ && finishedGeneration_ == generation_) { error_ = CaptureError{CaptureErrorCode::Resources, 0}; }
        resourceFailure_ = false;
        if (mailbox_ && mailbox_->generation == generation_) { delivery = std::move(mailbox_); }
        mailbox_.reset();
    }
    if (delivery) {
        const bool observed{flagsReady_ && !hostGateFailed_ && observeHistory()};
        if (closing_ || delivery->generation != generation_) { return; }
        if (!observed) {
            delivery.reset(); pending_.reset(); requested_ = true; queueDiagnostic(); return;
        }
        processDelivery(*delivery);
        delivery.reset(); applyingIdentity_.reset(); applying_ = false;
    }
    queueDiagnostic();
    if (closing_ || !flagsReady_ || hostGateFailed_) { pending_.reset(); return; }
    if (pending_) {
        const auto intent{*pending_}; pending_.reset();
        const auto identity{gateway_.identity()};
        if (!identity) { error_ = CaptureError{CaptureErrorCode::Api, 0}; }
        else if (identity == intent.identity) { start(intent.mode); }
        else { error_ = CaptureError{CaptureErrorCode::DocumentChanged, 0}; }
    }
    queueDiagnostic();
}
void PluginController::processDelivery(FormatDelivery& delivery) noexcept {
    applying_ = true;
    applyingIdentity_ = delivery.snapshot.metadata;
    applyingIdentity_->begin = std::min(applyingIdentity_->anchor, applyingIdentity_->caret);
    applyingIdentity_->end = std::max(applyingIdentity_->anchor, applyingIdentity_->caret);
    if (!delivery.result.ok()) {
        error_ = coreError(delivery);
    } else {
        struct Validity { PluginController* self; std::uint64_t generation; } validity{this, delivery.generation};
        const auto current = [](void* context) noexcept {
            const auto& v{*static_cast<Validity*>(context)};
            return !v.self->closing_ && v.self->flagsReady_ && !v.self->hostGateFailed_ && v.self->generation_ == v.generation;
        };
        std::optional<UndoRecord>* slot{};
        int token{};
        const bool changes{delivery.result.text != delivery.snapshot.text()};
        if (changes && nextToken_ > 0) {
            for (auto& record : records_) { if (!record) { slot = &record; break; } }
        }
        CommitResult committed{};
        if (changes && (!slot || nextToken_ <= 0)) {
            committed.error = CaptureError{CaptureErrorCode::Resources, 0};
        } else {
            if (changes) {
                token = nextToken_;
                nextToken_ = nextToken_ == std::numeric_limits<int>::max() ? 0 : nextToken_ + 1;
                *slot = UndoRecord{delivery.snapshot.metadata, {}, session_, delivery.generation, token, false};
                trace(0, 0, token, ProbePhase::Commit, &delivery.snapshot.metadata);
            }
            committed = gateway_.commit(delivery.snapshot, delivery.result.text, budget_, current, &validity, token);
            if (committed.error && committed.error->code == CaptureErrorCode::CommitIndeterminate) { hostGateFailed_ = true; }
            if (slot && *slot) {
                const auto finalIdentity{gateway_.identity()};
                if (!*slot || (*slot)->token != token) { slot = nullptr; }
                if (slot && committed.applied && finalIdentity && !closing_
                    && finalIdentity->buffer == (*slot)->before.buffer && finalIdentity->document == (*slot)->before.document) {
                    (*slot)->after = *finalIdentity; (*slot)->restorable = true;
                    discardRedo(finalIdentity->document);
                } else if (slot && (!committed.error || committed.error->code != CaptureErrorCode::CommitIndeterminate)) {
                    slot->reset();
                }
            }
        }
        if (delivery.generation == generation_) { error_ = committed.error; }
    }
}
std::unique_ptr<FormatDelivery> PluginController::takeDelivery() noexcept { poll(); return {}; }
void PluginController::shutdown() noexcept {
    if (closing_ && !window_ && !worker_.joinable() && !budget_ && !module_) { return; }
    closing_ = true; ++generation_; pending_.reset(); diagnostic_.reset(); requested_ = false; cancellation_.request_stop();
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
    worker_ = std::jthread{};
    job_.reset(); mailbox_.reset(); running_ = finished_ = false;
    restoreBatch_.reset(); undoRedo_ = false; ++restoreEpoch_; ++session_;
    deferredRestore_ = false;
    for (auto& record : records_) { record.reset(); }
    historyStorage_ = CoreBudget::Reservation{};
    if (window_) { KillTimer(window_, fallback); DestroyWindow(window_); window_ = nullptr; }
    if (module_) { (void)UnregisterClassW(L"LdapFormatterWorker", module_); module_ = nullptr; }
    gateway_.reset(); budget_.reset(); uiThread_ = 0;
}

void PluginController::trace(UINT code, int flags, int token, ProbePhase phase, const SnapshotMetadata* scope) noexcept {
    if (probeCount_ == probe_.size()) { std::rotate(probe_.begin(), probe_.begin() + 1, probe_.end()); }
    const auto index{probeCount_ < probe_.size() ? probeCount_ : probe_.size() - 1};
    probe_[index] = ProbeEvent{++ordinal_, session_, code, flags, token, phase,
        scope ? scope->view : nullptr, scope ? scope->buffer : 0, scope ? scope->document : 0};
    if (probeCount_ < probe_.size()) { ++probeCount_; }
}
void PluginController::invalidateWork() noexcept {
    ++generation_; ++restoreEpoch_; pending_.reset(); diagnostic_.reset(); requested_ = false; cancellation_.request_stop();
    restoreBatch_.reset(); undoRedo_ = false;
    std::lock_guard lock{mutex_}; mailbox_.reset();
}
bool PluginController::observeHistory() noexcept {
    if (closing_ || !flagsReady_) { return false; }
    if (applying_ || undoRedo_) { return true; }
    const auto scope{gateway_.identity()};
    if (!scope) { error_ = CaptureError{CaptureErrorCode::Api, 0}; return false; }
    (void)SendMessageW(scope->view, SCI_SETSTATUS, SC_STATUS_OK, 0);
    const auto count{SendMessageW(scope->view, SCI_GETUNDOACTIONS, 0, 0)};
    const auto cursor{SendMessageW(scope->view, SCI_GETUNDOCURRENT, 0, 0)};
    if (count < 0 || cursor < 0 || cursor > count
        || SendMessageW(scope->view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) {
        flagsReady_ = false; error_ = CaptureError{CaptureErrorCode::Api, 0}; invalidateWork(); return false;
    }
    bool invalidated{};
    for (auto& record : records_) {
        if (record && record->before.buffer == scope->buffer
            && (record->before.document != scope->document || count == 0)) {
            record.reset(); invalidated = true;
        }
    }
    if (invalidated) { ++session_; ++restoreEpoch_; restoreBatch_.reset(); }
    return true;
}
void PluginController::discardRedo(sptr_t document) noexcept {
    for (auto& record : records_) {
        if (record && record->before.document == document && record->undone) { record.reset(); }
    }
}
bool PluginController::processLifecycle(const SCNotification& notification, UINT code, bool hostEvent) noexcept {
    const auto& host{gateway_.host()};
    if (code == NPPN_READY) {
        if (!hostEvent || readyAttempted_) { return true; }
        readyAttempted_ = true;
        constexpr int flags{SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO | SC_PERFORMED_REDO
            | SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO};
        flagsReady_ = SendMessageW(host._nppHandle, NPPM_ADDSCNMODIFIEDFLAGS, 0, flags) == TRUE;
        if (!flagsReady_) { error_ = CaptureError{CaptureErrorCode::Api, 0}; }
        return true;
    }
    if (code == NPPN_SHUTDOWN && hostEvent) { shutdown(); return true; }
    if (code == NPPN_BUFFERACTIVATED && hostEvent) {
        if (applying_) { hostGateFailed_ = true; }
        invalidateWork(); (void)observeHistory(); return true;
    }
    if (code == NPPN_FILEBEFORECLOSE && hostEvent) {
        const auto buffer{notification.nmhdr.idFrom};
        for (auto& record : records_) { if (record && record->before.buffer == buffer) { record.reset(); } }
        invalidateWork(); return true;
    }
    return false;
}
void PluginController::notified(const SCNotification* notification) noexcept {
    if (closing_ || GetCurrentThreadId() != uiThread_ || !readableNotification(notification)) { return; }
    struct CallbackScope {
        PluginController& owner;
        explicit CallbackScope(PluginController& value) noexcept : owner{value} { ++owner.notificationDepth_; }
        ~CallbackScope() noexcept {
            --owner.notificationDepth_;
            if (owner.notificationDepth_ == 0 && owner.deferredRestore_ && !owner.closing_) {
                owner.deferredRestore_ = false;
                if (!PostMessageW(owner.window_, restoreSelection, 0, 0)) {
                    owner.restoreBatch_.reset(); owner.undoRedo_ = false;
                    owner.error_ = CaptureError{CaptureErrorCode::Api, 0};
                }
            }
        }
    } callbackScope{*this};
    // The ABI supplies a borrowed readable SCNotification; never retain it or text.
    const auto code{notification->nmhdr.code};
    const auto origin{static_cast<HWND>(notification->nmhdr.hwndFrom)};
    const auto& host{gateway_.host()};
    const bool hostEvent{origin == host._nppHandle};
    const bool viewEvent{origin == host._scintillaMainHandle || origin == host._scintillaSecondHandle};
    if (processLifecycle(*notification, code, hostEvent)) { return; }
    if (!viewEvent || !flagsReady_) { return; }
    processViewNotification(*notification, code, origin);
}
void PluginController::processViewNotification(const SCNotification& notification, UINT code, HWND origin) noexcept {
    auto scope{gateway_.identity()};
    if (code == SCN_UPDATEUI && (notification.updated & SC_UPDATE_SELECTION) && restoreBatch_ && !restoring_) {
        if (!scope || scope->view != origin) { return; }
        // Without native-order evidence, a post-terminal selection update may
        // be deliberate. Discard instead of overwriting the user's selection.
        ++restoreEpoch_; restoreBatch_.reset(); undoRedo_ = false;
        return;
    }
    if (code != SCN_MODIFIED) { return; }
    const bool active{scope && scope->view == origin};
    if (!active) {
        if (!IsWindow(origin) || GetWindowThreadProcessId(origin, nullptr) != uiThread_) { return; }
        (void)SendMessageW(origin, SCI_SETSTATUS, SC_STATUS_OK, 0);
        SnapshotMetadata emitting{}; emitting.view = origin;
        emitting.document = SendMessageW(origin, SCI_GETDOCPOINTER, 0, 0);
        emitting.length = SendMessageW(origin, SCI_GETLENGTH, 0, 0);
        if (!emitting.document || emitting.length < 0 || SendMessageW(origin, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) { return; }
        const auto actions{SendMessageW(origin, SCI_GETUNDOACTIONS, 0, 0)};
        const auto undoCursor{SendMessageW(origin, SCI_GETUNDOCURRENT, 0, 0)};
        if (actions < 0 || undoCursor < 0 || undoCursor > actions
            || SendMessageW(origin, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) {
            flagsReady_ = false; error_ = CaptureError{CaptureErrorCode::Api, 0}; invalidateWork(); return;
        }
        if (actions == 0) {
            for (auto& record : records_) {
                if (record && record->before.document == emitting.document) { record.reset(); }
            }
        }
        for (const auto& record : records_) {
            if (record && record->before.document == emitting.document) { emitting.buffer = record->before.buffer; break; }
        }
        scope = emitting;
    }
    const int flags{notification.modificationType};
    const int direction{flags & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)};
    trace(code, flags, notification.token, ProbePhase::Callback, &*scope);
    if (!direction) {
        if ((!applying_ || !active) && (flags & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
            if (applying_ && applyingIdentity_ && applyingIdentity_->document == scope->document) { hostGateFailed_ = true; }
            invalidateWork(); discardRedo(scope->document); (void)observeHistory(); restoredToken_ = restoredDirection_ = 0;
        }
        trace(code, flags, notification.token, ProbePhase::Return, &*scope); return;
    }
    if (direction == (SC_PERFORMED_UNDO | SC_PERFORMED_REDO) || applying_) {
        hostGateFailed_ = true; invalidateWork(); return;
    }
    if (restoredDirection_ && restoredDirection_ != direction) { restoredToken_ = 0; }
    bool knownContainer{};
    for (auto& record : records_) {
        if (record && record->token == notification.token && (flags & SC_MOD_CONTAINER)
            && record->before.document == scope->document) {
            record->undone = direction == SC_PERFORMED_UNDO; knownContainer = record->restorable;
        }
    }
    if (!active) { invalidateWork(); return; }
    const auto cursor{SendMessageW(origin, SCI_GETUNDOCURRENT, 0, 0)};
    if (cursor < 0 || SendMessageW(origin, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) {
        flagsReady_ = false; error_ = CaptureError{CaptureErrorCode::Api, 0}; invalidateWork(); return;
    }
    const bool terminalBatch{restoreBatch_ && restoreBatch_->last && restoreBatch_->token && !restoreBatch_->incompatible};
    const bool nextOperation{terminalBatch && (restoreBatch_->direction != direction
        || restoreBatch_->terminalCursor != cursor
        || (knownContainer && notification.token != restoreBatch_->token))};
    if (nextOperation) { restoreBatch_.reset(); undoRedo_ = false; }
    if (!undoRedo_) { invalidateWork(); }
    undoRedo_ = true;
    if (!restoreBatch_ || restoreBatch_->view != scope->view || restoreBatch_->buffer != scope->buffer
        || restoreBatch_->document != scope->document || restoreBatch_->direction != direction) {
        restoreBatch_ = RestoreBatch{scope->view, scope->buffer, scope->document, session_, restoreEpoch_, 0, direction, false, false};
    }
    auto& batch{*restoreBatch_};
    if (flags & SC_MOD_CONTAINER) {
        for (const auto& record : records_) {
            if (record && record->restorable && record->token == notification.token
                && record->before.buffer == scope->buffer && record->before.document == scope->document) {
                // A multi-token native batch is incompatible with one format per step.
                if (batch.token && batch.token != record->token) { hostGateFailed_ = true; batch.incompatible = true; break; }
                if (!batch.incompatible) { batch.token = record->token; batch.session = record->session; } break;
            }
        }
    }
    if (flags & SC_LASTSTEPINUNDOREDO) { batch.last = true; batch.terminalCursor = cursor; }
    if (batch.last && !batch.posted) {
        batch.posted = true;
        if (!PostMessageW(window_, restoreSelection, 0, 0)) {
            restoreBatch_.reset(); undoRedo_ = false; error_ = CaptureError{CaptureErrorCode::Api, 0};
        }
    }
    trace(code, flags, notification.token, ProbePhase::Return, &*scope);
}
void PluginController::restore() noexcept {
    if (presenting_) { return; }
    if (closing_ || GetCurrentThreadId() != uiThread_ || !restoreBatch_) { return; }
    if (notificationDepth_ != 0) { deferredRestore_ = true; return; }
    const auto batch{*restoreBatch_}; restoreBatch_.reset(); undoRedo_ = false;
    const auto scope{gateway_.identity()};
    if (!scope || batch.epoch != restoreEpoch_ || batch.view != scope->view
        || batch.buffer != scope->buffer || batch.document != scope->document || !batch.last || batch.incompatible || hostGateFailed_
        || (batch.token == restoredToken_ && batch.direction == restoredDirection_)) { return; }
    if (!observeHistory()) { return; }
    const UndoRecord* record{};
    for (const auto& item : records_) {
        if (item && item->restorable && item->token == batch.token && item->session == batch.session) { record = &*item; break; }
    }
    if (!record) { trace(0, 0, batch.token, ProbePhase::Discard, &*scope); return; }
    const auto wanted{batch.direction == SC_PERFORMED_UNDO ? record->before : record->after};
    if (scope->length != wanted.length || wanted.anchor < 0 || wanted.caret < 0
        || wanted.anchor > scope->length || wanted.caret > scope->length || scope->codepage != SC_CP_UTF8
        || SendMessageW(scope->view, SCI_GETSELECTIONS, 0, 0) != 1
        || SendMessageW(scope->view, SCI_GETSELECTIONMODE, 0, 0) != SC_SEL_STREAM) {
        trace(0, 0, batch.token, ProbePhase::Discard, &*scope); return;
    }
    restoring_ = true;
    (void)SendMessageW(scope->view, SCI_SETSTATUS, SC_STATUS_OK, 0);
    const auto revalidated{gateway_.identity()};
    if (revalidated == scope && batch.epoch == restoreEpoch_) {
        (void)SendMessageW(scope->view, SCI_SETSEL, static_cast<WPARAM>(wanted.anchor), static_cast<LPARAM>(wanted.caret));
        const auto selectionStatus{SendMessageW(scope->view, SCI_GETSTATUS, 0, 0)};
        const auto after{gateway_.identity()};
        if (selectionStatus != SC_STATUS_OK || !after || after->view != scope->view || after->buffer != scope->buffer || after->document != scope->document
            || after->anchor != wanted.anchor || after->caret != wanted.caret
            || SendMessageW(scope->view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) {
            hostGateFailed_ = true; error_ = CaptureError{CaptureErrorCode::Api, 0};
        } else {
            restoredToken_ = batch.token; restoredDirection_ = batch.direction;
            trace(0, 0, batch.token, ProbePhase::Restore, &*after);
        }
    }
    restoring_ = false;
}
}
