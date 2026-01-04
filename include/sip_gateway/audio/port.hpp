#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <pjsua2.hpp>
namespace sip_gateway {
namespace audio {

class AudioMediaPort : public pj::AudioMediaPort {
public:
    using FrameHandler = std::function<void(const std::vector<int16_t>&)>;
    using FrameProvider = std::function<std::vector<int16_t>()>;

    AudioMediaPort();

    void set_on_frame_received(FrameHandler handler);
    void set_on_frame_requested(FrameProvider handler);

    void onFrameRequested(pj::MediaFrame& frame) override;
    void onFrameReceived(pj::MediaFrame& frame) override;

private:
    FrameHandler on_frame_received_;
    FrameProvider on_frame_requested_;
};

}
}
