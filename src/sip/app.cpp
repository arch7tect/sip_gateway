#include "sip_gateway/sip/app.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <httplib.h>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/account.hpp"
#include "sip_gateway/sip/call.hpp"
#include "sip_gateway/server/rest_server.hpp"
#include "sip_gateway/vad/model.hpp"

namespace sip_gateway {

namespace {

void parse_url(const std::string& url, std::string& scheme, std::string& host,
               int& port, std::string& base_path) {
    std::string working = url;
    scheme = "http";
    base_path = "";
    host.clear();
    port = 0;

    const auto scheme_pos = working.find("://");
    if (scheme_pos != std::string::npos) {
        scheme = working.substr(0, scheme_pos);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        working = working.substr(scheme_pos + 3);
    }

    const auto path_pos = working.find('/');
    if (path_pos != std::string::npos) {
        base_path = working.substr(path_pos);
        working = working.substr(0, path_pos);
    } else {
        base_path = "/";
    }

    const auto port_pos = working.find(':');
    if (port_pos != std::string::npos) {
        host = working.substr(0, port_pos);
        port = std::stoi(working.substr(port_pos + 1));
    } else {
        host = working;
        port = scheme == "https" ? 443 : 80;
    }
}

std::string build_url(const std::string& scheme,
                      const std::string& host,
                      int port,
                      const std::string& path) {
    std::ostringstream out;
    out << scheme << "://" << host;
    const bool default_port = (scheme == "https" && port == 443) ||
                              (scheme == "http" && port == 80);
    if (!default_port && port > 0) {
        out << ":" << port;
    }
    if (!path.empty() && path.front() != '/') {
        out << '/';
    }
    out << path;
    return out.str();
}

std::string resolve_redirect_url(const std::string& base_url,
                                 const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    std::string scheme;
    std::string host;
    std::string base_path;
    int port = 0;
    parse_url(base_url, scheme, host, port, base_path);
    if (location.empty()) {
        return "";
    }
    if (location.front() == '/') {
        return build_url(scheme, host, port, location);
    }
    const auto slash = base_path.find_last_of('/');
    const std::string base_dir = (slash == std::string::npos)
                                     ? "/"
                                     : base_path.substr(0, slash + 1);
    return build_url(scheme, host, port, base_dir + location);
}

bool download_file(const std::string& url, const std::filesystem::path& path) {
    auto logger = logging::get_logger();
    std::string current_url = url;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::string scheme;
        std::string host;
        std::string base_path;
        int port = 0;
        parse_url(current_url, scheme, host, port, base_path);
        if (host.empty()) {
            return false;
        }
        std::unique_ptr<httplib::Client> http_client;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        std::unique_ptr<httplib::SSLClient> https_client;
        if (scheme == "https") {
            https_client = std::make_unique<httplib::SSLClient>(host, port);
            https_client->enable_server_certificate_verification(false);
            https_client->set_url_encode(false);
        } else
#endif
        {
            http_client = std::make_unique<httplib::Client>(host, port);
            http_client->set_url_encode(false);
        }

        const httplib::Headers request_headers = {
            {"User-Agent", "sip-gateway/1.0"},
            {"Accept", "*/*"},
            {"Host", host}
        };
        auto get_response = [&](const std::string& path_part) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            if (https_client) {
                return https_client->Get(path_part.c_str(), request_headers);
            }
#endif
            return http_client->Get(path_part.c_str(), request_headers);
        };

        auto response = get_response(base_path);
        if (!response) {
            logger->error(with_kv(
                "VAD model download request failed",
                {kv("url", current_url)}));
            return false;
        }
        if (response->status >= 200 && response->status < 300) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary);
            out.write(response->body.data(),
                      static_cast<std::streamsize>(response->body.size()));
            out.close();
            return static_cast<bool>(out);
        }
        if (response->status == 301 || response->status == 302 ||
            response->status == 303 || response->status == 307 ||
            response->status == 308) {
            auto location_it = response->headers.find("Location");
            if (location_it == response->headers.end()) {
                logger->error(with_kv(
                    "VAD model download redirect missing location",
                    {kv("status", response->status),
                     kv("url", current_url)}));
                return false;
            }
            const auto next_url = resolve_redirect_url(current_url, location_it->second);
            if (next_url.empty()) {
                logger->error(with_kv(
                    "VAD model download redirect invalid",
                    {kv("location", location_it->second),
                     kv("url", current_url)}));
                return false;
            }
            logger->info(with_kv(
                "VAD model download redirect",
                {kv("status", response->status),
                 kv("from", current_url),
                 kv("to", next_url)}));
            current_url = next_url;
            continue;
        }
        if (!response->body.empty()) {
            const size_t limit = 256;
            std::string snippet = response->body.substr(0, limit);
            logger->error(with_kv(
                "VAD model download failed",
                {kv("status", response->status),
                 kv("url", current_url),
                 kv("response", snippet)}));
        } else {
            logger->error(with_kv(
                "VAD model download failed",
                {kv("status", response->status),
                 kv("url", current_url)}));
        }
        return false;
    }
    logger->error(with_kv(
        "VAD model download failed: too many redirects",
        {kv("url", current_url)}));
    return false;
}

