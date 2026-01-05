#include "sip_gateway/logging.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"


namespace sip_gateway::logging {

namespace {

spdlog::level::level_enum parse_level(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "TRACE") return spdlog::level::trace;
    if (value == "DEBUG") return spdlog::level::debug;
    if (value == "WARN") return spdlog::level::warn;
    if (value == "ERROR") return spdlog::level::err;
    if (value == "CRITICAL") return spdlog::level::critical;
    if (value == "OFF") return spdlog::level::off;
    return spdlog::level::info;
}

}

void init(const Config& config) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (config.log_filename) {
        const std::filesystem::path log_path(*config.log_filename);
        if (!log_path.parent_path().empty()) {
            std::filesystem::create_directories(log_path.parent_path());
        }
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(),
                                                                            true));
    }

    auto logger = std::make_shared<spdlog::logger>("sip_gateway", sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    spdlog::set_level(parse_level(config.log_level));
}

std::shared_ptr<spdlog::logger> get_logger() {
    if (auto logger = spdlog::get("sip_gateway")) {
        return logger;
    }
    return spdlog::default_logger();
}

}

