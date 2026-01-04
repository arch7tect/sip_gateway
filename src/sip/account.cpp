#include "sip_gateway/sip/account.hpp"

#include <typeinfo>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"
#include "sip_gateway/sip/call.hpp"

namespace sip_gateway {

SipAccount::SipAccount(SipApp& app) : app_(app) {}

void SipAccount::onRegState(pj::OnRegStateParam& prm) {
    auto logger = logging::get_logger();
    try {
        const int status_code = static_cast<int>(prm.code);
        logger->info(with_kv("SIP registration state",
                             {kv("status", status_code),
                              kv("reason", prm.reason)}));
        if (status_code / 100 == 5) {
            logger->error(with_kv("SIP registration server error",
                                  {kv("status", status_code),
                                   kv("reason", prm.reason)}));
        } else if (status_code == 408) {
            logger->warn(with_kv("SIP registration timeout",
                                 {kv("status", status_code),
                                  kv("reason", prm.reason)}));
        } else if (status_code == 200) {
            pj::PresenceStatus status;
            status.status = PJSUA_BUDDY_STATUS_ONLINE;
            status.note = "Ready to answer";
            setOnlineStatus(status);
            logger->info("SIP registration successful.");
        } else if (status_code != 0) {
            logger->warn(with_kv("SIP registration failed",
                                 {kv("status", status_code),
                                  kv("reason", prm.reason)}));
        }
    } catch (const std::exception& ex) {
        logger->error(with_kv(
            "Exception in onRegState",
            {kv("error_type", typeid(ex).name()),
             kv("error", ex.what())}));
    }
}

void SipAccount::onIncomingCall(pj::OnIncomingCallParam& iprm) {
    auto logger = logging::get_logger();
    try {
        logger->info(with_kv(
            "Incoming call",
            {kv("call_id", iprm.callId)}));
        auto call = std::make_shared<SipCall>(app_, *this, app_.backend_url(), iprm.callId);
        call->answer(PJSIP_SC_RINGING);
        app_.register_call(call);
        const auto info = call->getInfo();
        app_.handle_incoming_call(call, info.remoteUri);
    } catch (const std::exception& ex) {
        logger->error(with_kv(
            "Exception in onIncomingCall",
            {kv("error_type", typeid(ex).name()),
             kv("error", ex.what()),
             kv("call_id", iprm.callId)}));
    }
}

}
