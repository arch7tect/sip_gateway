#include "sip_gateway/utils/async.hpp"

#include <pj/os.h>
#include <thread>

namespace sip_gateway {
namespace utils {

void run_async(std::function<void()> task) {
    std::thread worker([task = std::move(task)]() mutable {
        if (!pj_thread_is_registered()) {
            thread_local pj_thread_desc desc;
            pj_thread_t* thread = nullptr;
            pj_thread_register("sipgw_async", desc, &thread);
        }
        task();
    });
    worker.detach();
}

}
}
