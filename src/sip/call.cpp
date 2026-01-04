#include "sip_gateway/sip/call.hpp"

#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"

namespace sip_gateway {

SipCall::SipCall(SipApp& app, pj::Account& account, std::string backend_url, int call_id)
    : pj::Call(account, call_id),
      app_(app),
      ws_client_(std::move(backend_url)) {}

SipCall::~SipCall() {
    stop_ws();
}

void SipCall::set_session_id(const std::string& session_id) {
    session_id_ = session_id;
}

const std::optional<std::string>& SipCall::session_id() const {
    return session_id_;
}

void SipCall::make_call(const std::string& to_uri) {
    to_uri_ = to_uri;
    pj::CallOpParam prm(true);
    pj::Call::makeCall(to_uri, prm);
}

void SipCall::answer(int status_code) {
    pj::CallOpParam prm(true);
    prm.statusCode = static_cast<pjsip_status_code>(status_code);
    pj::Call::answer(prm);
}

void SipCall::hangup(int status_code) {
    pj::CallOpParam prm(true);
    prm.statusCode = static_cast<pjsip_status_code>(status_code);
    pj::Call::hangup(prm);
}

void SipCall::connect_ws(BackendWsClient::MessageHandler on_message,
                         BackendWsClient::EventHandler on_timeout,
                         BackendWsClient::EventHandler on_close) {
    if (!session_id_) {
        logging::get_logger()->warn("WebSocket connect skipped: session_id is not set");
        return;
    }
    ws_client_.connect(*session_id_, std::move(on_message), std::move(on_timeout),
                       std::move(on_close));
}

void SipCall::stop_ws() {
    ws_client_.stop();
}

void SipCall::onCallState(pj::OnCallStateParam& prm) {
    (void)prm;
    try {
        const auto info = getInfo();
        logging::get_logger()->debug(with_kv(
            "Call state changed",
            {kv("call_id", info.callIdString),
             kv("uri", info.remoteUri),
             kv("state", static_cast<int>(info.state)),
             kv("state_text", info.stateText),
             kv("session_id", session_id_.value_or(""))}));
        if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
            app_.handle_call_disconnected(getId());
        }
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "Call state handler exception",
            {kv("error", ex.what())}));
    }
}

void SipCall::onCallMediaState(pj::OnCallMediaStateParam& prm) {
    (void)prm;
    try {
        logging::get_logger()->debug(with_kv(
            "Call media state changed",
            {kv("session_id", session_id_.value_or(""))}));
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "Call media handler exception",
            {kv("error", ex.what())}));
    }
}

}
