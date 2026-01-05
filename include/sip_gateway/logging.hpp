#pragma once

#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>

#include "sip_gateway/config.hpp"
#include "spdlog/logger.h"

namespace sip_gateway {
namespace logging {

struct KeyValue {
    std::string key;
    std::string value;
};

template <typename T>
inline KeyValue kv(const std::string& key, const T& value) {
    std::ostringstream oss;
    oss << value;
    return {key, oss.str()};
}

inline std::string format_kv(std::initializer_list<KeyValue> items) {
    std::string result;
    for (const auto& item : items) {
        if (!result.empty()) {
            result += ", ";
        }
        bool needs_quotes = item.value.find(' ') != std::string::npos;
        result += item.key;
        result += '=';
        if (needs_quotes) {
            result += '"';
            result += item.value;
            result += '"';
        } else {
            result += item.value;
        }
    }
    return result;
}

inline std::string with_kv(const std::string& message,
                           std::initializer_list<KeyValue> items) {
    const auto context = format_kv(items);
    if (context.empty()) {
        return message;
    }
    return message + " [" + context + "]";
}

void init(const Config& config);
std::shared_ptr<spdlog::logger> get_logger();

inline void log(spdlog::level::level_enum level,
                const std::string& message,
                std::initializer_list<KeyValue> items = {}) {
    auto logger = get_logger();
    if (logger && logger->should_log(level)) {
        logger->log(level, with_kv(message, items));
    }
}

inline void trace(const std::string& message,
                  std::initializer_list<KeyValue> items = {}) {
    log(spdlog::level::trace, message, items);
}

inline void debug(const std::string& message,
                  std::initializer_list<KeyValue> items = {}) {
    log(spdlog::level::debug, message, items);
}

inline void info(const std::string& message,
                 std::initializer_list<KeyValue> items = {}) {
    log(spdlog::level::info, message, items);
}

inline void warn(const std::string& message,
                 std::initializer_list<KeyValue> items = {}) {
    log(spdlog::level::warn, message, items);
}

inline void error(const std::string& message,
                  std::initializer_list<KeyValue> items = {}) {
    log(spdlog::level::err, message, items);
}

}

using logging::kv;
using logging::with_kv;
using logging::debug;
using logging::error;
using logging::info;
using logging::trace;
using logging::warn;

}
