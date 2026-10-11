#include "PluginController.hpp"

namespace ldap {
namespace { constexpr UINT completed{WM_APP + 37}; constexpr UINT_PTR fallback{1}; }
LRESULT CALLBACK PluginController::procedure(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create{reinterpret_cast<const CREATESTRUCTW*>(l)};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self{reinterpret_cast<PluginController*>(GetWindowLongPtrW(window, GWLP_USERDATA))};
    if (self && (message == completed || message == WM_TIMER)) { self->poll(); return 0; }
    return DefWindowProcW(window, message, w, l);
}
void PluginController::initialize(NppData data) noexcept {
    shutdown();
    error_.reset();
    try {
        uiThread_ = GetCurrentThreadId(); gateway_.initialize(data);
        budget_ = std::make_shared<CoreBudget>();
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
void PluginController::start(OutputMode mode) noexcept {
    auto captured{gateway_.capture(mode, budget_)};
    if (!captured.ok()) { error_ = captured.error; return; }
    try {
        std::lock_guard lock{mutex_};
        cancellation_ = std::stop_source{};
        job_ = std::move(captured.snapshot); jobGeneration_ = generation_; running_ = true;
        wake_.notify_one();
    } catch (...) { job_.reset(); running_ = false; error_ = CaptureError{CaptureErrorCode::Resources, 0}; }
}
void PluginController::submit(OutputMode mode) noexcept {
    if (closing_ || GetCurrentThreadId() != uiThread_) { return; }
    ++generation_; pending_.reset(); ready_.reset(); error_.reset(); poll();
    if (running_) {
        cancellation_.request_stop();
        const auto identity{gateway_.identity()};
        if (!identity) { error_ = CaptureError{CaptureErrorCode::Api, 0}; }
        pending_ = identity ? std::optional<Intent>{Intent{mode, *identity}} : std::nullopt;
    } else { start(mode); }
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
void PluginController::poll() noexcept {
    if (closing_ || GetCurrentThreadId() != uiThread_) { return; }
    {
        std::lock_guard lock{mutex_};
        if (!finished_) { return; }
        finished_ = false; running_ = false;
        if (resourceFailure_ && finishedGeneration_ == generation_) { error_ = CaptureError{CaptureErrorCode::Resources, 0}; }
        resourceFailure_ = false;
        if (mailbox_ && mailbox_->generation == generation_) { ready_ = std::move(mailbox_); }
        mailbox_.reset();
    }
    if (pending_) {
        const auto intent{*pending_}; pending_.reset();
        const auto identity{gateway_.identity()};
        if (!identity) { error_ = CaptureError{CaptureErrorCode::Api, 0}; }
        else if (identity == intent.identity) { start(intent.mode); }
    }
}
std::unique_ptr<FormatDelivery> PluginController::takeDelivery() noexcept { poll(); return std::move(ready_); }
void PluginController::shutdown() noexcept {
    if (closing_ && !window_ && !worker_.joinable() && !budget_ && !module_) { return; }
    closing_ = true; ++generation_; pending_.reset(); cancellation_.request_stop();
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
    worker_ = std::jthread{};
    job_.reset(); mailbox_.reset(); ready_.reset(); running_ = finished_ = false;
    if (window_) { KillTimer(window_, fallback); DestroyWindow(window_); window_ = nullptr; }
    if (module_) { (void)UnregisterClassW(L"LdapFormatterWorker", module_); module_ = nullptr; }
    gateway_.reset(); budget_.reset(); uiThread_ = 0;
}
}
