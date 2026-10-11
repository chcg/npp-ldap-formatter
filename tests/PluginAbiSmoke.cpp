#include <windows.h>
#include "PluginInterface.h"
#include <array>
#include <cwchar>
#include <iostream>
#include <memory>

struct ModuleCloser {
    void operator()(HINSTANCE__* module) const noexcept { (void)FreeLibrary(module); }
};

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
    info(NppData{});
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
    SCNotification shutdown{};
    shutdown.nmhdr.code = NPPN_SHUTDOWN;
    notify(&shutdown);
    if (message(0, 0, 0) != TRUE) { return 5; }
    std::cout << "ABI UTF-16, exportaciones, menu y ciclo de vida verificados\n";
    return 0;
}