std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            escaped << ch;
        } else {
            escaped << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(ch);
        }
    }
    return escaped.str();
}

}

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
    logger->info(with_kv(
        "Backend capabilities received",
        {kv("capabilities", capabilities.dump())}));

    init_pjsip();
    init_vad();
    rest_server_ = std::make_unique<RestServer>(
        config_,
        [this](const nlohmann::json& body) { return handle_call_request(body); },
        [this](const std::string& session_id, const nlohmann::json& body) {
            return handle_transfer_request(session_id, body);
        });
    rest_server_->start();
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
    if (rest_server_) {
        rest_server_->stop();
    }
    shutdown_pjsip();
}

const Config& SipApp::config() const {
    return config_;
}

std::string SipApp::synthesize_session_audio(const std::string& session_id,
                                             const std::string& text) {
    const auto query = "text=" + url_encode(text) + "&format=wav";
    return backend_client_.get_binary("/session/" + session_id + "/synthesize", query);
}

std::string SipApp::transcribe_audio(const std::string& wav_bytes) {
    auto response = backend_client_.post_binary("/transcribe", "audio/wav", wav_bytes);
    if (response.is_string()) {
        return response.get<std::string>();
    }
    if (response.is_object() && response.contains("text") && response["text"].is_string()) {
        return response["text"].get<std::string>();
    }
    return "";
}

nlohmann::json SipApp::start_session_text(const std::string& session_id,
                                          const std::string& text) {
    nlohmann::json payload;
    payload["message"] = text;
    payload["kwargs"] = nlohmann::json::object();
    return backend_client_.post_json("/session/" + session_id + "/start", payload);
}

nlohmann::json SipApp::commit_session(const std::string& session_id) {
    nlohmann::json payload = nlohmann::json::object();
    return backend_client_.post_json("/session/" + session_id + "/commit", payload);
}

nlohmann::json SipApp::rollback_session(const std::string& session_id) {
    nlohmann::json payload = nlohmann::json::object();
    return backend_client_.post_json("/session/" + session_id + "/rollback", payload);
}

std::shared_ptr<vad::VadModel> SipApp::vad_model() const {
    return vad_model_;
}

int SipApp::handle_events() {
    if (!endpoint_) {
        return 0;
    }
    try {
        const auto delay_ms = static_cast<int>(config_.events_delay * 1000.0);
        return endpoint_->libHandleEvents(delay_ms);
    } catch (const pj::Error& err) {
        logging::get_logger()->error(with_kv(
            "PJSIP handle events error",
            {kv("reason", err.reason),
             kv("status", err.status)}));
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "PJSIP handle events exception",
            {kv("error", ex.what())}));
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

RestResponse SipApp::handle_call_request(const nlohmann::json& body) {
    auto logger = logging::get_logger();
    if (!body.contains("to_uri")) {
        return {400, nlohmann::json{{"message", "to_uri is required"}}};
    }
    const auto to_uri = body.at("to_uri").get<std::string>();
    nlohmann::json env_info = nlohmann::json::object();
    if (body.contains("env_info") && body["env_info"].is_object()) {
        env_info = body["env_info"];
    }
    std::optional<std::string> communication_id;
    if (body.contains("communication_id") && body["communication_id"].is_string()) {
        communication_id = body["communication_id"].get<std::string>();
    }
    logger->info(with_kv(
        "Making outbound call",
        {kv("to_uri", to_uri),
         kv("communication_id", communication_id.value_or(""))}));
    auto backend_session =
        create_backend_session(to_uri, "", "", env_info, communication_id);
    if (!account_) {
        return {503, nlohmann::json{{"message", "sip not initialized"}}};
    }
    auto call = std::make_shared<SipCall>(*this, *account_, backend_url());
    bind_session(call, backend_session.session_id);
    call->set_greeting(backend_session.greeting);
    call->connect_ws(
        [call](const nlohmann::json& message) {
            call->handle_ws_message(message);
        },
        [call]() {
            call->handle_ws_timeout();
        },
        [call]() {
            call->handle_ws_close();
        });
    call->make_call(to_uri);
    register_call(call);
    return {200, nlohmann::json{{"message", "ok"}, {"session_id", backend_session.session_id}}};
}

