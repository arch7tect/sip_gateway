#include <catch2/catch_test_macros.hpp>

#include "sip_gateway/utils/text.hpp"

#include <string>

TEST_CASE("remove_emojis strips emoji codepoints") {
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string input = "Hello " + emoji + " world";
    const std::string expected = "Hello  world";
    REQUIRE(sip_gateway::utils::remove_emojis(input) == expected);
}

TEST_CASE("remove_emojis leaves plain ASCII untouched") {
    const std::string input = "Plain text only.";
    REQUIRE(sip_gateway::utils::remove_emojis(input) == input);
}

TEST_CASE("normalize_text lowercases and trims whitespace") {
    const std::string input = "  Hello\tWORLD  ";
    const std::string expected = "hello world";
    REQUIRE(sip_gateway::utils::normalize_text(input) == expected);
}
