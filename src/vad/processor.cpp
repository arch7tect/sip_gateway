#include "sip_gateway/vad/processor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "sip_gateway/vad/model.hpp"

namespace sip_gateway {
namespace vad {

StreamingVadProcessor::StreamingVadProcessor(std::shared_ptr<VadModel> model,
                                             float threshold,
                                             int min_speech_duration_ms,
                                             int min_silence_duration_ms,
                                             int speech_pad_ms,
                                             int short_pause_ms,
                                             int long_pause_ms,
                                             int user_silence_duration_ms,
                                             int speech_prob_window)
    : model_(std::move(model)),
      threshold_(threshold),
      sampling_rate_(model_ ? model_->sampling_rate() : 16000),
      speech_prob_window_(std::max(1, speech_prob_window)) {
    min_speech_samples_ = sampling_rate_ * min_speech_duration_ms / 1000;
    min_silence_samples_ = sampling_rate_ * min_silence_duration_ms / 1000;
    speech_pad_samples_ = sampling_rate_ * speech_pad_ms / 1000;
    short_pause_samples_ = min_silence_samples_ + sampling_rate_ * short_pause_ms / 1000;
    long_pause_samples_ = short_pause_samples_ + sampling_rate_ * long_pause_ms / 1000;
    user_silence_samples_ = sampling_rate_ * user_silence_duration_ms / 1000;
    const int max_silence_ms = std::max(speech_pad_ms * 2, min_silence_duration_ms);
    max_silence_samples_ = sampling_rate_ * max_silence_ms / 1000;
    if (model_) {
        state_ = model_->initialize_state();
    }
}

void StreamingVadProcessor::set_on_speech_start(SpeechCallback cb) {
    on_speech_start_ = std::move(cb);
}

void StreamingVadProcessor::set_on_speech_end(SpeechCallback cb) {
    on_speech_end_ = std::move(cb);
}

void StreamingVadProcessor::set_on_short_pause(SpeechCallback cb) {
    on_short_pause_ = std::move(cb);
}

void StreamingVadProcessor::set_on_long_pause(SpeechCallback cb) {
    on_long_pause_ = std::move(cb);
}

void StreamingVadProcessor::set_on_user_silence_timeout(SilenceCallback cb) {
    on_user_silence_timeout_ = std::move(cb);
}

void StreamingVadProcessor::process_samples(const std::vector<int16_t>& samples) {
    if (!model_ || samples.empty()) {
        return;
    }
    std::vector<float> audio;
    audio.reserve(samples.size());
    for (auto sample : samples) {
        audio.push_back(static_cast<float>(sample) / 32768.0f);
    }
    buffer_.insert(buffer_.end(), audio.begin(), audio.end());
    while (buffer_.size() >= static_cast<size_t>(window_size_samples_)) {
        std::vector<float> window(buffer_.begin(),
                                  buffer_.begin() + window_size_samples_);
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + window_size_samples_);
        process_window(window);
    }
}

void StreamingVadProcessor::finalize() {
    if (speech_buffer_.size() >= static_cast<size_t>(min_speech_samples_)) {
        fire_long_pause();
    }
}

float StreamingVadProcessor::get_smoothed_prob(const std::vector<float>& window) {
    std::vector<float> normalized = window;
    float max_amp = 0.0f;
    for (auto val : normalized) {
        max_amp = std::max(max_amp, std::abs(val));
    }
    if (max_amp > 1.0f || max_amp < 0.01f) {
        if (max_amp > 0.0f) {
            for (auto& val : normalized) {
                val /= max_amp;
            }
        }
    }
    float prob = model_->get_speech_prob(normalized, &state_);
    prob_history_.push_back(prob);
    if (prob_history_.size() > static_cast<size_t>(speech_prob_window_)) {
        prob_history_.pop_front();
    }
    if (prob_history_.size() <= 1) {
        return prob;
    }
    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    int weight = 1;
    for (const auto val : prob_history_) {
        weighted_sum += val * weight;
        weight_total += static_cast<float>(weight);
        ++weight;
    }
    return weighted_sum / weight_total;
}

void StreamingVadProcessor::process_window(const std::vector<float>& window) {
    const float speech_prob = get_smoothed_prob(window);
    const bool is_speech_frame = speech_prob > threshold_;
    current_sample_ += static_cast<int64_t>(window.size());

    if (active_long_speech_) {
        speech_buffer_.insert(speech_buffer_.end(), window.begin(), window.end());
        if (is_speech_frame) {
            if (!silence_buffer_.empty()) {
                silence_buffer_.clear();
            }
        } else {
            grow_silence_buffer(window);
        }
    } else {
        if (is_speech_frame) {
            speech_buffer_.insert(speech_buffer_.end(), window.begin(), window.end());
        } else {
            if (!speech_buffer_.empty()) {
                grow_silence_buffer(speech_buffer_);
                speech_buffer_.clear();
            }
            grow_silence_buffer(window);
        }
    }

    if (is_speech_frame) {
        if (!active_speech_) {
            speech_start_ = current_sample_ - static_cast<int64_t>(window.size());
            if (speech_buffer_.size() >= static_cast<size_t>(min_speech_samples_)) {
                fire_speech_start();
            }
        }
    } else {
        if (active_speech_) {
            if (silence_buffer_.size() >= static_cast<size_t>(min_silence_samples_)) {
                fire_speech_end();
            }
        } else if (!user_silence_timeout_fired_ &&
                   (current_sample_ - user_silence_start_ > user_silence_samples_)) {
            fire_user_silence_timeout();
        }
        if (active_long_speech_) {
            if (!short_pause_fired_ &&
                silence_buffer_.size() >= static_cast<size_t>(short_pause_samples_)) {
                fire_short_pause();
            }
            if (!long_pause_suspended_ &&
                silence_buffer_.size() >= static_cast<size_t>(long_pause_samples_)) {
                fire_long_pause();
            }
        }
    }
}

void StreamingVadProcessor::grow_silence_buffer(const std::vector<float>& window) {
    silence_buffer_.insert(silence_buffer_.end(), window.begin(), window.end());
    if (silence_buffer_.size() > static_cast<size_t>(max_silence_samples_)) {
        silence_buffer_.erase(
            silence_buffer_.begin(),
            silence_buffer_.begin() + (silence_buffer_.size() - max_silence_samples_));
    }
}

void StreamingVadProcessor::fire_speech_start() {
    active_speech_ = true;
    if (!active_long_speech_) {
        active_long_speech_ = true;
        const size_t start_padding = std::min(
            static_cast<size_t>(speech_pad_samples_), silence_buffer_.size());
        silence_pad_buffer_ = apply_fade(
            std::vector<float>(silence_buffer_.end() - start_padding,
                               silence_buffer_.end()),
            true);
    }
    silence_buffer_.clear();
    if (on_speech_start_) {
        double start = 0.0;
        double duration = 0.0;
        times_sec(silence_pad_buffer_, start, duration);
        on_speech_start_(silence_pad_buffer_, start, duration);
    }
}

void StreamingVadProcessor::fire_speech_end() {
    active_speech_ = false;
    if (!active_long_speech_) {
        speech_buffer_.clear();
    }
    short_pause_fired_ = false;
    user_silence_start_ = current_sample_ - static_cast<int64_t>(silence_buffer_.size());
    user_silence_timeout_fired_ = false;
    const int64_t start_offset = speech_start_ - current_sample_;
    const int64_t end_offset = -static_cast<int64_t>(silence_buffer_.size());
    const int64_t start_index =
        std::max<int64_t>(0, static_cast<int64_t>(speech_buffer_.size()) + start_offset);
    const int64_t end_index =
        std::max<int64_t>(0, static_cast<int64_t>(speech_buffer_.size()) + end_offset);
    std::vector<float> buffer;
    if (end_index > start_index) {
        buffer.assign(speech_buffer_.begin() + start_index,
                      speech_buffer_.begin() + end_index);
    }
    if (on_speech_end_) {
        double start = 0.0;
        double duration = 0.0;
        times_sec(buffer, start, duration);
        on_speech_end_(buffer, start, duration);
    }
}

void StreamingVadProcessor::fire_short_pause() {
    const size_t silence_length = silence_buffer_.size();
    auto silence_postfix = apply_fade(silence_buffer_, false);
    std::vector<float> buffer = silence_pad_buffer_;
    if (speech_buffer_.size() > silence_length) {
        buffer.insert(buffer.end(),
                      speech_buffer_.begin(),
                      speech_buffer_.end() - silence_length);
    }
    buffer.insert(buffer.end(), silence_postfix.begin(), silence_postfix.end());
    if (on_short_pause_) {
        double start = 0.0;
        double duration = 0.0;
        times_sec(buffer, start, duration);
        on_short_pause_(buffer, start, duration);
    }
    short_pause_fired_ = true;
}

void StreamingVadProcessor::fire_long_pause() {
    const size_t silence_length = silence_buffer_.size();
    auto silence_postfix = apply_fade(silence_buffer_, false);
    std::vector<float> buffer = silence_pad_buffer_;
    if (speech_buffer_.size() > silence_length) {
        buffer.insert(buffer.end(),
                      speech_buffer_.begin(),
                      speech_buffer_.end() - silence_length);
    }
    buffer.insert(buffer.end(), silence_postfix.begin(), silence_postfix.end());
    if (on_long_pause_) {
        double start = 0.0;
        double duration = 0.0;
        times_sec(buffer, start, duration);
        on_long_pause_(buffer, start, duration);
    }
    short_pause_fired_ = false;
    active_long_speech_ = false;
    speech_buffer_.clear();
}

void StreamingVadProcessor::fire_user_silence_timeout() {
    if (on_user_silence_timeout_) {
        on_user_silence_timeout_(current_time_sec());
    }
    user_silence_timeout_fired_ = true;
}

std::vector<float> StreamingVadProcessor::apply_fade(const std::vector<float>& audio,
                                                     bool fade_in) const {
    if (audio.size() <= 1) {
        return audio;
    }
    std::vector<float> result = audio;
    const size_t length = audio.size();
    for (size_t i = 0; i < length; ++i) {
        const float ratio = static_cast<float>(i) / static_cast<float>(length - 1);
        float curve = std::sin(ratio * 1.57079632679f);
        if (!fade_in) {
            curve = 1.0f - curve;
        }
        result[i] *= curve;
    }
    return result;
}

double StreamingVadProcessor::current_time_sec() const {
    return static_cast<double>(current_sample_) /
           static_cast<double>(sampling_rate_);
}

void StreamingVadProcessor::times_sec(const std::vector<float>& audio,
                                      double& start,
                                      double& duration) const {
    start = static_cast<double>(current_sample_ - audio.size()) /
            static_cast<double>(sampling_rate_);
    duration = static_cast<double>(audio.size()) /
               static_cast<double>(sampling_rate_);
}

}
}
