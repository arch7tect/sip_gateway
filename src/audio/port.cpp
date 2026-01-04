#include "sip_gateway/audio/port.hpp"

#include <algorithm>
#include <cstring>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/utils/async.hpp"

namespace sip_gateway {
namespace audio {

AudioMediaPort::AudioMediaPort() = default;

void AudioMediaPort::set_on_frame_received(FrameHandler handler) {
    on_frame_received_ = std::move(handler);
}

void AudioMediaPort::set_on_frame_requested(FrameProvider handler) {
    on_frame_requested_ = std::move(handler);
}

void AudioMediaPort::onFrameRequested(pj::MediaFrame& frame) {
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    if (!on_frame_requested_) {
        frame.size = 0;
        frame.buf.clear();
        return;
    }
    const auto data = on_frame_requested_();
    if (data.empty() || frame.size == 0) {
        frame.size = 0;
        frame.buf.clear();
        return;
    }
    const auto max_samples = static_cast<size_t>(frame.size / sizeof(int16_t));
    const auto copy_samples = std::min(max_samples, data.size());
    const auto copy_bytes = copy_samples * sizeof(int16_t);
    frame.buf.resize(copy_bytes);
    std::memcpy(frame.buf.data(), data.data(), copy_bytes);
    frame.size = static_cast<unsigned>(copy_bytes);
}

void AudioMediaPort::onFrameReceived(pj::MediaFrame& frame) {
    if (frame.buf.empty() || frame.size == 0 || !on_frame_received_) {
        return;
    }
    const auto available_bytes = std::min(static_cast<size_t>(frame.size), frame.buf.size());
    const auto samples = available_bytes / sizeof(int16_t);
    std::vector<int16_t> audio_data(samples);
    std::memcpy(audio_data.data(), frame.buf.data(), samples * sizeof(int16_t));
    utils::run_async([handler = on_frame_received_, audio_data = std::move(audio_data)]() {
        handler(audio_data);
    });
}

}
}
