#pragma once
#include <windows.h>
#include <string_view>
#include "PluginInterface.h"

inline constexpr wchar_t NPP_PLUGIN_NAME[]{L"LDAP Formatter"};
inline constexpr int nbFunc{3};

void commandMenuInit() noexcept;
[[nodiscard]] bool setCommand(int index, std::wstring_view name,
    PFUNCPLUGINCMD callback, ShortcutKey* shortcut = nullptr,
    bool initiallyChecked = false) noexcept;
void pluginInit(NppData data) noexcept;
void pluginShutdown() noexcept;
[[nodiscard]] FuncItem* pluginFunctions() noexcept;
