#pragma once
#include <string_view>

namespace ldap {
[[nodiscard]] std::string_view buildVersion() noexcept;
}
