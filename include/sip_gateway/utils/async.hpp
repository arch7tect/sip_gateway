#pragma once

#include <functional>

namespace sip_gateway {
namespace utils {

void run_async(std::function<void()> task);
void ensure_pj_thread_registered(const char* name);

}
}
