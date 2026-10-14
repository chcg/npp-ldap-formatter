#include "support/NativeBenchmarkFixture.hpp"
#include <iostream>

int main() {
    for (const auto size : ldap::benchmark::Sizes) {
        const auto input{ldap::benchmark::fixture(size)};
        if (input.size() != size || ldap::validateUtf8Bytes(input)) { return 1; }
        const auto canonical{ldap::formatFilter(input, {ldap::OutputMode::Compact})};
        if (!canonical.ok() || canonical.text == input) { return 1; }
        for (const auto mode : {ldap::OutputMode::Pretty, ldap::OutputMode::Compact}) {
            const auto output{ldap::formatFilter(input, {mode})};
            if (!output.ok() || output.text.empty() || output.text.size() > ldap::MaxOutputBytes
                || output.text == input || ldap::validateUtf8Bytes(output.text)) { return 1; }
            const auto roundtrip{ldap::formatFilter(output.text, {ldap::OutputMode::Compact})};
            if (!roundtrip.ok() || std::string_view{roundtrip.text} != std::string_view{canonical.text}) { return 1; }
        }
    }
    try { (void)ldap::benchmark::fixture(0); return 1; }
    catch (const std::invalid_argument&) {}
    std::cout << "Native benchmark fixtures: exact sizes, UTF-8 and canonical equivalence passed\n";
    return 0;
}
