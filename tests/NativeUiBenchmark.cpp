#include "ScintillaGateway.hpp"
#include "support/NativeBenchmarkFixture.hpp"
#include "support/NativeBenchmarkState.hpp"
#include "menuCmdID.h"
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <thread>
#include <vector>

#if defined(_DEBUG) || defined(_DLL) || !defined(_MT) || !defined(_M_X64)
#error Native UI benchmark requires Release x64 /MT
#endif

namespace {
constexpr UINT Start{WM_APP + 71}, Done{WM_APP + 72}, Probe{WM_APP + 73};
constexpr UINT_PTR Timer{1};
constexpr UINT_PTR SettleTimer{2};
constexpr UINT PeriodMs{10};
constexpr UINT SettleMs{100};
struct Sample {
    double capture{}, format{}, commit{}, total{}, maxGap{};
    std::size_t output{}, peak{}, ticks{};
};
ldap::ScintillaGateway gateway{};
ldap::benchmark::OwnedDocument owned{};
HWND dispatcher{};
std::jthread worker{};
std::optional<ldap::ScintillaSnapshot> snapshot{};
std::optional<ldap::FormatResult> formatted{};
std::shared_ptr<ldap::CoreBudget> budget{};
std::array<Sample, ldap::benchmark::Warmups + ldap::benchmark::Samples> samples{};
std::size_t size{}, iteration{};
ldap::OutputMode mode{};
std::filesystem::path outputPath{};
LARGE_INTEGER frequency{}, totalStart{}, previousTick{};
std::atomic<bool> formatting{};
std::atomic<bool> postFailed{};
std::size_t baseWs{}, finalWs{}, peakWs{};
bool failed{};
bool finished{};
unsigned stage{}, failureCode{};

auto currentDocument() { return gateway.identity(); }
sptr_t send(HWND view, UINT message, uptr_t w, sptr_t l) {
    return static_cast<sptr_t>(SendMessageW(view, message, static_cast<WPARAM>(w), static_cast<LPARAM>(l)));
}
void requireOwned() {
    if (!owned.matches(currentDocument())) { throw std::runtime_error{"Synthetic document changed"}; }
}

LARGE_INTEGER now() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) { throw std::runtime_error{"Clock failure"}; }
    return value;
}
double elapsed(LARGE_INTEGER start, LARGE_INTEGER end) {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}
void memory(std::size_t* current = nullptr) {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        throw std::runtime_error{"Working Set unavailable"};
    }
    if (current) { *current = counters.WorkingSetSize; }
    peakWs = std::max(peakWs, counters.PeakWorkingSetSize);
}
std::wstring environment(const wchar_t* key) {
    const auto length{GetEnvironmentVariableW(key, nullptr, 0)};
    if (!length || length > 32768) { throw std::runtime_error{"Missing benchmark environment"}; }
    std::wstring value(length, L'\0');
    if (GetEnvironmentVariableW(key, value.data(), length) != length - 1) {
        throw std::runtime_error{"Invalid benchmark environment"};
    }
    value.resize(length - 1);
    return value;
}
void finish() {
    if (finished) { return; }
    finished = true;
    (void)KillTimer(dispatcher, Timer);
    (void)KillTimer(dispatcher, SettleTimer);
    if (worker.joinable()) { worker.request_stop(); worker.join(); }
    snapshot.reset(); formatted.reset(); budget.reset();
    // Never clear whichever tab happens to be active. Cleanup precedes publication.
    try { owned.cleanup(currentDocument, send); }
    catch (...) { failed = true; if (failureCode == 0) { stage = 6; failureCode = 1; } }
    memory(&finalWs);
    // Only numeric/enumerated data crosses this boundary; never document text/hashes.
    std::ofstream stream{outputPath, std::ios::binary | std::ios::trunc};
    if (!stream) { throw std::runtime_error{"Result file unavailable"}; }
    stream << std::setprecision(17) << "{\"schema\":1,\"complete\":" << (failed ? "false" : "true")
        << ",\"input_bytes\":" << size << ",\"mode\":\"" << (mode == ldap::OutputMode::Pretty ? "Pretty" : "Compact")
        << "\",\"host_version_code\":" << SendMessageW(gateway.host()._nppHandle, NPPM_GETNPPVERSION, TRUE, 0)
        << ",\"compiler_version\":" << _MSC_FULL_VER
        << ",\"ui_thread\":" << GetCurrentThreadId()
        << ",\"worker_scintilla_calls\":0,\"timer_period_ms\":" << PeriodMs
        << ",\"setup_settle_ms\":" << SettleMs << ",\"failure_stage\":" << (failed ? stage : 0)
        << ",\"failure_code\":" << failureCode
        << ",\"working_set_base_bytes\":" << baseWs << ",\"working_set_final_bytes\":" << finalWs
        << ",\"working_set_peak_bytes\":" << peakWs << ",\"executions\":" << iteration << ",\"runs\":[";
    for (std::size_t i{}; i < iteration; ++i) {
        if (i) { stream << ','; }
        const auto& s{samples[i]};
        stream << "{\"capture_ms\":" << s.capture << ",\"format_ms\":" << s.format
            << ",\"commit_ms\":" << s.commit << ",\"total_ms\":" << s.total
            << ",\"output_bytes\":" << s.output << ",\"core_peak_bytes\":" << s.peak
            << ",\"ui_ticks\":" << s.ticks << ",\"ui_max_gap_ms\":" << s.maxGap << '}';
    }
    stream << "]}\n";
    stream.flush();
    if (!stream) { throw std::runtime_error{"Result write failure"}; }
}
void startIteration() {
    if (iteration == samples.size()) { finish(); return; }
    stage = 1;
    // All setup is outside the capture/format/commit intervals.
    requireOwned();
    const auto input{ldap::benchmark::fixture(size)};
    owned.mutate(currentDocument, send, SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(input.c_str()));
    if (send(owned.identity.view, SCI_GETLENGTH, 0, 0) != static_cast<sptr_t>(size)
        || send(owned.identity.view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) {
        throw std::runtime_error{"Fixture setup failure"};
    }
    owned.mutate(currentDocument, send, SCI_SETSEL);
    if (send(owned.identity.view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) { throw std::runtime_error{"Setup selection failure"}; }
    owned.mutate(currentDocument, send, SCI_EMPTYUNDOBUFFER);
    if (send(owned.identity.view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK) { throw std::runtime_error{"Setup undo failure"}; }
    // A low-priority timer lets setup paint/idle work finish before capture begins.
    if (!SetTimer(dispatcher, SettleTimer, SettleMs, nullptr)) { throw std::runtime_error{"Setup timer failure"}; }
}
void captureIteration() {
    (void)KillTimer(dispatcher, SettleTimer);
    stage = 2;
    requireOwned();
    budget = std::make_shared<ldap::CoreBudget>();
    memory();
    totalStart = now();
    auto capture{gateway.capture(mode, budget)};
    samples[iteration].capture = elapsed(totalStart, now());
    if (!capture.ok()) { failureCode = 1 + static_cast<unsigned>(capture.error->code); throw std::runtime_error{"Capture failure"}; }
    snapshot = std::move(capture.snapshot);
    if (!owned.matches(snapshot->metadata)) { throw std::runtime_error{"Captured another document"}; }
    previousTick = now();
    if (!SetTimer(dispatcher, Timer, PeriodMs, nullptr)) { throw std::runtime_error{"UI timer failure"}; }
    worker = std::jthread{[](std::stop_token stop) {
        try {
            // This thread only borrows the immutable snapshot; it never uses Scintilla.
            formatting.store(true);
            if (!PostMessageW(dispatcher, Probe, 0, 0)) { postFailed.store(true); }
            const auto begin{now()};
            formatted = ldap::formatFilter(snapshot->text(), {mode, ldap::Eol::CrLf, stop, budget, &snapshot->reservation()});
            samples[iteration].format = elapsed(begin, now());
            formatting.store(false);
            if (!PostMessageW(dispatcher, Done, 0, 0)) { postFailed.store(true); }
        } catch (...) {
            formatting.store(false);
            postFailed.store(true);
            (void)PostMessageW(dispatcher, Done, 0, 0);
        }
    }};
}
void completeIteration() {
    (void)KillTimer(dispatcher, Timer);
    worker.join(); // Done is posted only after all writes; join establishes publication.
    stage = 3;
    auto& sample{samples[iteration]};
    sample.maxGap = std::max(sample.maxGap, elapsed(previousTick, now()));
    if (postFailed.load() || !formatted || !formatted->ok() || formatted->textView().empty()
        || formatted->textView().size() > ldap::MaxOutputBytes || formatted->textView() == snapshot->text()) {
        if (formatted && formatted->error) { failureCode = 1 + static_cast<unsigned>(formatted->error->code); }
        throw std::runtime_error{"Worker failure"};
    }
    stage = 4;
    const auto begin{now()};
    const auto commit{gateway.commit(*snapshot, formatted->textView(), budget)};
    const auto end{now()};
    sample.commit = elapsed(begin, end);
    sample.total = elapsed(totalStart, end);
    if (!commit.ok() || !commit.applied) { if (commit.error) { failureCode = 1 + static_cast<unsigned>(commit.error->code); } throw std::runtime_error{"Commit failure"}; }
    sample.output = formatted->textView().size();
    sample.peak = budget->peak();
    stage = 5;
    // Byte oracle and cleanup are outside every measured phase.
    requireOwned();
    std::vector<char> observed(sample.output + 1, '\x7f');
    const auto view{snapshot->metadata.view};
    if (SendMessageW(view, SCI_GETTEXT, observed.size(), reinterpret_cast<LPARAM>(observed.data()))
            != static_cast<LRESULT>(sample.output) || observed.back() != '\0'
        || SendMessageW(view, SCI_GETSTATUS, 0, 0) != SC_STATUS_OK
        || std::string_view{observed.data(), sample.output} != formatted->textView()) {
        throw std::runtime_error{"Commit oracle failure"};
    }
    memory();
    snapshot.reset(); formatted.reset(); budget.reset();
    ++iteration;
    if (!PostMessageW(dispatcher, Start, 0, 0)) { throw std::runtime_error{"Iteration dispatch failure"}; }
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
    try {
        if (finished && (message == Start || message == Done || message == Probe || message == WM_TIMER)) { return 0; }
        if (message == Start) { startIteration(); return 0; }
        if (message == Done) { completeIteration(); return 0; }
        if (message == WM_TIMER && w == SettleTimer) { captureIteration(); return 0; }
        if (message == Probe || (message == WM_TIMER && w == Timer)) {
            if (ldap::benchmark::failedPublication(postFailed.load(), formatting.load())) {
                stage = 3; failureCode = 100;
                throw std::runtime_error{"Worker publication failed"};
            }
            if (formatting.load()) {
                const auto tick{now()};
                auto& sample{samples[iteration]};
                sample.maxGap = std::max(sample.maxGap, elapsed(previousTick, tick));
                previousTick = tick;
                ++sample.ticks;
            }
            return 0;
        }
    } catch (...) {
        failed = true;
        try { finish(); } catch (...) {} // Missing/partial output is rejected by runner.
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void ready() {
    outputPath = environment(L"NPP_LDAP_BENCHMARK_OUTPUT");
    const auto sizeText{environment(L"NPP_LDAP_BENCHMARK_SIZE")};
    if (sizeText == L"262144") { size = 262144; }
    else if (sizeText == L"1048576") { size = 1048576; }
    else if (sizeText == L"5242880") { size = 5242880; }
    else { throw std::runtime_error{"Invalid size"}; }
    const auto modeText{environment(L"NPP_LDAP_BENCHMARK_MODE")};
    if (modeText != L"Pretty" && modeText != L"Compact") { throw std::runtime_error{"Invalid mode"}; }
    mode = modeText == L"Pretty" ? ldap::OutputMode::Pretty : ldap::OutputMode::Compact;
    if (SendMessageW(gateway.host()._nppHandle, NPPM_GETNPPVERSION, TRUE, 0) != MAKELONG(981, 8)
        || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        throw std::runtime_error{"Incorrect host/clock"};
    }
    // The isolated runner gives us an empty instance; create our own synthetic tab.
    const auto previous{gateway.identity()};
    if (!SendMessageW(gateway.host()._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW)) {
        throw std::runtime_error{"New document failure"};
    }
    const auto identity{gateway.identity()};
    if (!identity || identity->length != 0 || identity->codepage != SC_CP_UTF8
        || (previous && identity->buffer == previous->buffer)) {
        throw std::runtime_error{"Incorrect new document"};
    }
    owned.identity = *identity;
    memory(&baseWs);
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.lpszClassName = L"NppLdapNativeUiBenchmarkDispatcher";
    cls.hInstance = GetModuleHandleW(nullptr);
    if (!RegisterClassW(&cls)) { throw std::runtime_error{"Dispatcher class failure"}; }
    dispatcher = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!dispatcher || !PostMessageW(dispatcher, Start, 0, 0)) { throw std::runtime_error{"Dispatcher failure"}; }
}
void noop() {}
FuncItem item{L"Native UI benchmark (runner only)", noop, 0, false, nullptr};
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
extern "C" __declspec(dllexport) void setInfo(NppData data) { gateway.initialize(data); }
extern "C" __declspec(dllexport) const wchar_t* getName() { return L"NativeUiBenchmark"; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    if (!count) { return nullptr; }
    *count = 1; return &item;
}
extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    if (!notification) { return; }
    if (notification->nmhdr.code == NPPN_READY) {
        try { ready(); } catch (...) { failed = true; try { finish(); } catch (...) {} }
    }
    if (notification->nmhdr.code == NPPN_SHUTDOWN) {
        if (worker.joinable()) { worker.request_stop(); worker.join(); }
        if (dispatcher) { (void)DestroyWindow(dispatcher); dispatcher = nullptr; }
    }
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
