#pragma once

#include <filesystem>
#include <string>

namespace sip_gateway::utils {

void parse_url(const std::string& url, std::string& scheme, std::string& host,
               int& port, std::string& base_path);

std::string build_url(const std::string& scheme,
                      const std::string& host,
                      int port,
                      const std::string& path);

std::string resolve_redirect_url(const std::string& base_url,
                                 const std::string& location);

bool download_file(const std::string& url, const std::filesystem::path& path);

std::string url_encode(const std::string& value);

}
