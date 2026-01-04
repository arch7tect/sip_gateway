#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>

#include "sip_gateway/backend/client.hpp"
#include "sip_gateway/config.hpp"
#include "sip_gateway/sip/account.hpp"
#include <nlohmann/json.hpp>
#include <pjsua2.hpp>

namespace httplib {
class Server;
class Request;
class Response;
}

namespace sip_gateway {

class SipAccount;
class SipCall;

class SipApp {
public:
    explicit SipApp(Config config);

    void init();
    void run();
    void stop();

private:
    friend class SipAccount;
    friend class SipCall;

    struct BackendSession {
        std::string session_id;
        std::optional<std::string> greeting;
    };

    void init_pjsip();
    void shutdown_pjsip();
    int handle_events();
    void handle_incoming_call(const std::shared_ptr<SipCall>& call, const std::string& from_uri);
    void handle_call_disconnected(int call_id);
    void register_call(const std::shared_ptr<SipCall>& call);
    void unregister_call(int call_id);
    void bind_session(const std::shared_ptr<SipCall>& call, const std::string& session_id);

    BackendSession create_backend_session(const std::string& user_id,
                                          const std::string& name,
                                          const std::string& conversation_id,
                                          const nlohmann::json& kwargs,
                                          const std::optional<std::string>& communication_id);
    void start_rest_server();
    void stop_rest_server();
    bool authorize_request(const httplib::Request& request, httplib::Response& response) const;

    const std::string& backend_url() const;

    Config config_;
    BackendClient backend_client_;
    std::unique_ptr<pj::Endpoint> endpoint_;
    std::unique_ptr<SipAccount> account_;
    std::unordered_map<int, std::shared_ptr<SipCall>> calls_;
    std::unordered_map<std::string, int> session_calls_;
    std::mutex calls_mutex_;
    std::atomic<bool> quitting_{false};
    std::unique_ptr<httplib::Server> rest_server_;
    std::thread rest_thread_;
};

}
