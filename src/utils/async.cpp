#include "sip_gateway/utils/async.hpp"

#include <pj/os.h>
#include <thread>


namespace sip_gateway::utils {

void ensure_pj_thread_registered(const char* name) {
    if (pj_thread_is_registered()) {
        return;
    }
    thread_local pj_thread_desc desc;
    pj_thread_t* thread = nullptr;
    pj_thread_register(name ? name : "sipgw", desc, &thread);
}

void run_async(std::function<void()> task) {
    std::thread worker([task = std::move(task)]() mutable {
        ensure_pj_thread_registered("sipgw_async");
        task();
    });
    worker.detach();
}

}

