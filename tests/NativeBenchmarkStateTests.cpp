#include "support/NativeBenchmarkState.hpp"
#include <iostream>

int main() {
    ldap::SnapshotMetadata first{};
    first.view = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1)); first.buffer = 2; first.document = 3;
    ldap::benchmark::OwnedDocument owned{first};
    auto current{first};
    Sci_Position length{10}; sptr_t status{SC_STATUS_OK}, modified{1};
    unsigned writes{};
    bool clearFails{}, switchAfterClear{};
    const auto identity = [&]() -> std::optional<ldap::SnapshotMetadata> { return current; };
    const auto send = [&](HWND, UINT message, uptr_t, sptr_t) -> sptr_t {
        if (message == SCI_CLEARALL) {
            ++writes;
            if (clearFails) { status = SC_STATUS_FAILURE; }
            else { length = 0; }
            if (switchAfterClear) { ++current.buffer; }
        }
        if (message == SCI_SETSAVEPOINT) { ++writes; modified = 0; }
        if (message == SCI_GETLENGTH) { return length; }
        if (message == SCI_GETSTATUS) { return status; }
        if (message == SCI_GETMODIFY) { return modified; }
        if (message == SCI_SETTEXT) { ++writes; }
        return 0;
    };
    for (const auto mismatch : {0, 1, 2}) {
        current = first;
        if (mismatch == 0) { current.view = nullptr; }
        if (mismatch == 1) { ++current.buffer; }
        if (mismatch == 2) { ++current.document; }
        try { owned.mutate(identity, send, SCI_SETTEXT); return 1; } catch (const std::runtime_error&) {}
        try { owned.cleanup(identity, send); return 1; } catch (const std::runtime_error&) {}
        if (writes || length != 10 || modified != 1) { return 1; }
    }
    current = first; clearFails = true;
    try { owned.cleanup(identity, send); return 1; } catch (const std::runtime_error&) {}
    if (writes != 1 || modified != 1) { return 1; }
    clearFails = false; status = SC_STATUS_OK; writes = 0; switchAfterClear = true;
    try { owned.cleanup(identity, send); return 1; } catch (const std::runtime_error&) {}
    if (writes != 1 || modified != 1) { return 1; } // Never mark the switched buffer saved.
    current = first; switchAfterClear = false; writes = 0;
    owned.cleanup(identity, send);
    if (length != 0 || modified != 0 || writes != 2) { return 1; }
    if (!ldap::benchmark::failedPublication(true, false)
        || ldap::benchmark::failedPublication(false, false) || ldap::benchmark::failedPublication(true, true)) { return 1; }
    std::cout << "Owned document, cleanup failures and lost publication passed\n";
    return 0;
}
