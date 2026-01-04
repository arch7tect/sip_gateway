#include "sip_gateway/backend/client.hpp"

#include <algorithm>
#include <cctype>
#include <httplib.h>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sip_gateway {

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void parse_url(const std::string& url, std::string& scheme, std::string& host,
               int& port, std::string& base_path) {
    std::string working = url;
    scheme = "http";
    base_path = "";

    const auto scheme_pos = working.find("://");
    if (scheme_pos != std::string::npos) {
        scheme = to_lower(working.substr(0, scheme_pos));
        working = working.substr(scheme_pos + 3);
    }

    const auto path_pos = working.find('/');
    if (path_pos != std::string::npos) {
        base_path = working.substr(path_pos);
        working = working.substr(0, path_pos);
    }

    const auto port_pos = working.find(':');
    if (port_pos != std::string::npos) {
        host = working.substr(0, port_pos);
        port = std::stoi(working.substr(port_pos + 1));
    } else {
        host = working;
        port = scheme == "https" ? 443 : 80;
    }
}

}

BackendClient::BackendClient(std::string base_url,
                             std::optional<std::string> authorization_token,
                             BackendRequestOptions options)
    : authorization_token_(std::move(authorization_token)),
      options_(options) {
    parse_url(base_url, scheme_, host_, port_, base_path_);

    if (scheme_ == "https") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        client_https_ = std::make_unique<httplib::SSLClient>(host_, port_);
        client_https_->enable_server_certificate_verification(false);
#else
        throw BackendError("HTTPS backend requires CPPHTTPLIB_OPENSSL_SUPPORT");
#endif
    } else {
        client_http_ = std::make_unique<httplib::Client>(host_, port_);
    }
    apply_timeouts();
}

nlohmann::json BackendClient::get_json(const std::string& path) {
    auto headers = httplib::Headers{{"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    auto response = client_https_
                        ? client_https_->Get(build_path(path).c_str(), headers)
                        : client_http_->Get(build_path(path).c_str(), headers);
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

nlohmann::json BackendClient::post_json(const std::string& path, const nlohmann::json& body) {
    auto headers = httplib::Headers{{"Content-Type", "application/json"},
                                    {"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    auto response = client_https_
                        ? client_https_->Post(build_path(path).c_str(), headers, body.dump(),
                                              "application/json")
                        : client_http_->Post(build_path(path).c_str(), headers, body.dump(),
                                             "application/json");
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

nlohmann::json BackendClient::post_multipart_json(const std::string& path,
                                                  const std::string& field_name,
                                                  const nlohmann::json& body) {
    auto headers = httplib::Headers{{"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    httplib::MultipartFormDataItems items;
    items.push_back({field_name, body.dump(), "", "application/json"});
    auto response = client_https_
                        ? client_https_->Post(build_path(path).c_str(), headers, items)
                        : client_http_->Post(build_path(path).c_str(), headers, items);
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

nlohmann::json BackendClient::put_json(const std::string& path, const nlohmann::json& body) {
    auto headers = httplib::Headers{{"Content-Type", "application/json"},
                                    {"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    auto response = client_https_
                        ? client_https_->Put(build_path(path).c_str(), headers, body.dump(),
                                             "application/json")
                        : client_http_->Put(build_path(path).c_str(), headers, body.dump(),
                                            "application/json");
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

nlohmann::json BackendClient::delete_json(const std::string& path) {
    auto headers = httplib::Headers{{"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    auto response = client_https_
                        ? client_https_->Delete(build_path(path).c_str(), headers)
                        : client_http_->Delete(build_path(path).c_str(), headers);
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

nlohmann::json BackendClient::post_binary(const std::string& path,
                                          const std::string& content_type,
                                          const std::string& payload) {
    auto headers = httplib::Headers{{"Content-Type", content_type},
                                    {"Accept", "application/json"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    auto response = client_https_
                        ? client_https_->Post(build_path(path).c_str(), headers, payload,
                                              content_type)
                        : client_http_->Post(build_path(path).c_str(), headers, payload,
                                             content_type);
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return nlohmann::json::parse(response->body);
}

std::string BackendClient::get_binary(const std::string& path, const std::string& query) {
    auto headers = httplib::Headers{{"Accept", "*/*"}};
    if (authorization_token_) {
        headers.emplace("Authorization", "Bearer " + *authorization_token_);
    }
    const auto full_path = build_path(path) + "?" + query;
    auto response = client_https_
                        ? client_https_->Get(full_path.c_str(), headers)
                        : client_http_->Get(full_path.c_str(), headers);
    if (!response) {
        throw BackendError("Backend request failed");
    }
    if (response->status == 403) {
        throw BackendPermissionError(response->body);
    }
    if (response->status < 200 || response->status >= 300) {
        throw BackendError(response->body);
    }
    return response->body;
}

std::string BackendClient::build_path(const std::string& path) const {
    if (base_path_.empty()) {
        return path;
    }
    if (path.empty()) {
        return base_path_;
    }
    if (base_path_.back() == '/' && path.front() == '/') {
        return base_path_ + path.substr(1);
    }
    if (base_path_.back() != '/' && path.front() != '/') {
        return base_path_ + "/" + path;
    }
    return base_path_ + path;
}

void BackendClient::apply_timeouts() {
    if (client_https_) {
        client_https_->set_connection_timeout(options_.connect_timeout.count(), 0);
        client_https_->set_read_timeout(options_.sock_read_timeout.count(), 0);
        client_https_->set_write_timeout(options_.request_timeout.count(), 0);
        return;
    }
    if (client_http_) {
        client_http_->set_connection_timeout(options_.connect_timeout.count(), 0);
        client_http_->set_read_timeout(options_.sock_read_timeout.count(), 0);
        client_http_->set_write_timeout(options_.request_timeout.count(), 0);
        return;
    }
}

}