RestResponse SipApp::handle_transfer_request(const std::string& session_id,
                                             const nlohmann::json& body) {
    auto logger = logging::get_logger();
    if (!body.contains("to_uri") || !body["to_uri"].is_string()) {
        return {400, nlohmann::json{{"message", "to_uri is required"}}};
    }
    const auto to_uri = body.at("to_uri").get<std::string>();
    double transfer_delay = 1.0;
    if (body.contains("transfer_delay") && body["transfer_delay"].is_number()) {
        transfer_delay = body["transfer_delay"].get<double>();
    }

    std::shared_ptr<SipCall> call;
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = session_calls_.find(session_id);
        if (it != session_calls_.end()) {
            auto call_it = calls_.find(it->second);
            if (call_it != calls_.end()) {
                call = call_it->second;
            }
        }
    }
    if (!call) {
        return {404, nlohmann::json{{"message", "session not found"}}};
    }
    try {
        const auto info = call->getInfo();
        if (info.state != PJSIP_INV_STATE_CONFIRMED) {
            return {400, nlohmann::json{{"message", "call is not active"}}};
        }
    } catch (const pj::Error& ex) {
        logger->error(with_kv(
            "Failed to inspect call state",
            {kv("reason", ex.reason),
             kv("status", ex.status),
             kv("session_id", session_id)}));
        return {500, nlohmann::json{{"message", "call state error"}}};
    }

    call->set_transfer_target(to_uri, transfer_delay);
    logger->info(with_kv(
        "Transfer target set",
        {kv("to_uri", to_uri),
         kv("transfer_delay", transfer_delay),
         kv("session_id", session_id)}));
    return {200, nlohmann::json{{"status", "ok"},
                                {"message", "Successfully transferred"},
                                {"session_id", session_id},
                                {"to_uri", to_uri}}};
}

void SipApp::handle_incoming_call(const std::shared_ptr<SipCall>& call,
                                  const std::string& from_uri) {
    nlohmann::json env_info = nlohmann::json::object();
    auto call_info = call->getInfo();
    auto backend_session =
        create_backend_session(from_uri, "", call_info.callIdString, env_info, std::nullopt);
    bind_session(call, backend_session.session_id);
    call->set_greeting(backend_session.greeting);
    call->connect_ws(
        [call](const nlohmann::json& message) {
            call->handle_ws_message(message);
        },
        [call]() {
            call->handle_ws_timeout();
        },
        [call]() {
            call->handle_ws_close();
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
    ep_cfg.uaConfig.maxCalls = config_.sip_max_calls;
    ep_cfg.medConfig.threadCnt = config_.sip_media_thread_cnt;
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
        logger->info(with_kv(
            "Supported codec",
            {kv("codec_id", codec.codecId),
             kv("priority", static_cast<int>(codec.priority))}));
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
    account_cfg.mediaConfig.srtpUse = PJMEDIA_SRTP_OPTIONAL;
    account_cfg.mediaConfig.srtpSecureSignaling = 0;
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

void SipApp::init_vad() {
    if (vad_model_) {
        return;
    }
    auto logger = logging::get_logger();
    try {
        logger->info(with_kv(
            "VAD model setup",
            {kv("path", config_.vad_model_path.string()),
             kv("url", config_.vad_model_url)}));
        if (!std::filesystem::exists(config_.vad_model_path)) {
            logger->info(with_kv(
                "VAD model file missing, downloading",
                {kv("path", config_.vad_model_path.string()),
                 kv("url", config_.vad_model_url)}));
            if (config_.vad_model_url.empty() ||
                !download_file(config_.vad_model_url, config_.vad_model_path)) {
                logger->error(with_kv(
                    "VAD model download failed",
                    {kv("path", config_.vad_model_path.string()),
                     kv("url", config_.vad_model_url)}));
                throw std::runtime_error("failed to download VAD model");
            }
            std::error_code size_ec;
            const auto size = std::filesystem::file_size(config_.vad_model_path, size_ec);
            if (size_ec || size == 0) {
                logger->error(with_kv(
                    "VAD model download produced empty file",
                    {kv("path", config_.vad_model_path.string()),
                     kv("url", config_.vad_model_url)}));
                throw std::runtime_error("downloaded VAD model is empty");
            }
        }
        vad_model_ = std::make_shared<vad::VadModel>(
            config_.vad_model_path, config_.vad_sampling_rate);
        logger->info(with_kv(
            "VAD model loaded",
            {kv("path", config_.vad_model_path.string()),
             kv("sampling_rate", config_.vad_sampling_rate)}));
    } catch (const std::exception& ex) {
        logger->error(with_kv(
            "VAD model load failed",
            {kv("error", ex.what()),
             kv("path", config_.vad_model_path.string()),
             kv("url", config_.vad_model_url)}));
        throw;
    }
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
