#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace sip_gateway {
namespace vad {

class VadModel;

class StreamingVadProcessor {
public:
    using SpeechCallback = std::function<void(const std::vector<float>&, double, double)>;
    using SilenceCallback = std::function<void(double)>;

    StreamingVadProcessor(std::shared_ptr<VadModel> model,
                          float threshold,
                          int min_speech_duration_ms,
                          int min_silence_duration_ms,
                          int speech_pad_ms,
                          int short_pause_ms,
                          int long_pause_ms,
                          int user_silence_duration_ms,
                          int speech_prob_window);

    void set_on_speech_start(SpeechCallback cb);
    void set_on_speech_end(SpeechCallback cb);
    void set_on_short_pause(SpeechCallback cb);
    void set_on_long_pause(SpeechCallback cb);
    void set_on_user_silence_timeout(SilenceCallback cb);

    void process_samples(const std::vector<int16_t>& samples);
    void finalize();

private:
    void process_window(const std::vector<float>& window);
    float get_smoothed_prob(const std::vector<float>& window);
    void grow_silence_buffer(const std::vector<float>& window);
    void fire_speech_start();
    void fire_speech_end();
    void fire_short_pause();
    void fire_long_pause();
    void fire_user_silence_timeout();
    std::vector<float> apply_fade(const std::vector<float>& audio, bool fade_in) const;
    double current_time_sec() const;
    void times_sec(const std::vector<float>& audio, double& start, double& duration) const;

    std::shared_ptr<VadModel> model_;
    float threshold_;
    int window_size_samples_ = 512;
    int sampling_rate_;
    int speech_prob_window_;

    int min_speech_samples_;
    int min_silence_samples_;
    int speech_pad_samples_;
    int short_pause_samples_;
    int long_pause_samples_;
    int user_silence_samples_;
    int max_silence_samples_;

    std::vector<float> buffer_;
    std::vector<float> speech_buffer_;
    std::vector<float> silence_buffer_;
    std::vector<float> silence_pad_buffer_;
    std::deque<float> prob_history_;
    std::vector<float> state_;

    int64_t current_sample_ = 0;
    bool active_speech_ = false;
    bool active_long_speech_ = false;
    bool short_pause_fired_ = false;
    bool long_pause_suspended_ = false;
    int64_t speech_start_ = 0;
    int64_t user_silence_start_ = 0;
    bool user_silence_timeout_fired_ = false;

    SpeechCallback on_speech_start_;
    SpeechCallback on_speech_end_;
    SpeechCallback on_short_pause_;
    SpeechCallback on_long_pause_;
    SilenceCallback on_user_silence_timeout_;
};

}
}
