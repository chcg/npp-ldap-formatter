// Reuse the simulated host and deterministic allocation injector.
#define main gatewayHarnessMain
#include "ScintillaGatewayTests.cpp"
#undef main
#include "PluginController.hpp"
#include <limits>
#include <source_location>
namespace ldap {
struct PluginControllerTestAccess {
    static void exhaustTokens(PluginController& controller) { controller.nextToken_ = std::numeric_limits<int>::max(); }
    static void occupyRecords(PluginController& controller) {
        for (auto& record : controller.records_) { record = PluginController::UndoRecord{}; }
    }
    static void clearRecords(PluginController& controller) { for (auto& record : controller.records_) { record.reset(); } }
    static std::size_t recordCount(PluginController& controller) {
        return static_cast<std::size_t>(std::count_if(controller.records_.begin(), controller.records_.end(), [](const auto& record) { return record.has_value(); }));
    }
};
}
namespace {
struct Event {
    HANDLE handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Event() { if (!handle) { std::abort(); } }
    ~Event() { CloseHandle(handle); }
};
struct Barrier { Event entered{}, release{}; };
void checkpoint(ldap::CorePhase phase, std::size_t, void* context) noexcept {
    if (phase != ldap::CorePhase::Entry) { return; }
    auto& barrier{*static_cast<Barrier*>(context)};
    SetEvent(barrier.entered.handle);
    (void)WaitForSingleObject(barrier.release.handle, INFINITE);
}
bool finish(ldap::PluginController& controller) {
    const auto deadline{GetTickCount64() + 5000};
    for (;;) {
        controller.poll();
        if (!controller.busy()) { return true; }
        const auto now{GetTickCount64()};
        if (now >= deadline) { return false; }
        (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    }
}
ldap::PluginController* reentrantController{};
void resubmit() { reentrantController->poll(); reentrantController->submit(ldap::OutputMode::Compact); }
void closeController() { reentrantController->shutdown(); }
void notifyController(const SCNotification* notification) { reentrantController->notified(notification); }
void readyController(ldap::PluginController& controller) {
    SCNotification notification{}; notification.nmhdr.hwndFrom = state.host; notification.nmhdr.code = NPPN_READY;
    controller.notified(&notification);
}
void drainMessages() {
    MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
}
bool restorationInsideCallback{};
void pumpContainer() {
    const auto before{state.selectionSets}; drainMessages();
    restorationInsideCallback = restorationInsideCallback || before != state.selectionSets;
}
bool finishByMessages(ldap::PluginController& controller, bool notification) {
    const auto deadline{GetTickCount64() + 5000};
    bool observed{};
    while (controller.busy() && GetTickCount64() < deadline) {
        const auto remaining{deadline - GetTickCount64()};
        (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(remaining), QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == static_cast<UINT>(notification ? WM_APP + 37 : WM_TIMER)) { observed = true; }
            // Normal delivery must be exercised through its completion message;
            // fallback delivery must be exercised through its timer.
            if (notification && message.message == WM_TIMER) { continue; }
            DispatchMessageW(&message);
        }
    }
    return observed && !controller.busy();
}
}
int main() {
    WNDCLASSW klass{}; klass.lpfnWndProc = procedure; klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"LdapGatewayTest";
    if (!RegisterClassW(&klass)) { return 1; }
    Window host{}, main{}, second{};
    state.host = host.handle; state.gatewayHarnessMain = main.handle; state.second = second.handle;
    if (!host.handle || !main.handle || !second.handle) { return 2; }
    int failures{};
    const auto check = [&](bool passed, std::source_location location = std::source_location::current()) {
        if (!passed) { ++failures; std::cerr << "Controller case failed at line " << location.line() << '\n'; }
    };
    const auto owner{std::make_unique<ldap::PluginController>()}; auto& controller{*owner}; reentrantController = &controller;
    controller.initialize({host.handle, main.handle, second.handle});
    readyController(controller); state.notify = notifyController;
    check(state.registeredFlags == (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO | SC_PERFORMED_REDO
        | SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO));
    const auto registrations{state.registrations}; readyController(controller); check(state.registrations == registrations);
    const auto baseline{controller.ownedBytes()};
    for (const bool first : {false, true}) {
        for (int view{}; view != 2; ++view) {
            for (int direction{}; direction != 3; ++direction) {
                state.undoTexts.clear(); state.redoTexts.clear(); state.undoTokens.clear(); state.redoTokens.clear();
                state.active = view; state.containerFirst = first;
                state.text = "( & (cn=a) (sn=b) )";
                state.anchor = direction == 0 ? 0 : direction == 1 ? static_cast<Sci_Position>(state.text.size()) : 3;
                state.caret = direction == 0 ? static_cast<Sci_Position>(state.text.size()) : direction == 1 ? 0 : 3;
                const auto original{state.text}; const auto anchor{state.anchor}, caret{state.caret};
                controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
                const auto formatted{state.text}; const auto finalAnchor{state.anchor}, finalCaret{state.caret};
                const auto tokens{state.addedTokens};
                SendMessageW(view == 0 ? main.handle : second.handle, SCI_UNDO, 0, 0);
                check(state.text == original && state.anchor == static_cast<Sci_Position>(state.text.size())
                    && state.caret == state.anchor); // native adjustment, before queue
                std::uint64_t callbackReturn{};
                for (const auto& event : controller.probe()) {
                    if (event.phase == ldap::PluginController::ProbePhase::Return) { callbackReturn = std::max(callbackReturn, event.ordinal); }
                }
                drainMessages(); check(state.anchor == anchor && state.caret == caret && !controller.error());
                bool deferred{};
                for (const auto& event : controller.probe()) {
                    if (event.phase == ldap::PluginController::ProbePhase::Restore && event.ordinal > callbackReturn) { deferred = true; }
                }
                check(deferred);
                SendMessageW(view == 0 ? main.handle : second.handle, SCI_REDO, 0, 0);
                drainMessages(); check(state.text == formatted && state.anchor == finalAnchor && state.caret == finalCaret);
                check(state.addedTokens == tokens && !controller.hostGateFailed());
            }
        }
    }
    // A format from an independent edit history keeps its own token, even when
    // other commands increment the work generation in between undo operations.
    state.active = 0; state.containerFirst = false;
    state.pumpContainer = true; state.pump = pumpContainer;
    state.undoTexts.clear(); state.redoTexts.clear(); state.undoTokens.clear(); state.redoTokens.clear();
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
    controller.submit(ldap::OutputMode::Pretty); check(finishByMessages(controller, true));
    const auto prettyText{state.text}; const auto prettyAnchor{state.anchor}, prettyCaret{state.caret};
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
    SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages();
    check(state.text == prettyText && state.anchor == prettyAnchor && state.caret == prettyCaret);
    SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages(); check(state.anchor == 3 && state.caret == 3);
    check(!restorationInsideCallback); state.pumpContainer = false;
    SendMessageW(main.handle, SCI_REDO, 0, 0); drainMessages();
    check(state.anchor == prettyAnchor && state.caret == prettyCaret);
    // Undo cancels a retained worker and prevents its stale delivery from
    // replacing the restored text, with the original selection still restored.
    Barrier undoHeld{}; controller.setCheckpoint(checkpoint, &undoHeld);
    controller.submit(ldap::OutputMode::Compact);
    check(WaitForSingleObject(undoHeld.entered.handle, 5000) == WAIT_OBJECT_0);
    const auto beforeUndoEdits{state.replacements};
    SendMessageW(main.handle, SCI_UNDO, 0, 0);
    SetEvent(undoHeld.release.handle); drainMessages(); check(finish(controller));
    check(state.replacements == beforeUndoEdits && state.anchor == 3 && state.caret == 3);
    controller.setCheckpoint(nullptr, nullptr);
    // Discard queued restoration after activation, close, empty history, edit,
    // or document substitution; another scope keeps its native selection.
    for (int invalidation{}; invalidation != 5; ++invalidation) {
        state.active = 0; state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
        state.undoTexts.clear(); state.redoTexts.clear(); state.undoTokens.clear(); state.redoTokens.clear();
        controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
        SendMessageW(main.handle, SCI_UNDO, 0, 0);
        SCNotification changed{}; changed.nmhdr.hwndFrom = host.handle;
        if (invalidation == 0) { state.active = 1; changed.nmhdr.code = NPPN_BUFFERACTIVATED; controller.notified(&changed); }
        if (invalidation == 1) { changed.nmhdr.code = NPPN_FILEBEFORECLOSE; changed.nmhdr.idFrom = 42; controller.notified(&changed); }
        if (invalidation == 2) { state.undoTexts.clear(); state.redoTexts.clear(); state.undoTokens.clear(); state.redoTokens.clear(); }
        if (invalidation == 3) { changed.nmhdr.hwndFrom = main.handle; changed.nmhdr.code = SCN_MODIFIED; changed.modificationType = SC_MOD_INSERTTEXT; controller.notified(&changed); }
        if (invalidation == 4) { state.changeIdentity = 1; state.copied = true; }
        state.anchor = state.caret = 1; drainMessages(); check(state.anchor == 1 && state.caret == 1);
        state.changeIdentity = 0; state.active = 0;
    }
    // Unknown tokens, foreign origins, and duplicate container events do not
    // produce another restoration or touch the selection.
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
    SCNotification foreign{}; foreign.nmhdr.code = SCN_MODIFIED; foreign.nmhdr.hwndFrom = host.handle;
    foreign.modificationType = SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO | SC_PERFORMED_UNDO;
    foreign.token = std::numeric_limits<int>::max(); controller.notified(&foreign); drainMessages();
    state.anchor = state.caret = 2; foreign.nmhdr.hwndFrom = main.handle;
    controller.notified(&foreign); controller.notified(&foreign); drainMessages(); check(state.anchor == 2 && state.caret == 2);
    controller.notified(reinterpret_cast<const SCNotification*>(static_cast<std::uintptr_t>(1)));
    // Null/foreign/wrong-thread callbacks are safe, and a no-op adds no token.
    controller.notified(nullptr); std::jthread wrongThread{[&] { controller.notified(&foreign); }}; wrongThread.join();
    const auto noopTokens{state.addedTokens}; state.anchor = state.caret = 0;
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true)); check(state.addedTokens == noopTokens);
    state.notify = nullptr; state.active = 0;
    // Regression cases isolate sticky failures by reinitializing the controller.
    const auto resetRegression = [&] {
        controller.setCheckpoint(nullptr, nullptr); controller.shutdown();
        state.undoCountOverride.reset(); state.undoCursorOverride.reset(); state.historyStatusFailure = false;
        state.addTokenFailure = state.replaceFailure = state.postFailure = false; state.nullDocument = false;
        state.undoTexts.clear(); state.redoTexts.clear(); state.undoTokens.clear(); state.redoTokens.clear();
        state.active = 0; state.secondDocument = 123; state.containerFirst = false;
        state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
        controller.initialize({host.handle, main.handle, second.handle}); readyController(controller); state.notify = notifyController;
    };
    for (int fault{}; fault != 3; ++fault) {
        for (const bool restoring : {false, true}) {
            resetRegression();
            if (restoring) {
                controller.submit(ldap::OutputMode::Compact); check(finish(controller));
                SendMessageW(main.handle, SCI_UNDO, 0, 0); state.anchor = state.caret = 1;
            }
            if (fault == 0) { state.undoCountOverride = -1; }
            if (fault == 1) { state.undoCursorOverride = -1; }
            if (fault == 2) { state.historyStatusFailure = true; }
            const auto edits{state.replacements}, sets{state.selectionSets};
            if (restoring) { drainMessages(); } else { controller.submit(ldap::OutputMode::Compact); }
            check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Api
                && state.replacements == edits && state.selectionSets == sets);
            state.undoCountOverride.reset(); state.undoCursorOverride.reset(); state.historyStatusFailure = false;
            controller.submit(ldap::OutputMode::Compact); check(state.replacements == edits && !controller.busy());
        }
    }
    // A completed mailbox is rejected when history observation fails at apply.
    resetRegression(); Barrier historyHeld{}; controller.setCheckpoint(checkpoint, &historyHeld);
    controller.submit(ldap::OutputMode::Compact); check(WaitForSingleObject(historyHeld.entered.handle, 5000) == WAIT_OBJECT_0);
    state.undoCountOverride = -1; const auto staleEdits{state.replacements}; SetEvent(historyHeld.release.handle); check(finish(controller));
    check(controller.error() && state.replacements == staleEdits);
    resetRegression();
    // Reentrancy clears the engaged slot precisely during finalIdentity query.
    state.endReenter = [] {
        state.documentReenterSkip = 2;
        state.documentReenter = [] {
            SCNotification close{}; close.nmhdr.hwndFrom = state.host; close.nmhdr.code = NPPN_FILEBEFORECLOSE; close.nmhdr.idFrom = 42;
            reentrantController->notified(&close);
        };
    };
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(!state.documentReenter && ldap::PluginControllerTestAccess::recordCount(controller) == 0);
    // Separate native operations can complete before posted dispatch drains.
    for (const bool first : {false, true}) {
        resetRegression(); state.containerFirst = first;
        controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
        const auto firstAnchor{state.anchor}, firstCaret{state.caret};
        controller.submit(ldap::OutputMode::Compact); check(finish(controller));
        const auto secondAnchor{state.anchor}, secondCaret{state.caret};
        SendMessageW(main.handle, SCI_UNDO, 0, 0); SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages();
        check(state.anchor == 3 && state.caret == 3 && !controller.hostGateFailed());
        SendMessageW(main.handle, SCI_REDO, 0, 0); SendMessageW(main.handle, SCI_REDO, 0, 0); drainMessages();
        check(state.anchor == secondAnchor && state.caret == secondCaret && !controller.hostGateFailed());
        SendMessageW(main.handle, SCI_UNDO, 0, 0); SendMessageW(main.handle, SCI_REDO, 0, 0); drainMessages();
        check(state.anchor == secondAnchor && state.caret == secondCaret);
        SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages(); check(state.anchor == firstAnchor && state.caret == firstCaret);
    }
    // Repeated callbacks for an already restored known token must not overwrite
    // a selection chosen after that restoration.
    resetRegression(); controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages();
    SCNotification duplicate{}; duplicate.nmhdr.hwndFrom = main.handle; duplicate.nmhdr.code = SCN_MODIFIED;
    duplicate.modificationType = SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO | SC_PERFORMED_UNDO;
    duplicate.token = state.redoTokens.back();
    state.anchor = state.caret = 1; const auto duplicateSets{state.selectionSets};
    controller.notified(&duplicate); controller.notified(&duplicate); drainMessages();
    check(state.anchor == 1 && state.caret == 1 && state.selectionSets == duplicateSets && !controller.hostGateFailed());
    // identity() resets Scintilla status, so preserve SETSEL's status first.
    resetRegression(); controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    SendMessageW(main.handle, SCI_UNDO, 0, 0);
    state.selectionReenter = [] { state.status = SC_STATUS_FAILURE; }; drainMessages();
    check(controller.hostGateFailed() && controller.error() && controller.error()->code == ldap::CaptureErrorCode::Api);
    // Three distinct known tokens in one operation stay incompatible.
    resetRegression();
    for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact, ldap::OutputMode::Pretty}) {
        controller.submit(mode); check(finish(controller));
    }
    const auto knownTokens{state.undoTokens};
    for (const auto token : knownTokens) {
        SCNotification callback{}; callback.nmhdr.hwndFrom = main.handle; callback.nmhdr.code = SCN_MODIFIED;
        callback.modificationType = SC_MOD_CONTAINER | SC_PERFORMED_UNDO; callback.token = token; controller.notified(&callback);
    }
    SCNotification last{}; last.nmhdr.hwndFrom = main.handle; last.nmhdr.code = SCN_MODIFIED;
    last.modificationType = SC_LASTSTEPINUNDOREDO | SC_PERFORMED_UNDO; controller.notified(&last);
    const auto incompatibleSets{state.selectionSets}, incompatibleEdits{state.replacements}; drainMessages();
    check(controller.hostGateFailed() && state.selectionSets == incompatibleSets);
    controller.submit(ldap::OutputMode::Compact); check(state.replacements == incompatibleEdits && !controller.busy());
    // Dropped redo branches reclaim undone metadata, while live history remains.
    resetRegression();
    // Retain one independent action so empty-history cleanup cannot hide leaks.
    state.undoTexts = {"previous independent edit"}; state.undoTokens = {0};
    for (std::size_t branch{}; branch != 1030; ++branch) {
        controller.submit(ldap::OutputMode::Compact); check(finish(controller));
        SendMessageW(main.handle, SCI_UNDO, 0, 0); drainMessages();
        state.redoTexts.clear(); state.redoTokens.clear(); state.anchor = state.caret = 3;
        SCNotification edit{}; edit.nmhdr.hwndFrom = main.handle; edit.nmhdr.code = SCN_MODIFIED; edit.modificationType = SC_MOD_INSERTTEXT;
        controller.notified(&edit);
        check(ldap::PluginControllerTestAccess::recordCount(controller) == 0);
    }
    controller.submit(ldap::OutputMode::Compact); check(finish(controller)); check(!controller.error());
    // A deliberate caret/selection update after native return wins over queue.
    for (const bool selection : {false, true}) {
        resetRegression(); controller.submit(ldap::OutputMode::Compact); check(finish(controller));
        SendMessageW(main.handle, SCI_UNDO, 0, 0); state.anchor = 1; state.caret = selection ? 4 : 1;
        SCNotification update{}; update.nmhdr.hwndFrom = main.handle; update.nmhdr.code = SCN_UPDATEUI; update.updated = SC_UPDATE_SELECTION;
        controller.notified(&update); drainMessages(); check(state.anchor == 1 && state.caret == (selection ? 4 : 1));
    }
    // Inactive views (separate or cloned document) invalidate work and restore.
    for (const bool clone : {false, true}) {
        resetRegression(); state.secondDocument = clone ? 123 : 321;
        Barrier inactiveHeld{}; controller.setCheckpoint(checkpoint, &inactiveHeld);
        controller.submit(ldap::OutputMode::Compact); check(WaitForSingleObject(inactiveHeld.entered.handle, 5000) == WAIT_OBJECT_0);
        SCNotification edit{}; edit.nmhdr.hwndFrom = second.handle; edit.nmhdr.code = SCN_MODIFIED; edit.modificationType = SC_MOD_INSERTTEXT;
        controller.notified(&edit); const auto inactiveEdits{state.replacements}; SetEvent(inactiveHeld.release.handle); check(finish(controller));
        check(state.replacements == inactiveEdits); controller.setCheckpoint(nullptr, nullptr);
        controller.submit(ldap::OutputMode::Compact); check(finish(controller)); SendMessageW(main.handle, SCI_UNDO, 0, 0);
        edit.modificationType = SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO | SC_PERFORMED_UNDO; edit.token = state.redoTokens.back();
        controller.notified(&edit); state.anchor = state.caret = 1; drainMessages(); check(state.anchor == 1 && state.caret == 1);
    }
    // Indeterminate token insertion also closes the gate until reinitialize.
    resetRegression(); state.addTokenFailure = true; controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    state.addTokenFailure = false; const auto blockedEdits{state.replacements};
    check(controller.hostGateFailed()); controller.submit(ldap::OutputMode::Pretty); check(state.replacements == blockedEdits && !controller.busy());
    for (int failure{}; failure != 5; ++failure) {
        resetRegression();
        if (failure == 0) { state.replaceFailure = true; }
        if (failure == 1) { state.postFailure = true; }
        if (failure == 2) { state.finalSelectionReenter = [] { state.anchor = 1; }; }
        if (failure == 3) {
            state.tokenReenter = [] {
                SCNotification activated{}; activated.nmhdr.hwndFrom = state.host; activated.nmhdr.code = NPPN_BUFFERACTIVATED;
                reentrantController->notified(&activated);
            };
        }
        if (failure == 4) {
            SCNotification ambiguous{}; ambiguous.nmhdr.hwndFrom = main.handle; ambiguous.nmhdr.code = SCN_MODIFIED;
            ambiguous.modificationType = SC_MOD_CONTAINER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO; controller.notified(&ambiguous);
        } else { controller.submit(ldap::OutputMode::Compact); check(finish(controller)); }
        state.replaceFailure = state.postFailure = false;
        const auto edits{state.replacements}; check(controller.hostGateFailed());
        controller.submit(ldap::OutputMode::Pretty); check(state.replacements == edits && !controller.busy());
    }
    resetRegression(); state.notify = nullptr;
    for (const bool notification : {true, false}) {
        controller.setFailures(false, !notification);
        state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
        const auto edits{state.replacements};
        controller.submit(ldap::OutputMode::Compact);
        check(finishByMessages(controller, notification));
        check(!controller.error() && state.text == "(&(cn=a)(sn=b))" && state.replacements == edits + 1);
        // Dispatch any duplicate completion without an explicit poll.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        check(state.replacements == edits + 1);
    }
    controller.setFailures(false, false);
    for (int view{}; view != 2; ++view) {
      for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
        for (const int eol : {SC_EOL_CRLF, SC_EOL_LF, SC_EOL_CR}) {
          state.active = view; state.eol = eol;
          state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
          ldap::FormatOptions options{}; options.mode = mode;
          options.eol = eol == SC_EOL_LF ? ldap::Eol::Lf : eol == SC_EOL_CR ? ldap::Eol::Cr : ldap::Eol::CrLf;
          const auto expected{ldap::formatFilter(state.text, options)};
          const auto count{state.replacements};
          controller.submit(mode); check(finish(controller));
          check(!controller.error() && state.text == expected.text && state.replacements == count + 1);
          controller.poll(); check(!controller.takeDelivery() && state.replacements == count + 1);
          check(controller.ownedBytes() == baseline);
        }
      }
    }
    state.active = 0; state.eol = SC_EOL_CRLF; state.anchor = state.caret = 0;
    state.text = "(cn=" + std::string(300 * 1024, 'a') + ")";
    const auto noopCount{state.replacements};
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(!controller.error() && state.replacements == noopCount && controller.ownedBytes() == baseline);
    state.text = "(cn=)extra"; const auto malformed{state.text};
    const auto expectedError{ldap::formatFilter(state.text)};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(controller.error() && controller.error()->core && expectedError.error
        && controller.error()->core->code == expectedError.error->code
        && controller.error()->offset == expectedError.error->offset && state.text == malformed);
    state.text.assign(ldap::MaxInputBytes + 1, 'a');
    controller.submit(ldap::OutputMode::Pretty);
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Length);
    state.text = "(|(cn=" + std::string(ldap::MaxInputBytes - 14, 'a') + ")(sn=b))";
    const auto overflowInput{state.text};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(controller.error() && controller.error()->core && controller.error()->core->code == ldap::LdapErrorCode::OutputTooLarge
        && state.text == overflowInput && controller.ownedBytes() == baseline);
    state.text = "( & (cn=a) (sn=b) )";
    Barrier held{}; controller.setCheckpoint(checkpoint, &held);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(held.entered.handle, 5000) == WAIT_OBJECT_0);
    const auto count{state.replacements};
    controller.submit(ldap::OutputMode::Pretty); controller.submit(ldap::OutputMode::Compact);
    SetEvent(held.release.handle); check(finish(controller));
    check(state.text == "(&(cn=a)(sn=b))" && state.replacements == count + 1 && !controller.error());
    controller.setCheckpoint(nullptr, nullptr);
    // Allocation failure from an obsolete generation is discarded; pending
    // intent still captures and publishes exactly one latest result.
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0;
    Barrier staleAllocation{}; controller.setCheckpoint(checkpoint, &staleAllocation);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(staleAllocation.entered.handle, 5000) == WAIT_OBJECT_0);
    controller.submit(ldap::OutputMode::Compact); failAllocationSize = sizeof(ldap::FormatDelivery);
    SetEvent(staleAllocation.release.handle); check(finish(controller));
    check(!controller.error() && state.text == "(&(cn=a)(sn=b))" && failAllocationSize == 0);
    controller.setCheckpoint(nullptr, nullptr);
    // Pending identity changed after submission must not be captured again.
    for (int change{}; change != 3; ++change) {
      state.active = 0; state.anchor = state.caret = 0; state.text = "( & (cn=a) (sn=b) )";
      Barrier pending{}; controller.setCheckpoint(checkpoint, &pending);
      controller.submit(ldap::OutputMode::Pretty);
      check(WaitForSingleObject(pending.entered.handle, 5000) == WAIT_OBJECT_0);
      controller.submit(ldap::OutputMode::Compact);
      if (change == 0) { state.active = 1; }
      if (change == 1) { state.anchor = 1; }
      if (change == 2) { state.nullDocument = true; }
      const auto copies{state.copies}, edits{state.replacements};
      SetEvent(pending.release.handle); check(finish(controller));
      check(state.copies == copies && state.replacements == edits);
      state.nullDocument = false; controller.setCheckpoint(nullptr, nullptr);
    }
    for (int changed{}; changed != 4; ++changed) {
      state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0; state.active = 0;
      Barrier stale{}; controller.setCheckpoint(checkpoint, &stale);
      controller.submit(ldap::OutputMode::Pretty);
      check(WaitForSingleObject(stale.entered.handle, 5000) == WAIT_OBJECT_0);
      if (changed == 0) { state.text[7] = 'z'; }
      if (changed == 1) { state.anchor = 1; }
      if (changed == 2) { state.active = 1; }
      if (changed == 3) { state.nullDocument = true; }
      const auto prior{state.text}; const auto edits{state.replacements};
      SetEvent(stale.release.handle); check(finish(controller));
      check(state.text == prior && state.replacements == edits && controller.error());
      state.nullDocument = false; controller.setCheckpoint(nullptr, nullptr);
    }
    state.active = 0; state.anchor = state.caret = 3; state.text = "( & (cn=a) (sn=b) )";
    state.reenter = resubmit; const auto reentryEdits{state.replacements};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(state.text == "(&(cn=a)(sn=b))" && state.replacements == reentryEdits + 1 && !controller.error());
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
    state.selectionReenter = resubmit; const auto selectionEdits{state.replacements};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(state.text == "(&(cn=a)(sn=b))" && state.replacements == selectionEdits + 1 && !controller.error());
    // The new scope includes only a valid second filter; the original scope is
    // initially the first filter. Deliberate selection replaces the temporary
    // selection and must survive cancellation of the old request.
    state.text = "( & (cn=a) (sn=b) ) ( & (cn=z) (sn=q) )";
    state.anchor = 0; state.caret = 19;
    state.selectionReenter = [] {
        state.anchor = 20; state.caret = static_cast<Sci_Position>(state.text.size());
        reentrantController->submit(ldap::OutputMode::Compact);
    };
    const auto deliberateEdits{state.replacements};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(!controller.error() && state.text == "( & (cn=a) (sn=b) ) (&(cn=z)(sn=q))" && state.replacements == deliberateEdits + 1
        && state.anchor == 20 && state.caret == static_cast<Sci_Position>(state.text.size()));
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 3;
    state.reenter = closeController; const auto closeEdits{state.replacements};
    controller.submit(ldap::OutputMode::Pretty); check(finish(controller));
    check(state.replacements == closeEdits && !controller.takeDelivery() && controller.ownedBytes() == 0);
    controller.shutdown();
    controller.initialize({host.handle, main.handle, second.handle});
    readyController(controller);
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0;
    Barrier shutdownHeld{}; Event releaseShutdown{};
    controller.setCheckpoint(checkpoint, &shutdownHeld); controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(shutdownHeld.entered.handle, 5000) == WAIT_OBJECT_0);
    std::jthread releaser{[&] { (void)WaitForSingleObject(releaseShutdown.handle, INFINITE); SetEvent(shutdownHeld.release.handle); }};
    const auto shutdownEdits{state.replacements}; SetEvent(releaseShutdown.handle); controller.shutdown();
    check(state.replacements == shutdownEdits && controller.ownedBytes() == 0 && !controller.takeDelivery());
    controller.setFailures(true, false); controller.initialize({host.handle, main.handle, second.handle});
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources);
    controller.setFailures(false, true); controller.initialize({host.handle, main.handle, second.handle});
    readyController(controller);
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0;
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(!controller.error() && state.text == "(&(cn=a)(sn=b))");
    controller.setFailures(false, false); failAllocationSize = sizeof(ldap::FormatDelivery);
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(failAllocationSize == 0 && controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources);
    controller.shutdown(); controller.shutdown(); check(state.begins == state.ends);
    // Registration failure is sticky until a complete reinitialization.
    state.registerFlags = false; controller.initialize({host.handle, main.handle, second.handle});
    const auto rejectedEdits{state.replacements}; readyController(controller);
    state.registerFlags = true; readyController(controller); controller.submit(ldap::OutputMode::Pretty);
    check(controller.error() && state.replacements == rejectedEdits && !controller.busy());
    controller.shutdown(); controller.initialize({host.handle, main.handle, second.handle}); readyController(controller);
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0;
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true)); check(!controller.error());
    ldap::PluginControllerTestAccess::occupyRecords(controller);
    const auto capacityEdits{state.replacements}, capacityTokens{state.addedTokens};
    controller.submit(ldap::OutputMode::Pretty); check(finishByMessages(controller, true));
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources
        && state.replacements == capacityEdits && state.addedTokens == capacityTokens);
    ldap::PluginControllerTestAccess::clearRecords(controller); ldap::PluginControllerTestAccess::exhaustTokens(controller);
    controller.submit(ldap::OutputMode::Pretty); check(finishByMessages(controller, true)); check(!controller.error());
    check(state.pendingToken == std::numeric_limits<int>::max());
    const auto exhaustedEdits{state.replacements}, exhaustedTokens{state.addedTokens};
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources
        && state.replacements == exhaustedEdits && state.addedTokens == exhaustedTokens);
    controller.shutdown(); check(controller.ownedBytes() == 0);
    controller.initialize({host.handle, main.handle, second.handle}); readyController(controller);
    controller.submit(ldap::OutputMode::Compact); check(finishByMessages(controller, true));
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources && state.addedTokens == exhaustedTokens);
    controller.shutdown();
    std::cout << "Controller simulated host: " << failures << " failures\n";
    return failures ? 3 : 0;
}
