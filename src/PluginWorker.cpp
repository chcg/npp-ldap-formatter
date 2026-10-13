#include "PluginController.hpp"
#include "PluginMessages.hpp"

namespace ldap {
void PluginController::start(OutputMode mode) noexcept {
    auto captured{gateway_.capture(mode, budget_)};
    if (!captured.ok()) { error_ = captured.error; queueDiagnostic(); return; }
    try {
        std::lock_guard lock{mutex_};
        cancellation_ = std::stop_source{};
        job_ = std::move(captured.snapshot); jobGeneration_ = generation_; running_ = true;
        wake_.notify_one();
    } catch (...) { job_.reset(); running_ = false; error_ = CaptureError{CaptureErrorCode::Resources, 0}; }
}
void PluginController::work(std::stop_token stop) noexcept {
    for (;;) {
        std::unique_lock lock{mutex_};
        if (!wake_.wait(lock, stop, [this] { return job_.has_value(); })) { return; }
        std::optional<ScintillaSnapshot> snapshot{std::move(job_)}; job_.reset();
        const auto generation{jobGeneration_};
        FormatOptions options{}; options.mode = snapshot->mode;
        options.eol = snapshot->metadata.eol == SC_EOL_LF ? Eol::Lf : snapshot->metadata.eol == SC_EOL_CR ? Eol::Cr : Eol::CrLf;
        options.stop = cancellation_.get_token(); options.budget = budget_;
        options.inputReservation = &snapshot->reservation();
        options.checkpoint = checkpoint_; options.checkpointContext = checkpointContext_;
        lock.unlock();
        auto result{formatFilter(snapshot->text(), options)};
        std::unique_ptr<FormatDelivery> delivery{};
        try {
            auto reservation{budget_->reserve(sizeof(FormatDelivery))};
            if (reservation) {
                delivery = std::make_unique<FormatDelivery>(FormatDelivery{std::move(*reservation), generation, std::move(*snapshot), std::move(result)});
            }
        }
        catch (...) { /* The owned snapshot/result are released before quiescence. */ }
        snapshot.reset(); result = FormatResult{};
        lock.lock();
        const bool notify{!failNotification_};
        resourceFailure_ = !delivery; finishedGeneration_ = generation;
        mailbox_ = std::move(delivery); finished_ = true; lock.unlock();
        if (notify) { (void)PostMessageW(window_, completed, 0, 0); }
    }
}
}
