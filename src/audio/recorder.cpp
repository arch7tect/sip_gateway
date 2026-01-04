#include "sip_gateway/audio/recorder.hpp"

#include <filesystem>
#include <stdexcept>

#include "sip_gateway/logging.hpp"

namespace sip_gateway {
namespace audio {

CallRecorder::CallRecorder() = default;

CallRecorder::~CallRecorder() {
    stop_recording();
}

void CallRecorder::start_recording(
    const std::filesystem::path& filename,
    unsigned sample_rate,
    unsigned channels,
    unsigned bits_per_sample
) {
    (void)sample_rate;
    (void)channels;
    (void)bits_per_sample;

    if (recording_) {
        stop_recording();
    }

    current_file_ = filename;
    const auto parent_dir = current_file_.parent_path();
    if (!parent_dir.empty()) {
        std::filesystem::create_directories(parent_dir);
    }

    recorder_ = std::make_unique<pj::AudioMediaRecorder>();

    try {
        recorder_->createRecorder(filename.string());
        recording_ = true;
    } catch (const pj::Error& e) {
        recorder_.reset();
        recording_ = false;
        throw std::runtime_error(e.info());
    }
}

void CallRecorder::stop_recording() {
    recorder_.reset();
    recording_ = false;
    current_file_.clear();
}

bool CallRecorder::is_recording() const {
    return recording_;
}

pj::AudioMediaRecorder* CallRecorder::get_recorder() {
    return recorder_.get();
}

}
}
