#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <vector>

#include <pjsua2.hpp>
namespace sip_gateway {
namespace audio {

class AudioMediaPort : public pj::AudioMediaPort {
public:
    using FrameHandler = std::function<void(const std::vector<int16_t>&)>;
    using FrameProvider = std::function<std::vector<int16_t>()>;

    AudioMediaPort();
    ~AudioMediaPort() override;

    void set_on_frame_received(FrameHandler handler);
    void set_on_frame_requested(FrameProvider handler);

    void onFrameRequested(pj::MediaFrame& frame) override;
    void onFrameReceived(pj::MediaFrame& frame) override;

private:
    struct FrameTask {
        FrameHandler handler;
        std::vector<int16_t> data;
    };

    void worker_loop();

    static constexpr size_t kMaxQueueSize = 64;

    FrameHandler on_frame_received_;
    FrameProvider on_frame_requested_;
    std::mutex handler_mutex_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<FrameTask> frame_queue_;
    std::thread worker_;
    bool stop_worker_{false};
};

}
}
