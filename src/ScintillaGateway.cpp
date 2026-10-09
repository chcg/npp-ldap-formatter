#include "ScintillaGateway.hpp"

namespace ldap {
void ScintillaGateway::initialize(NppData data) noexcept { data_ = data; }
void ScintillaGateway::reset() noexcept { data_ = NppData{}; }
const NppData& ScintillaGateway::host() const noexcept { return data_; }
}
