// Reuse the simulated host and deterministic allocation injector.
#define main gatewayHarnessMain
#include "ScintillaGatewayTests.cpp"
#undef main
#include "PluginController.hpp"
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
    const auto check = [&](bool passed) { if (!passed) { ++failures; std::cerr << "Controller case failed\n"; } };
    ldap::PluginController controller{}; reentrantController = &controller;
    controller.initialize({host.handle, main.handle, second.handle});
    const auto baseline{controller.ownedBytes()};
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
    state.text = "( & (cn=a) (sn=b) )"; state.anchor = state.caret = 0;
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(!controller.error() && state.text == "(&(cn=a)(sn=b))");
    controller.setFailures(false, false); failAllocationSize = sizeof(ldap::FormatDelivery);
    controller.submit(ldap::OutputMode::Compact); check(finish(controller));
    check(failAllocationSize == 0 && controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources);
    controller.shutdown(); controller.shutdown(); check(state.begins == state.ends);
    std::cout << "Controller simulated host: " << failures << " failures\n";
    return failures ? 3 : 0;
}
