#pragma once

#include <atomic>
#include <optional>
#include <string>

#include "sip_gateway/backend/client.hpp"
#include "sip_gateway/backend/ws_client.hpp"
#include "sip_gateway/config.hpp"

namespace sip_gateway {

class SipApp {
public:
    explicit SipApp(Config config);

    void init();
    void run();
    void stop();

private:
    Config config_;
    BackendClient backend_client_;
    BackendWsClient ws_client_;
    std::optional<std::string> session_id_;
    std::atomic<bool> quitting_{false};
};

}
