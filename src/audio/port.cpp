#include "sip_gateway/audio/port.hpp"

#include <algorithm>

#include "sip_gateway/utils/async.hpp"


namespace sip_gateway::audio {

AudioMediaPort::AudioMediaPort() {
    worker_ = std::thread([this]() { worker_loop(); });
}

AudioMediaPort::~AudioMediaPort() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_worker_ = true;
    }
    queue_cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AudioMediaPort::set_on_frame_received(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_frame_received_ = std::move(handler);
}

void AudioMediaPort::set_on_frame_requested(FrameProvider handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_frame_requested_ = std::move(handler);
}

void AudioMediaPort::onFrameRequested(pj::MediaFrame& frame) {
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    FrameProvider provider;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        provider = on_frame_requested_;
    }
    if (!provider) {
        frame.size = 0;
        frame.buf.clear();
        return;
    }
    const auto data = provider();
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
    FrameHandler handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = on_frame_received_;
    }
    if (frame.buf.empty() || frame.size == 0 || !handler) {
        return;
    }
    const auto available_bytes = std::min(static_cast<size_t>(frame.size), frame.buf.size());
    const auto samples = available_bytes / sizeof(int16_t);
    std::vector<int16_t> audio_data(samples);
    std::memcpy(audio_data.data(), frame.buf.data(), samples * sizeof(int16_t));
    FrameTask task{std::move(handler), std::move(audio_data)};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (frame_queue_.size() >= kMaxQueueSize) {
            frame_queue_.pop_front();
        }
        frame_queue_.push_back(std::move(task));
    }
    queue_cv_.notify_one();
}

void AudioMediaPort::worker_loop() {
    utils::ensure_pj_thread_registered("sipgw_audio");
    while (true) {
        FrameTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stop_worker_ || !frame_queue_.empty(); });
            if (stop_worker_ && frame_queue_.empty()) {
                break;
            }
            task = std::move(frame_queue_.front());
            frame_queue_.pop_front();
        }
        if (task.handler) {
            task.handler(task.data);
        }
    }
}

}

