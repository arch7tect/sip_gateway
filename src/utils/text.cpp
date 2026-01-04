#include "sip_gateway/utils/text.hpp"

#include <cstdint>
#include <cctype>

namespace sip_gateway::utils {

namespace {

bool is_emoji_codepoint(uint32_t codepoint) {
    return (codepoint >= 0x1F600 && codepoint <= 0x1F64F) ||
           (codepoint >= 0x1F300 && codepoint <= 0x1F5FF) ||
           (codepoint >= 0x1F680 && codepoint <= 0x1F6FF) ||
           (codepoint >= 0x1F700 && codepoint <= 0x1F77F) ||
           (codepoint >= 0x1F780 && codepoint <= 0x1F7FF) ||
           (codepoint >= 0x1F800 && codepoint <= 0x1F8FF) ||
           (codepoint >= 0x1F900 && codepoint <= 0x1F9FF) ||
           (codepoint >= 0x1FA00 && codepoint <= 0x1FA6F) ||
           (codepoint >= 0x1FA70 && codepoint <= 0x1FAFF) ||
           (codepoint >= 0x2702 && codepoint <= 0x27B0) ||
           (codepoint >= 0x24C2 && codepoint <= 0x1F251);
}

bool decode_utf8(const std::string& text, size_t index, uint32_t& codepoint, size_t& length) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80) {
        codepoint = byte;
        length = 1;
        return true;
    }
    if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        if ((b1 & 0xC0) != 0x80) {
            return false;
        }
        codepoint = ((byte & 0x1F) << 6) | (b1 & 0x3F);
        length = 2;
        return codepoint >= 0x80;
    }
    if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        const auto b2 = static_cast<unsigned char>(text[index + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
            return false;
        }
        codepoint = ((byte & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        length = 3;
        return codepoint >= 0x800;
    }
    if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[index + 1]);
        const auto b2 = static_cast<unsigned char>(text[index + 2]);
        const auto b3 = static_cast<unsigned char>(text[index + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
            return false;
        }
        codepoint = ((byte & 0x07) << 18) |
                    ((b1 & 0x3F) << 12) |
                    ((b2 & 0x3F) << 6) |
                    (b3 & 0x3F);
        length = 4;
        return codepoint >= 0x10000 && codepoint <= 0x10FFFF;
    }
    return false;
}

}

std::string remove_emojis(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        uint32_t codepoint = 0;
        size_t length = 1;
        if (!decode_utf8(text, i, codepoint, length)) {
            result.push_back(text[i]);
            ++i;
            continue;
        }
        if (!is_emoji_codepoint(codepoint)) {
            result.append(text, i, length);
        }
        i += length;
    }
    return result;
}

std::string normalize_text(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool in_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!in_space) {
                normalized.push_back(' ');
                in_space = true;
            }
        } else {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
            in_space = false;
        }
    }
    if (!normalized.empty() && normalized.front() == ' ') {
        normalized.erase(normalized.begin());
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

}
