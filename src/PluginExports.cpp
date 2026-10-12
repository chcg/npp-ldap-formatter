#include "PluginDefinition.hpp"

// Sin trabajo bajo loader lock; el CRT estático gestiona su ciclo de vida.
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
extern "C" __declspec(dllexport) void setInfo(NppData data) {
    pluginInit(data);
    commandMenuInit();
}
extern "C" __declspec(dllexport) const wchar_t* getName() {
    return NPP_PLUGIN_NAME;
}
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    if (count == nullptr) { return nullptr; }
    *count = nbFunc;
    return pluginFunctions();
}
extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    pluginNotified(notification);
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) {
    return TRUE;
}
