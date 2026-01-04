#include "sip_gateway/sip/app.hpp"

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "sip_gateway/logging.hpp"

namespace sip_gateway {

SipApp::SipApp(Config config)
    : config_(std::move(config)),
      backend_client_(config_.backend_url, config_.authorization_token,
                      {std::chrono::seconds(static_cast<int>(config_.backend_request_timeout)),
                       std::chrono::seconds(static_cast<int>(config_.backend_connect_timeout)),
                       std::chrono::seconds(static_cast<int>(config_.backend_sock_read_timeout))}),
      ws_client_(config_.backend_url) {}

void SipApp::init() {
    auto logger = logging::get_logger();
    auto capabilities = backend_client_.get_json("/capabilities");
    logger->info("Backend capabilities received: {}", capabilities.dump());

    nlohmann::json payload;
    payload["user_id"] = config_.sip_user;
    payload["name"] = config_.sip_user;
    payload["type"] = config_.session_type;
    payload["conversation_id"] = nullptr;
    payload["args"] = nlohmann::json::array();
    payload["kwargs"] = nlohmann::json::object();

    auto response = backend_client_.post_json("/session", payload);
    const auto session = response.at("session");
    session_id_ = session.at("session_id").get<std::string>();
    logger->info("Backend session opened: {}", *session_id_);

    ws_client_.connect(
        *session_id_,
        [logger](const nlohmann::json& message) {
            logger->debug("WebSocket message received: {}", message.dump());
        },
        [logger]() {
            logger->info("WebSocket timeout received");
        },
        [logger]() {
            logger->info("WebSocket close received");
        });
}

void SipApp::run() {
    while (!quitting_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void SipApp::stop() {
    quitting_ = true;
    ws_client_.stop();
}

}
