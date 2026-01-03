#include "sip_gateway/sip/app.hpp"

namespace sip_gateway {

SipApp::SipApp(Config config) : config_(std::move(config)) {}

void SipApp::init() {}

void SipApp::run() {}

void SipApp::stop() {
    quitting_ = true;
}

}
