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
        logger->info("SIP registration state. [status={}, reason={}]", status_code, prm.reason);
        if (status_code / 100 == 5) {
            logger->error("SIP registration server error. [status={}, reason={}]", status_code,
                          prm.reason);
        } else if (status_code == 408) {
            logger->warn("SIP registration timeout. [status={}, reason={}]", status_code,
                         prm.reason);
        } else if (status_code == 200) {
            pj::PresenceStatus status;
            status.status = PJSUA_BUDDY_STATUS_ONLINE;
            status.note = "Ready to answer";
            setOnlineStatus(status);
            logger->info("SIP registration successful.");
        } else if (status_code != 0) {
            logger->warn("SIP registration failed. [status={}, reason={}]", status_code,
                         prm.reason);
        }
    } catch (const std::exception& ex) {
        logger->error("Exception in onRegState. [error_type={}, error={}]",
                      typeid(ex).name(), ex.what());
    }
}

void SipAccount::onIncomingCall(pj::OnIncomingCallParam& iprm) {
    auto logger = logging::get_logger();
    try {
        logger->info("Incoming call. [call_id={}]", iprm.callId);
        auto call = std::make_shared<SipCall>(app_, *this, app_.backend_url(), iprm.callId);
        call->answer(PJSIP_SC_RINGING);
        app_.register_call(call);
        const auto info = call->getInfo();
        app_.handle_incoming_call(call, info.remoteUri);
    } catch (const std::exception& ex) {
        logger->error("Exception in onIncomingCall. [error_type={}, error={}, call_id={}]",
                      typeid(ex).name(), ex.what(), iprm.callId);
    }
}

}
