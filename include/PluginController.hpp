#pragma once
#include "ScintillaGateway.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ldap {
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
    [[nodiscard]] std::unique_ptr<FormatDelivery> takeDelivery() noexcept;
    [[nodiscard]] std::optional<CaptureError> error() const noexcept { return error_; }
    [[nodiscard]] const NppData& host() const noexcept { return gateway_.host(); }
    void setCheckpoint(CheckpointObserver observer, void* context) noexcept;
    // Deterministic resource fault seams; set only while idle, on the UI thread.
    void setFailures(bool threadCreation, bool notification) noexcept {
        std::lock_guard lock{mutex_};
        if (!running_) { failThread_ = threadCreation; failNotification_ = notification; }
    }
private:
    struct Intent { OutputMode mode{}; SnapshotMetadata identity{}; };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;
    void work(std::stop_token stop) noexcept;
    void start(OutputMode mode) noexcept;
    ScintillaGateway gateway_{};
    std::shared_ptr<CoreBudget> budget_{};
    std::jthread worker_{};
    std::mutex mutex_{};
    std::condition_variable_any wake_{};
    std::optional<ScintillaSnapshot> job_{};
    std::unique_ptr<FormatDelivery> mailbox_{}, ready_{};
    std::optional<Intent> pending_{};
    std::stop_source cancellation_{};
    std::uint64_t generation_{}, jobGeneration_{}, finishedGeneration_{};
    bool running_{}, finished_{}, closing_{true};
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
