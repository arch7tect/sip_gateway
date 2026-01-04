#include "sip_gateway/config.hpp"
#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"

#include <string>

int main() {
    try {
        const auto config = sip_gateway::Config::load();
        config.validate();
        sip_gateway::logging::init(config);
        sip_gateway::info(
            "Starting sip-gateway",
            {sip_gateway::kv("backend_url", config.backend_url),
             sip_gateway::kv("rest_port", config.sip_rest_api_port),
             sip_gateway::kv("main_thread_only", config.ua_main_thread_only),
             sip_gateway::kv("interruptions_allowed", config.interruptions_are_allowed)});
        sip_gateway::SipApp app(config);
        app.init();
        app.run();
    } catch (const std::exception& ex) {
        sip_gateway::error(
            "Startup failed",
            {sip_gateway::kv("error", ex.what())});
        return 1;
    }
    return 0;
}
