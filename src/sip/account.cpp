#include "sip_gateway/sip/account.hpp"

#include <typeinfo>

#include "sip_gateway/backend/client.hpp"
#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"
#include "sip_gateway/sip/call.hpp"

namespace sip_gateway {

SipAccount::SipAccount(SipApp& app) : app_(app) {}

void SipAccount::onRegState(pj::OnRegStateParam& prm) {
    try {
        const int status_code = static_cast<int>(prm.code);
        logging::info("SIP registration state",
                      {kv("status", status_code),
                       kv("reason", prm.reason)});
        if (status_code / 100 == 5) {
            logging::error("SIP registration server error",
                           {kv("status", status_code),
                            kv("reason", prm.reason)});
        } else if (status_code == 408) {
            logging::warn("SIP registration timeout",
                          {kv("status", status_code),
                           kv("reason", prm.reason)});
        } else if (status_code == 200) {
            pj::PresenceStatus status;
            status.status = PJSUA_BUDDY_STATUS_ONLINE;
            status.note = "Ready to answer";
            setOnlineStatus(status);
            logging::info("SIP registration successful.");
        } else if (status_code != 0) {
            logging::warn("SIP registration failed",
                          {kv("status", status_code),
                           kv("reason", prm.reason)});
        }
    } catch (const std::exception& ex) {
        logging::error(
            "Exception in onRegState",
            {kv("error_type", typeid(ex).name()),
             kv("error", ex.what())});
    }
}

void SipAccount::onIncomingCall(pj::OnIncomingCallParam& iprm) {
    if (!app_.config().allow_inbound_calls) {
        logging::info(
            "Inbound call rejected (disabled)",
            {kv("call_id", iprm.callId)});
        auto call = std::make_shared<SipCall>(app_, *this, app_.backend_url(), iprm.callId);
        call->hangup(PJSIP_SC_FORBIDDEN);
        return;
    }

    logging::info(
        "Incoming call",
        {kv("call_id", iprm.callId)});
    auto call = std::make_shared<SipCall>(app_, *this, app_.backend_url(), iprm.callId);
    call->answer(PJSIP_SC_RINGING);
    app_.register_call(call);
    try {
        const auto info = call->getInfo();
        app_.handle_incoming_call(call, info.remoteUri);
    } catch (const BackendError& ex) {
        logging::error(
            "Incoming call backend error",
            {kv("error", ex.what()),
             kv("call_id", iprm.callId)});
        call->hangup(PJSIP_SC_SERVICE_UNAVAILABLE);
        app_.unregister_call(iprm.callId);
    } catch (const std::exception& ex) {
        logging::error(
            "Exception in onIncomingCall",
            {kv("error_type", typeid(ex).name()),
             kv("error", ex.what()),
             kv("call_id", iprm.callId)});
        call->hangup(PJSIP_SC_INTERNAL_SERVER_ERROR);
        app_.unregister_call(iprm.callId);
    }
}

}
