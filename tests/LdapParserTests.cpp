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
        return;
    }
    const auto repeated{ldap::parseSimpleEquality(result.compact)};
    if (!repeated.ok() || repeated.compact != result.compact) {
        std::cerr << "Compact output is not idempotent\n";
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
    valid("(cn;lang-en;binary=Jose)");
    valid("(cn;9bin=Jose)");
    valid("(cn;-lang=Jose)");
    valid("(2.5.4.3=Jose)");
    valid("(0.0=Jose)");
    valid("(12345678901234567890.0=Jose)");
    valid("(cn=\\28\\29\\2A\\5C\\00)");
    valid("(cn=\\c4\\8d)");
    valid("(cn=\\aF\\0a)");
    valid("(cn=\\ff)"); // Escaped binary octet remains textual UTF-8.
    valid("(cn=x\\29 y\\28z)");
    valid("(cn=Jos\xC3\xA9)");
    valid("(cn=\xC2\x80)");
    valid("(cn=\xE0\xA0\x80)");
    valid("(cn=\xF0\x9F\x98\x80)");
    valid("(cn=\xF4\x8F\xBF\xBF)");
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
    invalid("(2.5.=a)", LdapErrorCode::InvalidAttribute, 5);
    invalid("(2..5=a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(cn_name=a)", LdapErrorCode::InvalidAttribute, 3);
    invalid("(cn;=a)", LdapErrorCode::InvalidAttribute, 4);
    invalid("(cn=a*b)", LdapErrorCode::UnsupportedFilter, 5);
    invalid("(cn=*)", LdapErrorCode::UnsupportedFilter, 4);
    invalid("(&(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(|(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(!(cn=a))", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(cn>=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn<=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn~=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(cn:=a)", LdapErrorCode::UnsupportedFilter, 3);
    invalid("(:1.2.3:=a)", LdapErrorCode::UnsupportedFilter, 1);
    invalid("(cn=a(b)", LdapErrorCode::InvalidSyntax, 5);
    invalid("(cn=a)b)", LdapErrorCode::InvalidSyntax, 6);
    invalid("(cn=\\2)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\GG)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\0)", LdapErrorCode::InvalidEscape, 4);
    invalid("(cn=\\)", LdapErrorCode::InvalidEscape, 4);
    invalid(std::string{"(cn=a\0b)", 8}, LdapErrorCode::RawNul, 5);
    invalid(std::string{"(cn=\xC0\xAF)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xC1\xBF)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xE0\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xF0\x80\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xE2\x82)"}, LdapErrorCode::InvalidUtf8, 6);
    invalid(std::string{"(cn=\xED\xA0\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xF4\x90\x80\x80)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\xE2\x28\xA1)"}, LdapErrorCode::InvalidUtf8, 5);
    invalid(std::string{"(cn=\x80)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xFE)"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xC2"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xE2\x82"}, LdapErrorCode::InvalidUtf8, 4);
    invalid(std::string{"(cn=\xF0\x9F\x98"}, LdapErrorCode::InvalidUtf8, 4);
    // UTF-8 prevalece sobre el error sintáctico anterior de falta de '('.
    invalid(std::string{"bad=\xC0"}, LdapErrorCode::InvalidUtf8, 4);

    if (failures != 0) { return EXIT_FAILURE; }
    std::cout << "LDAP simple equality: all cases passed\n";
    return EXIT_SUCCESS;
}
