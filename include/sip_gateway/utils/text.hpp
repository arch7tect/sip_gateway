#pragma once

#include <string>

namespace sip_gateway::utils {

std::string remove_emojis(const std::string& text);
std::string normalize_text(const std::string& text);

}
