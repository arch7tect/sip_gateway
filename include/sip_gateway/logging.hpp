#pragma once

#include <memory>

#include "sip_gateway/config.hpp"
#include "spdlog/logger.h"

namespace sip_gateway {
namespace logging {

void init(const Config& config);
std::shared_ptr<spdlog::logger> get_logger();

}
}
