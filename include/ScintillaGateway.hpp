#pragma once
#include <windows.h>
#include "PluginInterface.h"

namespace ldap {
// El host entrega handles no propietarios. Las futuras operaciones serán de hilo UI.
class ScintillaGateway final {
public:
    void initialize(NppData data) noexcept;
    void reset() noexcept;
    [[nodiscard]] const NppData& host() const noexcept;
private:
    NppData data_{};
};
}
