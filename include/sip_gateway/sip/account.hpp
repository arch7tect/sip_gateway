#pragma once

#include <pjsua2.hpp>

namespace spdlog {
class logger;
}

namespace sip_gateway {

class SipApp;

class SipAccount : public pj::Account {
public:
    explicit SipAccount(SipApp& app);

    void onRegState(pj::OnRegStateParam& prm) override;
    void onIncomingCall(pj::OnIncomingCallParam& iprm) override;

private:
    SipApp& app_;
};

}
