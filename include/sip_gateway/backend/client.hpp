#pragma once

#include <chrono>
#include <functional>
#include <httplib.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace sip_gateway {

class BackendError : public std::runtime_error {
public:
    explicit BackendError(const std::string& message) : std::runtime_error(message) {}
};

class BackendPermissionError : public BackendError {
public:
    explicit BackendPermissionError(const std::string& message) : BackendError(message) {}
};

struct BackendRequestOptions {
    std::chrono::seconds request_timeout{60};
    std::chrono::seconds connect_timeout{60};
    std::chrono::seconds sock_read_timeout{60};
};

class BackendClient {
public:
    BackendClient(std::string base_url,
                  std::optional<std::string> authorization_token,
                  BackendRequestOptions options);

    nlohmann::json get_json(const std::string& path);
    nlohmann::json post_json(const std::string& path, const nlohmann::json& body);
    nlohmann::json post_multipart_json(const std::string& path,
                                       const std::string& field_name,
                                       const nlohmann::json& body);
    nlohmann::json put_json(const std::string& path, const nlohmann::json& body);
    nlohmann::json delete_json(const std::string& path);
    nlohmann::json post_binary(const std::string& path,
                               const std::string& content_type,
                               const std::string& payload);
    std::string get_binary(const std::string& path, const std::string& query);

private:
    std::string build_path(const std::string& path) const;
    void apply_timeouts();
    std::string scheme_;
    std::string host_;
    int port_;
    std::string base_path_;
    std::optional<std::string> authorization_token_;
    BackendRequestOptions options_;
    std::unique_ptr<httplib::Client> client_http_;
    std::unique_ptr<httplib::SSLClient> client_https_;
};

}
