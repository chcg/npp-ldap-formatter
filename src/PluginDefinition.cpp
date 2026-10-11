#include "PluginDefinition.hpp"
#include "ScintillaGateway.hpp"
#include "BuildInfo.hpp"
#include <algorithm>
#include <array>

namespace {
std::array<FuncItem, static_cast<std::size_t>(nbFunc)> functions{};
ldap::ScintillaGateway gateway{};
std::shared_ptr<ldap::CoreBudget> budget{};
void capture(ldap::OutputMode mode) noexcept {
    try {
        if (!budget) { budget = std::make_shared<ldap::CoreBudget>(); }
        (void)gateway.capture(mode, budget);
    } catch (...) { /* No exception crosses the host ABI. */ }
}
void pretty() noexcept { capture(ldap::OutputMode::Pretty); }
void compact() noexcept { capture(ldap::OutputMode::Compact); }

void about() {
    // Comando del scaffold; el formateador pertenece a tickets posteriores.
    const auto version{ldap::buildVersion()};
    if (!version.empty()) {
        MessageBoxW(gateway.host()._nppHandle,
            L"LDAP Formatter 0.1.0",
            NPP_PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
    }
}
}

bool setCommand(int index, std::wstring_view name, PFUNCPLUGINCMD callback,
    ShortcutKey* shortcut, bool initiallyChecked) noexcept {
    if (index < 0 || index >= nbFunc || callback == nullptr || name.empty()
        || name.size() >= static_cast<std::size_t>(menuItemSize)
        || name.find(L'\0') != std::wstring_view::npos) {
        return false;
    }
    auto& item{functions[static_cast<std::size_t>(index)]};
    item = FuncItem{};
    std::copy(name.begin(), name.end(), item._itemName);
    item._pFunc = callback;
    item._init2Check = initiallyChecked;
    item._pShKey = shortcut; // No propietario; requiere vida útil de DLL.
    return true;
}

void commandMenuInit() noexcept {
    (void)setCommand(0, L"Pretty", pretty);
    (void)setCommand(1, L"Compact", compact);
    (void)setCommand(2, L"Acerca de LDAP Formatter", about);
}
void pluginInit(NppData data) noexcept { gateway.initialize(data); }
void pluginShutdown() noexcept { gateway.reset(); budget.reset(); }
FuncItem* pluginFunctions() noexcept { return functions.data(); }
