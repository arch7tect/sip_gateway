#include "sip_gateway/audio/player.hpp"

#include <filesystem>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/utils/async.hpp"

namespace sip_gateway {
namespace audio {

namespace {

class AudioMediaPlayer : public pj::AudioMediaPlayer {
public:
    explicit AudioMediaPlayer(SmartPlayer& owner) : owner_(owner) {}

    void onEof2() override {
        utils::run_async([this]() { owner_.handle_eof(); });
    }

private:
    SmartPlayer& owner_;
};

}

SmartPlayer::SmartPlayer(
    pj::AudioMedia& audio_media,
    pj::AudioMediaRecorder* wav_recorder,
    std::function<void()> on_stop_callback
)
    : on_stop_callback_(std::move(on_stop_callback)),
      logger_(logging::get_logger()),
      active_(false)
      ,
      audio_media_(audio_media),
      wav_recorder_(wav_recorder)
{
}

void SmartPlayer::enqueue(const std::filesystem::path& filename, bool discard_after) {
    queue_.push_back({filename, discard_after});
}

void SmartPlayer::play() {
    if (!current_audio_ && !queue_.empty()) {
        play_next();
    }
}

void SmartPlayer::interrupt() {
    tearing_down_ = true;
    destroy_player();
    discard_current();
    while (!queue_.empty()) {
        auto next = queue_.front();
        queue_.pop_front();
        if (next.discard_after) {
            std::error_code ec;
            std::filesystem::remove(next.filename, ec);
        }
    }
    tearing_down_ = false;
    active_ = false;
}

bool SmartPlayer::is_active() const {
    return active_;
}

void SmartPlayer::play_next() {
    if (queue_.empty()) {
        active_ = false;
        if (on_stop_callback_) {
            on_stop_callback_();
        }
        return;
    }

    current_audio_ = queue_.front();
    queue_.pop_front();

    if (tearing_down_) {
        logger_->debug("Skip play_next during teardown.");
        return;
    }

    current_player_ = std::make_unique<AudioMediaPlayer>(*this);
    try {
        current_player_->createPlayer(current_audio_->filename.string(), PJMEDIA_FILE_NO_LOOP);
        if (wav_recorder_) {
            current_player_->startTransmit(*wav_recorder_);
        }
        current_player_->startTransmit(audio_media_);
        active_ = true;
    } catch (const pj::Error&) {
        current_player_.reset();
        active_ = false;
        discard_current();
        if (!queue_.empty()) {
            play_next();
        }
    }
}

void SmartPlayer::handle_eof() {
    destroy_player();
    discard_current();
    if (!queue_.empty() && !tearing_down_) {
        play_next();
    } else if (on_stop_callback_ && !tearing_down_) {
        on_stop_callback_();
    }
}

void SmartPlayer::destroy_player() {
    if (!current_player_) {
        return;
    }
    if (wav_recorder_) {
        try {
            current_player_->stopTransmit(*wav_recorder_);
        } catch (const pj::Error&) {
        }
    }
    try {
        current_player_->stopTransmit(audio_media_);
    } catch (const pj::Error&) {
    }
    current_player_.reset();
}

void SmartPlayer::discard_current() {
    if (!current_audio_) {
        return;
    }
    if (current_audio_->discard_after) {
        std::error_code ec;
        std::filesystem::remove(current_audio_->filename, ec);
    }
    current_audio_.reset();
}

}
}
