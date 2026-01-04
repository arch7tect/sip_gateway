#include <catch2/catch_test_macros.hpp>

#include "sip_gateway/utils/http.hpp"

#include <string>

TEST_CASE("url_encode escapes reserved characters") {
    const std::string input = "hello world!";
    const std::string expected = "hello%20world%21";
    REQUIRE(sip_gateway::utils::url_encode(input) == expected);
}

TEST_CASE("parse_url splits scheme host port and path") {
    std::string scheme;
    std::string host;
    std::string base_path;
    int port = 0;
    sip_gateway::utils::parse_url("https://example.com:8443/path/file",
                                  scheme, host, port, base_path);
    REQUIRE(scheme == "https");
    REQUIRE(host == "example.com");
    REQUIRE(port == 8443);
    REQUIRE(base_path == "/path/file");
}

TEST_CASE("resolve_redirect_url handles absolute and relative redirects") {
    const std::string base_url = "https://example.com/path/file";
    REQUIRE(sip_gateway::utils::resolve_redirect_url(base_url, "/new") ==
            "https://example.com/new");
    REQUIRE(sip_gateway::utils::resolve_redirect_url(base_url, "other") ==
            "https://example.com/path/other");
    REQUIRE(sip_gateway::utils::resolve_redirect_url(base_url, "https://host/x") ==
            "https://host/x");
}
