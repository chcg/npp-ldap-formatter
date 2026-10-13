#pragma once
#include "ScintillaGateway.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <array>

namespace ldap {
struct VisibleDiagnostic {
    CaptureError error{};
    std::array<wchar_t, 768> text{};
};
using DiagnosticPresenter = bool (*)(HWND, const VisibleDiagnostic&, void*) noexcept;
[[nodiscard]] VisibleDiagnostic describeError(CaptureError error) noexcept;
struct FormatDelivery {
    CoreBudget::Reservation storage{};
    std::uint64_t generation{};
    ScintillaSnapshot snapshot;
    FormatResult result{};
};
class PluginController final {
public:
    PluginController() = default;
    ~PluginController() noexcept { shutdown(); }
    PluginController(const PluginController&) = delete;
    PluginController& operator=(const PluginController&) = delete;
    void initialize(NppData data) noexcept;
    void submit(OutputMode mode) noexcept;
    void poll() noexcept;
    void shutdown() noexcept;
    void notified(const SCNotification* notification) noexcept;
    enum class ProbePhase { Callback, Return, Restore, Discard, Commit };
    struct ProbeEvent {
        std::uint64_t ordinal{}, session{};
        UINT code{};
        int flags{}, token{};
        ProbePhase phase{};
        HWND view{};
        uptr_t buffer{};
        sptr_t document{};
    };
    [[nodiscard]] const auto& probe() const noexcept { return probe_; }
    [[nodiscard]] std::size_t probeCount() const noexcept { return probeCount_; }
    [[nodiscard]] bool hostGateFailed() const noexcept { return hostGateFailed_; }
    // Compatibility poll: deliveries are consumed internally exactly once on UI.
    [[nodiscard]] std::unique_ptr<FormatDelivery> takeDelivery() noexcept;
    [[nodiscard]] std::optional<CaptureError> error() const noexcept { return error_; }
    [[nodiscard]] bool busy() const noexcept { return running_ || applying_; }
    [[nodiscard]] std::size_t ownedBytes() const noexcept { return budget_ ? budget_->used() : 0; }
    [[nodiscard]] const NppData& host() const noexcept { return gateway_.host(); }
    void setCheckpoint(CheckpointObserver observer, void* context) noexcept;
    DiagnosticPresenter setPresenter(DiagnosticPresenter presenter, void* context = nullptr) noexcept {
        const auto previous{presenter_};
        presenter_ = presenter; presenterContext_ = context;
        return previous;
    }
    // Deterministic resource fault seams; set only while idle, on the UI thread.
    void setFailures(bool threadCreation, bool notification) noexcept {
        std::lock_guard lock{mutex_};
        if (!running_) { failThread_ = threadCreation; failNotification_ = notification; }
    }
private:
    friend struct PluginControllerTestAccess;
    struct Intent { OutputMode mode{}; SnapshotMetadata identity{}; };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;
    void work(std::stop_token stop) noexcept;
    void start(OutputMode mode) noexcept;
    void queueDiagnostic() noexcept;
    void presentDiagnostic() noexcept;
    struct QueuedDiagnostic { CaptureError error{}; std::uint64_t generation{}; };
    std::optional<QueuedDiagnostic> diagnostic_{};
    DiagnosticPresenter presenter_{};
    void* presenterContext_{};
    std::uint64_t diagnosedGeneration_{};
    bool presenting_{}, requested_{};
    struct UndoRecord {
        SnapshotMetadata before{}, after{};
        std::uint64_t session{}, generation{};
        int token{};
        bool restorable{}, undone{};
    };
    struct RestoreBatch {
        HWND view{};
        uptr_t buffer{};
        sptr_t document{};
        std::uint64_t session{}, epoch{};
        int token{}, direction{};
        bool last{}, posted{}, incompatible{};
        sptr_t terminalCursor{-1};
    };
    void restore() noexcept;
    void invalidateWork() noexcept;
    [[nodiscard]] bool observeHistory() noexcept;
    void discardRedo(sptr_t document) noexcept;
    void trace(UINT code, int flags, int token, ProbePhase phase, const SnapshotMetadata* scope = nullptr) noexcept;
    std::array<std::optional<UndoRecord>, 1024> records_{};
    std::array<ProbeEvent, 512> probe_{};
    CoreBudget::Reservation historyStorage_{};
    std::optional<RestoreBatch> restoreBatch_{};
    std::size_t probeCount_{};
    std::uint64_t ordinal_{}, session_{1}, restoreEpoch_{};
    int nextToken_{1};
    int restoredToken_{}, restoredDirection_{};
    bool readyAttempted_{}, flagsReady_{}, undoRedo_{}, restoring_{}, hostGateFailed_{};
    std::size_t notificationDepth_{};
    bool deferredRestore_{};
    ScintillaGateway gateway_{};
    std::shared_ptr<CoreBudget> budget_{};
    std::jthread worker_{};
    std::mutex mutex_{};
    std::condition_variable_any wake_{};
    std::optional<ScintillaSnapshot> job_{};
    std::unique_ptr<FormatDelivery> mailbox_{};
    std::optional<Intent> pending_{};
    std::optional<SnapshotMetadata> applyingIdentity_{};
    std::stop_source cancellation_{};
    std::uint64_t generation_{}, jobGeneration_{}, finishedGeneration_{};
    bool running_{}, finished_{}, applying_{}, closing_{true};
    HWND window_{};
    HINSTANCE module_{};
    DWORD uiThread_{};
    CheckpointObserver checkpoint_{};
    void* checkpointContext_{};
    std::optional<CaptureError> error_{};
    bool resourceFailure_{};
    bool failThread_{}, failNotification_{};
};
}
