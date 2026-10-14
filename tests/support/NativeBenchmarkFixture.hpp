#pragma once
#include "LdapParser.hpp"
#include <stdexcept>
#include <string>

namespace ldap::benchmark {
inline constexpr std::size_t Sizes[]{262144, 1048576, 5242880};
inline constexpr std::size_t Warmups{5}, Samples{30};
// Structural padding makes both outputs smaller than the input at the 5 MiB boundary.
inline std::string fixture(std::size_t size) {
    if (size != Sizes[0] && size != Sizes[1] && size != Sizes[2]) {
        throw std::invalid_argument{"Unsupported benchmark size"};
    }
    constexpr std::string_view block{
        "                    (|                    (cn=José\\2a\\28\\29\\5c)"
        "                    (sn=*)                    (cn=ab*cd*ef)"
        "                    (cn:dn:2.5.13.2:=á)                    (!(uid=x))"
        "                    (&                    (uid>=a)                    (uid<=z)"
        "                    (cn~=name)))"};
    std::string result{"(&"};
    result.reserve(size);
    while (result.size() + block.size() + 8 <= size) { result.append(block); }
    result += " (cn=";
    result.append(size - result.size() - 2, 'x');
    result += "))";
    return result;
}
} // namespace ldap::benchmark
