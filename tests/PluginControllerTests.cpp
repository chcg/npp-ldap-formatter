// Reuse the simulated Scintilla host and allocation injector from the gateway harness.
#define main gatewayHarnessMain
#include "ScintillaGatewayTests.cpp"
#undef main
#include "PluginController.hpp"

namespace {
struct Event {
    HANDLE handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Event() { if (!handle) { std::abort(); } }
    ~Event() { if (handle) { CloseHandle(handle); } }
};
struct Barrier { Event entered{}, release{}; };
void checkpoint(ldap::CorePhase phase, std::size_t, void* context) noexcept {
    if (phase != ldap::CorePhase::Entry) { return; }
    auto& barrier{*static_cast<Barrier*>(context)};
    SetEvent(barrier.entered.handle);
    (void)WaitForSingleObject(barrier.release.handle, INFINITE);
}
std::unique_ptr<ldap::FormatDelivery> receive(ldap::PluginController& controller, DWORD timeout = 5000) {
    const auto deadline{GetTickCount64() + timeout};
    for (;;) {
        if (auto result{controller.takeDelivery()}) { return result; }
        const auto now{GetTickCount64()};
        if (now >= deadline) { return {}; }
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT) == WAIT_TIMEOUT) { return {}; }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    }
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
    ldap::PluginController controller{};
    controller.initialize({host.handle, main.handle, second.handle});
    state.text = "(&(cn=a)(sn=b))";
    controller.submit(ldap::OutputMode::Compact);
    auto initial{receive(controller)};
    if (!initial) { return 3; }
    const auto* sharedBudget{initial->snapshot.reservation().budget()};
    initial.reset();
    const auto baseline{sharedBudget->used()};
    for (int view{}; view != 2; ++view) {
        state.active = view;
        for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
          for (const int eol : {SC_EOL_CRLF, SC_EOL_LF, SC_EOL_CR}) {
            state.eol = eol;
            controller.submit(mode);
            auto delivery{receive(controller)};
            ldap::FormatOptions options{}; options.mode = mode;
            options.eol = eol == SC_EOL_LF ? ldap::Eol::Lf : eol == SC_EOL_CR ? ldap::Eol::Cr : ldap::Eol::CrLf;
            const auto expected{ldap::formatFilter(state.text, options)};
            check(delivery && delivery->result.ok() && delivery->result.text == expected.text);
          }
        }
    }
    check(sharedBudget->used() == baseline);
    state.eol = SC_EOL_CRLF;
    state.text = "(cn=" + std::string(300 * 1024, 'a') + ")";
    controller.submit(ldap::OutputMode::Compact);
    { auto large{receive(controller)}; check(large && large->result.ok() && large->result.text == state.text); }
    check(sharedBudget->used() == baseline);
    state.text.assign(ldap::MaxInputBytes + 1, 'a');
    controller.submit(ldap::OutputMode::Pretty);
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Length);
    const auto idleStart{GetTickCount64()};
    check(!receive(controller, 40) && GetTickCount64() - idleStart < 1000);
    state.text = "(|(cn=" + std::string(ldap::MaxInputBytes - 14, 'a') + ")(sn=b))";
    controller.submit(ldap::OutputMode::Pretty);
    { auto overflow{receive(controller)};
      auto expected{ldap::formatFilter(state.text)};
      check(overflow && overflow->result.error && expected.error
          && expected.error->code == ldap::LdapErrorCode::OutputTooLarge
          && overflow->result.error->code == expected.error->code
          && overflow->result.error->offset == expected.error->offset); }
    check(sharedBudget->used() == baseline);
    state.text = "(&(cn=a)(sn=b))";
    Barrier barrier{};
    controller.setCheckpoint(checkpoint, &barrier);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(barrier.entered.handle, 5000) == WAIT_OBJECT_0);
    state.copied = false;
    controller.submit(ldap::OutputMode::Pretty);
    controller.submit(ldap::OutputMode::Compact);
    check(!state.copied); // Pending identity reads do not retain another snapshot.
    check(PostMessageW(host.handle, WM_NULL, 0, 0) != FALSE);
    MSG message{};
    check(PeekMessageW(&message, host.handle, WM_NULL, WM_NULL, PM_REMOVE) != FALSE);
    DispatchMessageW(&message); // UI processes a message while the old worker remains held.
    SetEvent(barrier.release.handle);
    auto latest{receive(controller)};
    check(latest && latest->snapshot.mode == ldap::OutputMode::Compact && latest->result.ok());
    controller.setCheckpoint(nullptr, nullptr);
    latest.reset();
    check(sharedBudget->used() == baseline);
    controller.setFailures(false, true); // Setter immediately after poll/delivery.
    controller.setFailures(false, false);
    // Completed-but-unpolled old work must not start its old pending intent.
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    Barrier boundary{};
    controller.setCheckpoint(checkpoint, &boundary);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(boundary.entered.handle, 5000) == WAIT_OBJECT_0);
    controller.submit(ldap::OutputMode::Pretty);
    SetEvent(boundary.release.handle);
    const auto boundaryDeadline{GetTickCount64() + 5000};
    bool queued{};
    while (!queued && GetTickCount64() < boundaryDeadline) {
        queued = PeekMessageW(&message, nullptr, WM_APP + 37, WM_APP + 37, PM_REMOVE) != FALSE;
        if (!queued) { (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT); }
    }
    check(queued);
    const auto previousCopies{state.copies};
    controller.submit(ldap::OutputMode::Compact);
    { auto boundaryResult{receive(controller)};
      check(boundaryResult && boundaryResult->snapshot.mode == ldap::OutputMode::Compact
          && state.copies == previousCopies + 1); }
    controller.setCheckpoint(nullptr, nullptr);
    // An invalid pending observation reports Api without another snapshot.
    Barrier malformed{};
    controller.setCheckpoint(checkpoint, &malformed);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(malformed.entered.handle, 5000) == WAIT_OBJECT_0);
    state.nullBuffer = true;
    controller.submit(ldap::OutputMode::Compact);
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Api);
    state.nullBuffer = false;
    SetEvent(malformed.release.handle);
    check(!receive(controller, 40));
    controller.setCheckpoint(nullptr, nullptr);
    // Failure to allocate a stale delivery must not become the new generation's error.
    Barrier stale{};
    controller.setCheckpoint(checkpoint, &stale);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(stale.entered.handle, 5000) == WAIT_OBJECT_0);
    controller.submit(ldap::OutputMode::Compact);
    failAllocationSize = sizeof(ldap::FormatDelivery);
    SetEvent(stale.release.handle);
    { auto successful{receive(controller)}; check(successful && successful->result.ok() && !controller.error()); }
    controller.setCheckpoint(nullptr, nullptr);
    state.text = "(cn=)extra";
    controller.submit(ldap::OutputMode::Compact);
    auto invalid{receive(controller)};
    const auto expectedError{ldap::formatFilter(state.text)};
    check(invalid && invalid->result.error && expectedError.error
        && invalid->result.error->code == expectedError.error->code
        && invalid->result.error->offset == expectedError.error->offset);
    invalid.reset();
    for (int changed{}; changed != 3; ++changed) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        Barrier held{};
        state.text = "(cn=value)"; state.anchor = state.caret = 0; state.active = 0;
        controller.setCheckpoint(checkpoint, &held);
        controller.submit(ldap::OutputMode::Pretty);
        check(WaitForSingleObject(held.entered.handle, 5000) == WAIT_OBJECT_0);
        controller.submit(ldap::OutputMode::Compact);
        if (changed == 0) { state.active = 1; }
        if (changed == 1) { state.anchor = 1; }
        if (changed == 2) { state.nullDocument = true; }
        state.copied = false;
        SetEvent(held.release.handle);
        // Completion is delivered through the message queue; never wait by sleeping.
        bool delivered{};
        for (int attempt{}; attempt != 100 && !delivered; ++attempt) {
            check(MsgWaitForMultipleObjects(0, nullptr, FALSE, 5000, QS_ALLINPUT) != WAIT_TIMEOUT);
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_APP + 37) { delivered = true; }
                DispatchMessageW(&message);
            }
        }
        check(delivered && !state.copied && !controller.takeDelivery());
        state.nullDocument = false;
        controller.setCheckpoint(nullptr, nullptr);
    }
    state.active = 0; state.anchor = state.caret = 0;
    // Shutdown with work outstanding cancels and joins; the release event is signaled
    // by a helper waiting on an explicit event, without timing assumptions.
    Barrier closing{}; Event releaseShutdown{};
    controller.setCheckpoint(checkpoint, &closing);
    controller.submit(ldap::OutputMode::Pretty);
    check(WaitForSingleObject(closing.entered.handle, 5000) == WAIT_OBJECT_0);
    std::jthread releaser{[&] {
        (void)WaitForSingleObject(releaseShutdown.handle, INFINITE);
        SetEvent(closing.release.handle);
    }};
    SetEvent(releaseShutdown.handle);
    controller.shutdown(); controller.shutdown();
    check(!controller.takeDelivery() && state.mutations == 0);
    controller.setFailures(true, false);
    controller.initialize({host.handle, main.handle, second.handle});
    check(controller.error() && controller.error()->code == ldap::CaptureErrorCode::Resources);
    controller.shutdown(); controller.shutdown();
    controller.setFailures(false, true);
    controller.initialize({host.handle, main.handle, second.handle});
    controller.setCheckpoint(nullptr, nullptr);
    check(!controller.error());
    state.text = "(cn=value)";
    controller.submit(ldap::OutputMode::Compact);
    auto fallbackDelivery{receive(controller)};
    check(fallbackDelivery && fallbackDelivery->result.ok());
    fallbackDelivery.reset();
    controller.setFailures(false, false);
    failAllocationSize = sizeof(ldap::FormatDelivery);
    controller.submit(ldap::OutputMode::Compact);
    for (int attempt{}; attempt != 100 && !controller.error(); ++attempt) {
        check(MsgWaitForMultipleObjects(0, nullptr, FALSE, 5000, QS_ALLINPUT) != WAIT_TIMEOUT);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
    }
    check(failAllocationSize == 0 && controller.error() && !controller.takeDelivery());
    controller.shutdown(); controller.shutdown();
    std::cout << "Controller simulated host: " << failures << " failures\n";
    return failures ? 3 : 0;
}
