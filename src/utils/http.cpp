#include "sip_gateway/utils/http.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <httplib.h>
#include <iomanip>
#include <memory>
#include <sstream>

#include "sip_gateway/logging.hpp"

namespace sip_gateway::utils {

void parse_url(const std::string& url, std::string& scheme, std::string& host,
               int& port, std::string& base_path) {
    std::string working = url;
    scheme = "http";
    base_path = "";
    host.clear();
    port = 0;

    const auto scheme_pos = working.find("://");
    if (scheme_pos != std::string::npos) {
        scheme = working.substr(0, scheme_pos);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        working = working.substr(scheme_pos + 3);
    }

    const auto path_pos = working.find('/');
    if (path_pos != std::string::npos) {
        base_path = working.substr(path_pos);
        working = working.substr(0, path_pos);
    } else {
        base_path = "/";
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

std::string build_url(const std::string& scheme,
                      const std::string& host,
                      int port,
                      const std::string& path) {
    std::ostringstream out;
    out << scheme << "://" << host;
    const bool default_port = (scheme == "https" && port == 443) ||
                              (scheme == "http" && port == 80);
    if (!default_port && port > 0) {
        out << ":" << port;
    }
    if (!path.empty() && path.front() != '/') {
        out << '/';
    }
    out << path;
    return out.str();
}

std::string resolve_redirect_url(const std::string& base_url,
                                 const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    std::string scheme;
    std::string host;
    std::string base_path;
    int port = 0;
    parse_url(base_url, scheme, host, port, base_path);
    if (location.empty()) {
        return "";
    }
    if (location.front() == '/') {
        return build_url(scheme, host, port, location);
    }
    const auto slash = base_path.find_last_of('/');
    const std::string base_dir = (slash == std::string::npos)
                                     ? "/"
                                     : base_path.substr(0, slash + 1);
    return build_url(scheme, host, port, base_dir + location);
}

bool download_file(const std::string& url, const std::filesystem::path& path) {
    std::string current_url = url;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string scheme;
        std::string host;
        std::string base_path;
        int port = 0;
        parse_url(current_url, scheme, host, port, base_path);
        if (host.empty()) {
            return false;
        }
        std::unique_ptr<httplib::Client> http_client;
        std::unique_ptr<httplib::SSLClient> https_client;
        if (scheme == "https") {
            https_client = std::make_unique<httplib::SSLClient>(host, port);
            https_client->enable_server_certificate_verification(false);
            https_client->set_url_encode(false);
        } else {
            http_client = std::make_unique<httplib::Client>(host, port);
            http_client->set_url_encode(false);
        }

        const httplib::Headers request_headers = {
            {"User-Agent", "sip-gateway/1.0"},
            {"Accept", "*/*"},
            {"Host", host}
        };
        auto get_response = [&](const std::string& path_part) {
            if (https_client) {
                return https_client->Get(path_part.c_str(), request_headers);
            }
            return http_client->Get(path_part.c_str(), request_headers);
        };

        auto response = get_response(base_path);
        if (!response) {
            logging::error(
                "HTTP download request failed",
                {kv("url", current_url)});
            return false;
        }
        if (response->status >= 200 && response->status < 300) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary);
            out.write(response->body.data(),
                      static_cast<std::streamsize>(response->body.size()));
            out.close();
            return static_cast<bool>(out);
        }
        if (response->status == 301 || response->status == 302 ||
            response->status == 303 || response->status == 307 ||
            response->status == 308) {
            auto location_it = response->headers.find("Location");
            if (location_it == response->headers.end()) {
                logging::error(
                    "HTTP download redirect missing location",
                    {kv("status", response->status),
                     kv("url", current_url)});
                return false;
            }
            const auto next_url = resolve_redirect_url(current_url, location_it->second);
            if (next_url.empty()) {
                logging::error(
                    "HTTP download redirect invalid",
                    {kv("location", location_it->second),
                     kv("url", current_url)});
                return false;
            }
            logging::info(
                "HTTP download redirect",
                {kv("status", response->status),
                 kv("from", current_url),
                 kv("to", next_url)});
            current_url = next_url;
            continue;
        }
        if (!response->body.empty()) {
            const size_t limit = 256;
            std::string snippet = response->body.substr(0, limit);
            logging::error(
                "HTTP download failed",
                {kv("status", response->status),
                 kv("url", current_url),
                 kv("response", snippet)});
        } else {
            logging::error(
                "HTTP download failed",
                {kv("status", response->status),
                 kv("url", current_url)});
        }
        return false;
    }
    logging::error(
        "HTTP download failed: too many redirects",
        {kv("url", current_url)});
    return false;
}

std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            escaped << ch;
        } else {
            escaped << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(ch);
        }
    }
    return escaped.str();
}

}
