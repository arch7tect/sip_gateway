#pragma once

#include <atomic>

#include "sip_gateway/config.hpp"

namespace sip_gateway {

class SipApp {
public:
    explicit SipApp(Config config);

    void init();
    void run();
    void stop();

private:
    Config config_;
    std::atomic<bool> quitting_{false};
};

}
