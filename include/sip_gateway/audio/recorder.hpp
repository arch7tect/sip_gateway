#pragma once

#include <filesystem>
#include <memory>

#include <pjsua2.hpp>
namespace sip_gateway {
namespace audio {

class CallRecorder {
public:
    CallRecorder();
    ~CallRecorder();

    void start_recording(
        const std::filesystem::path& filename,
        unsigned sample_rate = 16000,
        unsigned channels = 1,
        unsigned bits_per_sample = 16
    );
    void stop_recording();
    bool is_recording() const;

    pj::AudioMediaRecorder* get_recorder();

private:
    std::unique_ptr<pj::AudioMediaRecorder> recorder_;
    std::filesystem::path current_file_;
    bool recording_ = false;
};

}
}
