#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace sip_gateway {

class BackendWsClient {
public:
    using MessageHandler = std::function<void(const nlohmann::json&)>;
    using EventHandler = std::function<void()>;

    explicit BackendWsClient(std::string base_url);
    ~BackendWsClient();

    void connect(const std::string& session_id,
                 MessageHandler on_message,
                 EventHandler on_timeout,
                 EventHandler on_close);
    void send_json(const nlohmann::json& payload);
    void stop();

private:
    void run_loop();
    std::string make_ws_url(const std::string& session_id) const;

    std::string base_url_;
    std::string session_id_;
    MessageHandler on_message_;
    EventHandler on_timeout_;
    EventHandler on_close_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex ws_mutex_;
    struct WsState;
    std::unique_ptr<WsState> ws_state_;
};

}
