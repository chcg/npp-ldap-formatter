#pragma once
#include "ScintillaGateway.hpp"
#include <stdexcept>

namespace ldap::benchmark {
struct OwnedDocument {
    SnapshotMetadata identity{};
    [[nodiscard]] bool matches(const std::optional<SnapshotMetadata>& current) const noexcept {
        return current && identity.view && identity.buffer && identity.document
            && current->view == identity.view && current->buffer == identity.buffer
            && current->document == identity.document;
    }
    template<class Current, class Send>
    void mutate(Current current, Send send, UINT message, uptr_t w = 0, sptr_t l = 0) const {
        if (!matches(current())) { throw std::runtime_error{"Synthetic document changed"}; }
        (void)send(identity.view, message, w, l);
    }
    template<class Current, class Send>
    void cleanup(Current current, Send send) const {
        mutate(current, send, SCI_CLEARALL);
        // Read status before identity(), which resets Scintilla status itself.
        if (send(identity.view, SCI_GETLENGTH, 0, 0) != 0
            || send(identity.view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK || !matches(current())) {
            throw std::runtime_error{"Synthetic cleanup failed"};
        }
        mutate(current, send, SCI_SETSAVEPOINT);
        if (send(identity.view, SCI_GETLENGTH, 0, 0) != 0 || send(identity.view, SCI_GETMODIFY, 0, 0) != 0
            || send(identity.view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK || !matches(current())) {
            throw std::runtime_error{"Synthetic savepoint failed"};
        }
    }
};
// The UI timer must detect publication failure even when Done was never queued.
inline bool failedPublication(bool postFailed, bool workerActive) noexcept {
    return postFailed && !workerActive;
}
} // namespace ldap::benchmark
