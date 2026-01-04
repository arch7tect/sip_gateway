#include "sip_gateway/sip/app.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>
#include <httplib.h>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/account.hpp"
#include "sip_gateway/sip/call.hpp"

namespace sip_gateway {

SipApp::SipApp(Config config)
    : config_(std::move(config)),
      backend_client_(config_.backend_url, config_.authorization_token,
                      {std::chrono::seconds(static_cast<int>(config_.backend_request_timeout)),
                       std::chrono::seconds(static_cast<int>(config_.backend_connect_timeout)),
                       std::chrono::seconds(static_cast<int>(config_.backend_sock_read_timeout))}),
      endpoint_(nullptr),
      account_(nullptr) {}

void SipApp::init() {
    auto logger = logging::get_logger();
    auto capabilities = backend_client_.get_json("/capabilities");
    logger->info("Backend capabilities received: {}", capabilities.dump());

    init_pjsip();
    start_rest_server();
}

void SipApp::run() {
    int consecutive_empty_cycles = 0;
    while (!quitting_) {
        const auto processed = handle_events();
        if (processed == 0) {
            ++consecutive_empty_cycles;
            const auto delay =
                consecutive_empty_cycles > 10 ? std::min(config_.async_delay * 2, 0.1)
                                              : config_.async_delay;
            std::this_thread::sleep_for(
                std::chrono::duration<double>(delay));
        } else {
            consecutive_empty_cycles = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void SipApp::stop() {
    quitting_ = true;
    stop_rest_server();
    shutdown_pjsip();
}

int SipApp::handle_events() {
    if (!endpoint_) {
        return 0;
    }
    try {
        const auto delay_ms = static_cast<int>(config_.events_delay * 1000.0);
        return endpoint_->libHandleEvents(delay_ms);
    } catch (const pj::Error& err) {
        logging::get_logger()->error("PJSIP handle events error: {} [{}]",
                                     err.reason, err.status);
    } catch (const std::exception& ex) {
        logging::get_logger()->error("PJSIP handle events exception: {}", ex.what());
    }
    return 0;
}

SipApp::BackendSession SipApp::create_backend_session(const std::string& user_id,
                                                      const std::string& name,
                                                      const std::string& conversation_id,
                                                      const nlohmann::json& kwargs,
                                                      const std::optional<std::string>& communication_id) {
    nlohmann::json payload;
    payload["user_id"] = user_id;
    payload["name"] = name;
    payload["type"] = "sip";
    payload["conversation_id"] = conversation_id;
    if (communication_id) {
        payload["communication_id"] = *communication_id;
    } else {
        payload["communication_id"] = nullptr;
    }
    payload["args"] = nlohmann::json::array();
    payload["kwargs"] = kwargs;

    auto response = backend_client_.post_multipart_json("/session_v2", "body", payload);
    const auto session = response.at("session");
    BackendSession result;
    result.session_id = session.at("session_id").get<std::string>();
    if (response.contains("greeting") && response["greeting"].is_string()) {
        result.greeting = response["greeting"].get<std::string>();
    }
    return result;
}

const std::string& SipApp::backend_url() const {
    return config_.backend_url;
}

void SipApp::start_rest_server() {
    rest_server_ = std::make_unique<httplib::Server>();
    auto logger = logging::get_logger();

    rest_server_->Get("/health", [logger](const httplib::Request&, httplib::Response& res) {
        nlohmann::json payload{{"status", "ok"}};
        res.set_content(payload.dump(), "application/json");
        logger->debug("Health check served");
    });

    rest_server_->Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("", "text/plain");
    });

    rest_server_->Post("/call", [this, logger](const httplib::Request& req, httplib::Response& res) {
        if (!authorize_request(req, res)) {
            return;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
            if (!body.contains("to_uri")) {
                res.status = 400;
                res.set_content(R"({"message":"to_uri is required"})", "application/json");
                return;
            }
        } catch (const std::exception& ex) {
            logger->error("Failed to parse /call request: {}", ex.what());
            res.status = 400;
            res.set_content(R"({"message":"invalid request body"})", "application/json");
            return;
        }
        try {
            const auto to_uri = body.at("to_uri").get<std::string>();
            nlohmann::json env_info = nlohmann::json::object();
            if (body.contains("env_info") && body["env_info"].is_object()) {
                env_info = body["env_info"];
            }
            std::optional<std::string> communication_id;
            if (body.contains("communication_id") && body["communication_id"].is_string()) {
                communication_id = body["communication_id"].get<std::string>();
            }
            logger->info("Making outbound call. [to_uri={}, communication_id={}]", to_uri,
                         communication_id.value_or(""));
            auto backend_session =
                create_backend_session(to_uri, "", "", env_info, communication_id);
            if (!account_) {
                res.status = 503;
                res.set_content(R"({"message":"sip not initialized"})", "application/json");
                return;
            }
            auto call = std::make_shared<SipCall>(*this, *account_, backend_url());
            bind_session(call, backend_session.session_id);
            call->connect_ws(
                [logger](const nlohmann::json& message) {
                    logger->debug("WebSocket message received: {}", message.dump());
                },
                [logger]() {
                    logger->info("WebSocket timeout received");
                },
                [logger]() {
                    logger->info("WebSocket close received");
                });
            call->make_call(to_uri);
            register_call(call);
            nlohmann::json payload{{"message", "ok"}, {"session_id", backend_session.session_id}};
            res.set_content(payload.dump(), "application/json");
            res.status = 200;
        } catch (const std::exception& ex) {
            logger->error("Failed to create backend session: {}", ex.what());
            res.status = 500;
            res.set_content(R"({"message":"failed to start session"})", "application/json");
        }
    });

    rest_server_->Post(R"(/transfer/([A-Za-z0-9_-]+))",
                       [this, logger](const httplib::Request& req, httplib::Response& res) {
        if (!authorize_request(req, res)) {
            return;
        }
        (void)req;
        res.status = 501;
        res.set_content(R"({"message":"transfer handling not implemented"})", "application/json");
    });

    rest_thread_ = std::thread([this, logger]() {
        logger->info("REST server listening on port {}", config_.sip_rest_api_port);
        rest_server_->listen("0.0.0.0", config_.sip_rest_api_port);
    });
}

void SipApp::stop_rest_server() {
    if (rest_server_) {
        rest_server_->stop();
    }
    if (rest_thread_.joinable()) {
        rest_thread_.join();
    }
}

bool SipApp::authorize_request(const httplib::Request& request,
                               httplib::Response& response) const {
    if (!config_.authorization_token) {
        return true;
    }
    const auto it = request.headers.find("Authorization");
    if (it == request.headers.end()) {
        response.status = 401;
        response.set_content(R"({"message":"missing authorization"})", "application/json");
        return false;
    }
    const auto expected = "Bearer " + *config_.authorization_token;
    if (it->second != expected) {
        response.status = 403;
        response.set_content(R"({"message":"invalid authorization"})", "application/json");
        return false;
    }
    return true;
}

void SipApp::handle_incoming_call(const std::shared_ptr<SipCall>& call,
                                  const std::string& from_uri) {
    auto logger = logging::get_logger();
    nlohmann::json env_info = nlohmann::json::object();
    auto call_info = call->getInfo();
    auto backend_session =
        create_backend_session(from_uri, "", call_info.callIdString, env_info, std::nullopt);
    bind_session(call, backend_session.session_id);
    call->connect_ws(
        [logger](const nlohmann::json& message) {
            logger->debug("WebSocket message received: {}", message.dump());
        },
        [logger]() {
            logger->info("WebSocket timeout received");
        },
        [logger]() {
            logger->info("WebSocket close received");
        });
    call->answer(PJSIP_SC_OK);
}

void SipApp::register_call(const std::shared_ptr<SipCall>& call) {
    std::lock_guard<std::mutex> lock(calls_mutex_);
    const auto call_id = call->getId();
    if (call_id != PJSUA_INVALID_ID) {
        calls_[call_id] = call;
        if (call->session_id()) {
            session_calls_[*call->session_id()] = call_id;
        }
    }
}

void SipApp::bind_session(const std::shared_ptr<SipCall>& call,
                          const std::string& session_id) {
    call->set_session_id(session_id);
    std::lock_guard<std::mutex> lock(calls_mutex_);
    const auto call_id = call->getId();
    if (call_id != PJSUA_INVALID_ID) {
        calls_[call_id] = call;
        session_calls_[session_id] = call_id;
    }
}

void SipApp::unregister_call(int call_id) {
    std::lock_guard<std::mutex> lock(calls_mutex_);
    auto it = calls_.find(call_id);
    if (it == calls_.end()) {
        return;
    }
    const auto session_id = it->second->session_id();
    it->second->stop_ws();
    calls_.erase(it);
    if (session_id) {
        session_calls_.erase(*session_id);
    }
}

void SipApp::handle_call_disconnected(int call_id) {
    unregister_call(call_id);
}

void SipApp::init_pjsip() {
    auto logger = logging::get_logger();
    endpoint_ = std::make_unique<pj::Endpoint>();
    endpoint_->libCreate();

    pj::EpConfig ep_cfg;
    ep_cfg.uaConfig.threadCnt = 1;
    if (config_.ua_zero_thread_cnt) {
        ep_cfg.uaConfig.threadCnt = 0;
    }
    ep_cfg.uaConfig.mainThreadOnly = config_.ua_main_thread_only;
    ep_cfg.uaConfig.maxCalls = 32;
    ep_cfg.medConfig.threadCnt = 1;
    ep_cfg.medConfig.hasIoqueue = true;
    ep_cfg.medConfig.noVad = config_.ec_no_vad;
    ep_cfg.medConfig.ecTailLen = config_.ec_tail_len;
    ep_cfg.medConfig.ecOptions = (
        PJMEDIA_ECHO_WEBRTC_AEC3 |
        PJMEDIA_ECHO_USE_GAIN_CONTROLLER |
        PJMEDIA_ECHO_USE_NOISE_SUPPRESSOR);
    ep_cfg.medConfig.sndAutoCloseTime = -1;
    ep_cfg.logConfig.level = config_.pjsip_log_level;
    if (config_.log_filename) {
        ep_cfg.logConfig.filename = *config_.log_filename;
    }
    if (!config_.sip_stun_servers.empty()) {
        pj::StringVector stun_servers;
        for (const auto& stun_server : config_.sip_stun_servers) {
            stun_servers.push_back(stun_server);
        }
        ep_cfg.uaConfig.stunServer = stun_servers;
    }
    endpoint_->libInit(ep_cfg);

    for (const auto& item : config_.codecs_priority) {
        endpoint_->codecSetPriority(item.first, item.second);
    }
    for (const auto& codec : endpoint_->codecEnum2()) {
        logger->info("Supported codec. [codec_id={}, priority={}]", codec.codecId, codec.priority);
    }
    if (config_.sip_null_device) {
        endpoint_->audDevManager().setNullDev();
    }
    pj::TransportConfig sip_tp_config;
    sip_tp_config.port = config_.sip_port;
    endpoint_->transportCreate(PJSIP_TRANSPORT_UDP, sip_tp_config);
    if (config_.sip_use_tcp) {
        endpoint_->transportCreate(PJSIP_TRANSPORT_TCP, sip_tp_config);
    }
    endpoint_->libStart();

    pj::AccountConfig account_cfg;
    if (config_.sip_caller_id) {
        account_cfg.idUri = "\"" + *config_.sip_caller_id + "\" <sip:" + config_.sip_user +
                            "@" + config_.sip_domain + ">";
    } else {
        account_cfg.idUri = "sip:" + config_.sip_user + "@" + config_.sip_domain;
    }
    account_cfg.regConfig.registrarUri =
        "sip:" + config_.sip_domain + (config_.sip_use_tcp ? ";transport=tcp" : "");
    pj::AuthCredInfo cred("digest", "*", config_.sip_login, 0, config_.sip_password);
    account_cfg.sipConfig.authCreds.push_back(cred);
    if (!config_.sip_proxy_servers.empty()) {
        pj::StringVector proxy_servers;
        for (const auto& proxy_server : config_.sip_proxy_servers) {
            proxy_servers.push_back(proxy_server);
        }
        account_cfg.sipConfig.proxies = proxy_servers;
    }
    account_cfg.natConfig.iceEnabled = config_.sip_use_ice;

    account_ = std::make_unique<SipAccount>(*this);
    account_->create(account_cfg);
}

void SipApp::shutdown_pjsip() {
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto& entry : calls_) {
            entry.second->stop_ws();
        }
        calls_.clear();
        session_calls_.clear();
    }
    if (account_) {
        account_->shutdown();
        account_.reset();
    }
    if (endpoint_) {
        endpoint_->libDestroy();
        endpoint_.reset();
    }
}

}
