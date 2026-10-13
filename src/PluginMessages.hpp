#pragma once
#include <Windows.h>

namespace ldap {
// Internal coordination messages shared by the controller responsibility units.
inline constexpr UINT completed{WM_APP + 37}, restoreSelection{WM_APP + 38}, showDiagnostic{WM_APP + 39};
inline constexpr UINT_PTR fallback{1};
}
