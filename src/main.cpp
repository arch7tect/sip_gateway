#include "sip_gateway/config.hpp"
#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"

#include <string>

#include "spdlog/spdlog.h"

int main() {
    try {
        const auto config = sip_gateway::Config::load();
        config.validate();
        sip_gateway::logging::init(config);
        auto logger = sip_gateway::logging::get_logger();
        logger->info("Starting sip-gateway. backend_url={}, rest_port={}, main_thread_only={}",
                     config.backend_url, config.sip_rest_api_port, config.ua_main_thread_only);
        sip_gateway::SipApp app(config);
        app.init();
        app.run();
    } catch (const std::exception& ex) {
        spdlog::error("Startup failed: {}", ex.what());
        return 1;
    }
    return 0;
}
