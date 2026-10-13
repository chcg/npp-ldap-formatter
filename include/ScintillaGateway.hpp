#pragma once
#include <windows.h>
#include "PluginInterface.h"
#include "LdapParser.hpp"
#include <vector>

namespace ldap {
enum class CaptureErrorCode { Api, DocumentChanged, UnsupportedState, Encoding, InvalidUtf8, RawNul, Length, Resources, Core, CommitIndeterminate };
struct SnapshotMetadata {
    HWND view{};
    uptr_t buffer{};
    sptr_t document{};
    Sci_Position length{}, begin{}, end{}, anchor{}, caret{};
    sptr_t codepage{}, eol{};
    bool operator==(const SnapshotMetadata&) const = default;
};
enum class ErrorPosition { Unavailable, Input, Output };
struct CaptureError {
    CaptureErrorCode code{}; std::size_t offset{}; std::optional<ParseError> core{};
    std::optional<SnapshotMetadata> scope{};
    ErrorPosition position{ErrorPosition::Unavailable};
    bool hostBlocked{};
};
struct ScintillaSnapshot {
    SnapshotMetadata metadata{};
    OutputMode mode{OutputMode::Pretty};
    [[nodiscard]] std::string_view text() const noexcept { return {bytes.data(), bytes.empty() ? 0 : bytes.size() - 1}; }
    [[nodiscard]] const CoreBudget::Reservation& reservation() const noexcept { return storage; }
private:
    friend class ScintillaGateway;
    ScintillaSnapshot(CoreBudget::Reservation reservation, SnapshotMetadata identity,
        OutputMode outputMode, std::vector<char> content) noexcept
        : metadata{identity}, mode{outputMode}, storage{std::move(reservation)}, bytes{std::move(content)} {}
    CoreBudget::Reservation storage{};
    std::vector<char> bytes{};
};
struct CaptureResult {
    std::optional<ScintillaSnapshot> snapshot{};
    std::optional<CaptureError> error{};
    [[nodiscard]] bool ok() const noexcept { return snapshot.has_value(); }
};
struct CommitResult {
    bool applied{};
    std::optional<CaptureError> error{};
    [[nodiscard]] bool ok() const noexcept { return !error; }
};
class ScintillaUndoGuard final {
public:
    explicit ScintillaUndoGuard(HWND view) noexcept;
    ~ScintillaUndoGuard() noexcept { close(); }
    ScintillaUndoGuard(const ScintillaUndoGuard&) = delete;
    ScintillaUndoGuard& operator=(const ScintillaUndoGuard&) = delete;
    void close() noexcept;
    [[nodiscard]] bool opened() const noexcept { return opened_; }
private:
    HWND view_{};
    bool opened_{};
};
// El host entrega handles no propietarios. Las futuras operaciones serán de hilo UI.
class ScintillaGateway final {
public:
    void initialize(NppData data) noexcept;
    void reset() noexcept;
    [[nodiscard]] const NppData& host() const noexcept;
    [[nodiscard]] std::optional<SnapshotMetadata> identity() const noexcept;
    [[nodiscard]] CaptureResult capture(OutputMode mode = OutputMode::Pretty,
        std::shared_ptr<CoreBudget> budget = {}) const noexcept;
    [[nodiscard]] CommitResult commit(const ScintillaSnapshot& snapshot, std::string_view output,
        std::shared_ptr<CoreBudget> budget, bool (*current)(void*) noexcept = nullptr, void* context = nullptr,
        int undoToken = 0) const noexcept;
private:
    NppData data_{};
    DWORD uiThread_{};
};
}
