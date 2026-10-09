#include "LdapParser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures{};

void valid(std::string_view input) {
    const auto result{ldap::parseSimpleEquality(input)};
    if (!result.ok() || result.compact != input) {
        std::cerr << "Valid filter rejected or bytes changed\n";
        ++failures;
    }
}

void invalid(std::string_view input, ldap::LdapErrorCode code, std::size_t offset) {
    const auto result{ldap::parseSimpleEquality(input)};
    if (result.ok() || !result.compact.empty() || result.error->code != code
        || result.error->offset != offset) {
        std::cerr << "Wrong error code/offset or partial output\n";
        ++failures;
    }
}
} // namespace

int main() {
    using ldap::LdapErrorCode;
    valid("(cn=John Doe)");
    valid("(cn= John Doe  )");
    valid("(cn=)");
    valid("(cn=a=b)");
    valid("(cn;lang-en=Jose)");
    valid("(2.5.4.3=Jose)");
    valid("(cn=\\28\\29\\2A\\5C\\00)");
    valid("(cn=\\c4\\8d)");
    valid("(cn=\\aF\\0a)");
    valid("(cn=Jos\xC3\xA9)");
    valid("(cn=John\tDoe)");

    invalid("", LdapErrorCode::InvalidSyntax, 0);
    invalid("cn=a)", LdapErrorCode::InvalidSyntax, 0);
    invalid("(cn=a", LdapErrorCode::InvalidSyntax, 5);
    invalid("(cn=a) ", LdapErrorCode::InvalidSyntax, 6);
    invalid(" (cn=a)", LdapErrorCode::InvalidSyntax, 0);
    invalid("(=a)", LdapErrorCode::InvalidAttribute, 1);
    invalid("(cn =a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(1cn=a)", LdapErrorCode::InvalidAttribute, 1);
    invalid("(2.05.4=a)", LdapErrorCode::InvalidAttribute, 4);
    invalid("(cn;=a)", LdapErrorCode::InvalidAttribute, 4);
    invalid("(cn=a*b)", LdapErrorCode::UnsupportedFilter, 5);
    invalid("(cn=*)", LdapErrorCode::UnsupportedFilter, 4);
    invalid("(&(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(cn>=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn=a(b)", LdapErrorCode::InvalidSyntax, 5);
    invalid("(cn=a)b)", LdapErrorCode::InvalidSyntax, 6);
    invalid("(cn=\\2)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\GG)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\)", LdapErrorCode::InvalidEscape, 4);
    invalid(std::string{"(cn=a\0b)", 8}, LdapErrorCode::RawNul, 5);
    invalid(std::string{"(cn=\xC0\xAF)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xE2\x82)"}, LdapErrorCode::InvalidUtf8, 6);
    invalid(std::string{"(cn=\xED\xA0\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xF4\x90\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xE2\x28\xA1)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\x80)"}, LdapErrorCode::InvalidUtf8, 4);

    if (failures != 0) { return EXIT_FAILURE; }
    std::cout << "LDAP simple equality: all cases passed\n";
    return EXIT_SUCCESS;
}
