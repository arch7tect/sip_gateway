#include "sip_gateway/utils/async.hpp"

#include <thread>

namespace sip_gateway {
namespace utils {

void run_async(std::function<void()> task) {
    std::thread worker(std::move(task));
    worker.detach();
}

}
}
