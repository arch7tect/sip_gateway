#pragma once

#include <optional>
#include <string>

#include <pjsua2.hpp>

#include "sip_gateway/backend/ws_client.hpp"

namespace sip_gateway {

class SipApp;

class SipCall : public pj::Call {
public:
    SipCall(SipApp& app,
            pj::Account& account,
            std::string backend_url,
            int call_id = PJSUA_INVALID_ID);
    ~SipCall() override;

    void set_session_id(const std::string& session_id);
    const std::optional<std::string>& session_id() const;

    void make_call(const std::string& to_uri);
    void answer(int status_code);
    void hangup(int status_code);

    void connect_ws(BackendWsClient::MessageHandler on_message,
                    BackendWsClient::EventHandler on_timeout,
                    BackendWsClient::EventHandler on_close);
    void stop_ws();

    void onCallState(pj::OnCallStateParam& prm) override;
    void onCallMediaState(pj::OnCallMediaStateParam& prm) override;

private:
    SipApp& app_;
    BackendWsClient ws_client_;
    std::optional<std::string> session_id_;
    std::string to_uri_;
};

}
