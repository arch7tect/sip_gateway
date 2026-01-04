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
            result += " ";
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

}

using logging::kv;
using logging::with_kv;

}
