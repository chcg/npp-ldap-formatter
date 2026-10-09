#include <windows.h>
#include <cstddef>
#include <type_traits>
#include "PluginInterface.h"
#include "Sci_Position.h"

static_assert(SC_CP_UTF8 == 65001);
static_assert(sizeof(Sci_Position) == sizeof(void*));
static_assert(sizeof(sptr_t) == sizeof(void*));
static_assert(sizeof(uptr_t) == sizeof(void*));
static_assert(std::is_signed_v<Sci_Position>);
static_assert(SCI_GETTEXTRANGEFULL > 0 && SCI_ADDUNDOACTION > 0);
static_assert(SCI_BEGINUNDOACTION != SCI_ENDUNDOACTION);
static_assert(NPPM_GETCURRENTSCINTILLA != NPPM_GETCURRENTBUFFERID);
// Inicialización explícita; no punteros/propiedad C++ exportados entre DLL.
[[maybe_unused]] const NppData baseline_data{};
