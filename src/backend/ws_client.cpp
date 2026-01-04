#include "sip_gateway/backend/ws_client.hpp"

#include <chrono>
#include <memory>
#include <thread>

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace sip_gateway {

namespace {

using WsClient = websocketpp::client<websocketpp::config::asio_client>;

std::string replace_scheme(const std::string& base_url) {
    if (base_url.rfind("https://", 0) == 0) {
        return "wss://" + base_url.substr(8);
    }
    if (base_url.rfind("http://", 0) == 0) {
        return "ws://" + base_url.substr(7);
    }
    return "ws://" + base_url;
}
}

struct BackendWsClient::WsState {
    std::shared_ptr<WsClient> client;
    websocketpp::connection_hdl connection;
};


BackendWsClient::BackendWsClient(std::string base_url)
    : base_url_(std::move(base_url)) {}

BackendWsClient::~BackendWsClient() {
    stop();
}

void BackendWsClient::connect(const std::string& session_id,
                              MessageHandler on_message,
                              EventHandler on_timeout,
                              EventHandler on_close) {
    if (running_) {
        return;
    }
    session_id_ = session_id;
    on_message_ = std::move(on_message);
    on_timeout_ = std::move(on_timeout);
    on_close_ = std::move(on_close);
    running_ = true;
    worker_ = std::thread([this]() { run_loop(); });
}

void BackendWsClient::send_json(const nlohmann::json& payload) {
    std::lock_guard<std::mutex> lock(ws_mutex_);
    if (!ws_state_ || !ws_state_->client || ws_state_->connection.expired()) {
        return;
    }
    websocketpp::lib::error_code ec;
    ws_state_->client->send(ws_state_->connection, payload.dump(),
                            websocketpp::frame::opcode::text, ec);
}

void BackendWsClient::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(ws_mutex_);
        if (ws_state_ && ws_state_->client && !ws_state_->connection.expired()) {
            websocketpp::lib::error_code ec;
            ws_state_->client->close(ws_state_->connection,
                                     websocketpp::close::status::going_away,
                                     "shutdown", ec);
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void BackendWsClient::run_loop() {
    while (running_) {
        auto client = std::make_shared<WsClient>();
        client->clear_access_channels(websocketpp::log::alevel::all);
        client->clear_error_channels(websocketpp::log::elevel::all);
        client->init_asio();

        client->set_message_handler([this](websocketpp::connection_hdl,
                                           WsClient::message_ptr msg) {
            try {
                auto payload = nlohmann::json::parse(msg->get_payload());
                const auto type = payload.value("type", "");
                if (type == "timeout") {
                    if (on_timeout_) {
                        on_timeout_();
                    }
                } else if (type == "close") {
                    if (on_close_) {
                        on_close_();
                    }
                } else if (on_message_) {
                    on_message_(payload);
                }
            } catch (...) {
            }
        });
        client->set_close_handler([this](websocketpp::connection_hdl) {
            if (on_close_) {
                on_close_();
            }
        });
        client->set_fail_handler([this](websocketpp::connection_hdl) {
            if (on_close_) {
                on_close_();
            }
        });

        websocketpp::lib::error_code ec;
        auto conn = client->get_connection(make_ws_url(session_id_), ec);
        if (ec) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(ws_mutex_);
            ws_state_ = std::make_unique<WsState>();
            ws_state_->client = client;
            ws_state_->connection = conn->get_handle();
        }
        client->connect(conn);
        client->run();

        {
            std::lock_guard<std::mutex> lock(ws_mutex_);
            ws_state_.reset();
        }
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

std::string BackendWsClient::make_ws_url(const std::string& session_id) const {
    return replace_scheme(base_url_) + "/ws/" + session_id;
}

}
