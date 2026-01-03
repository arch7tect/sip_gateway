#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <pjsua2.hpp>
#include "spdlog/logger.h"

namespace sip_gateway {
namespace audio {

class SmartPlayer {
public:
    struct AudioFile {
        std::filesystem::path filename;
        bool discard_after = false;
    };

    explicit SmartPlayer(
        pj::AudioMedia& audio_media,
        pj::AudioMediaRecorder* wav_recorder,
        std::function<void()> on_stop_callback = nullptr
    );

    void enqueue(const std::filesystem::path& filename, bool discard_after = false);
    void play();
    void interrupt();
    bool is_active() const;
    void handle_eof();

private:
    std::deque<AudioFile> queue_;
    std::function<void()> on_stop_callback_;
    std::shared_ptr<spdlog::logger> logger_;
    bool active_ = false;
    bool tearing_down_ = false;
    std::optional<AudioFile> current_audio_;
    pj::AudioMedia& audio_media_;
    pj::AudioMediaRecorder* wav_recorder_;
    std::unique_ptr<pj::AudioMediaPlayer> current_player_;

    void play_next();
    void destroy_player();
    void discard_current();
};

}
}
