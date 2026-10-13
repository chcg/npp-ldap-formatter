#include <windows.h>
#include "PluginInterface.h"
#include <array>
#include <cwchar>
#include <iostream>
#include <memory>

struct ModuleCloser {
    void operator()(HINSTANCE__* module) const noexcept { (void)FreeLibrary(module); }
};
namespace {
std::size_t registrations{};
int modifiedFlags{};
LRESULT CALLBACK hostProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == NPPM_ADDSCNMODIFIEDFLAGS) {
        ++registrations; modifiedFlags = static_cast<int>(l); return TRUE;
    }
    return DefWindowProcW(window, message, w, l);
}
struct HostWindow {
    HWND handle{};
    ~HostWindow() { if (handle) { DestroyWindow(handle); } }
};
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) { return 1; }
    const std::unique_ptr<HINSTANCE__, ModuleCloser> module{LoadLibraryW(argv[1])};
    if (!module) { return 2; }
    constexpr std::array names{"isUnicode", "setInfo", "getName", "getFuncsArray",
        "beNotified", "messageProc"};
    for (const auto* name : names) {
        if (GetProcAddress(module.get(), name) == nullptr) {
            std::cerr << "Export ausente: " << name << '\n';
            return 3;
        }
    }
    const auto unicode{reinterpret_cast<BOOL (__cdecl*)()>(GetProcAddress(module.get(), "isUnicode"))};
    const auto info{reinterpret_cast<PFUNCSETINFO>(GetProcAddress(module.get(), "setInfo"))};
    const auto name{reinterpret_cast<PFUNCGETNAME>(GetProcAddress(module.get(), "getName"))};
    const auto funcs{reinterpret_cast<PFUNCGETFUNCSARRAY>(GetProcAddress(module.get(), "getFuncsArray"))};
    const auto notify{reinterpret_cast<PBENOTIFIED>(GetProcAddress(module.get(), "beNotified"))};
    const auto message{reinterpret_cast<PMESSAGEPROC>(GetProcAddress(module.get(), "messageProc"))};
    WNDCLASSW klass{}; klass.lpfnWndProc = hostProcedure; klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"LdapAbiHost";
    if (!RegisterClassW(&klass)) { return 7; }
    const HostWindow host{CreateWindowExW(0, klass.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, klass.hInstance, nullptr)};
    if (!host.handle) { return 8; }
    info(NppData{host.handle, host.handle, host.handle});
    SCNotification ready{}; ready.nmhdr.code = NPPN_READY; ready.nmhdr.hwndFrom = host.handle;
    notify(&ready); notify(&ready);
    constexpr int expectedFlags{SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO | SC_PERFORMED_REDO
        | SC_MOD_CONTAINER | SC_LASTSTEPINUNDOREDO};
    if (registrations != 1 || modifiedFlags != expectedFlags) { return 9; }
    int count{};
    auto* items{funcs(&count)};
    if (unicode() != TRUE || std::wcscmp(name(), L"LDAP Formatter") != 0
        || count != 3 || items == nullptr || items[0]._pFunc == nullptr
        || items[0]._itemName[0] == L'\0' || funcs(nullptr) != nullptr) {
        return 4;
    }
    notify(nullptr);
    if (std::wcscmp(items[0]._itemName, L"Pretty") != 0
        || std::wcscmp(items[1]._itemName, L"Compact") != 0
        || items[1]._pFunc == nullptr) { return 6; }
    items[0]._pFunc();
    items[1]._pFunc();
    // Shutdown invalidates queued diagnostics before the harness pumps UI.
    SCNotification shutdown{};
    shutdown.nmhdr.code = NPPN_SHUTDOWN;
    shutdown.nmhdr.hwndFrom = host.handle;
    notify(&shutdown);
    MSG pending{};
    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&pending); }
    notify(&ready); if (registrations != 1) { return 10; }
    if (message(0, 0, 0) != TRUE) { return 5; }
    std::cout << "ABI UTF-16, exportaciones, menu y ciclo de vida verificados\n";
    return 0;
}
