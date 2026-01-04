#include "sip_gateway/vad/correction.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "sip_gateway/logging.hpp"

namespace sip_gateway {
namespace vad {

namespace {

double mean_value(const std::deque<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

double population_variance(const std::deque<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double mean = mean_value(values);
    double acc = 0.0;
    for (double value : values) {
        const double diff = value - mean;
        acc += diff * diff;
    }
    return acc / static_cast<double>(values.size());
}

double population_variance(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    const double mean = sum / static_cast<double>(values.size());
    double acc = 0.0;
    for (double value : values) {
        const double diff = value - mean;
        acc += diff * diff;
    }
    return acc / static_cast<double>(values.size());
}

}

DynamicCorrection::DynamicCorrection(VADCorrectionConfig cfg)
    : cfg_(cfg),
      score_buf_(),
      prob_buf_() {
    score_buf_.clear();
    prob_buf_.clear();
}

void DynamicCorrection::start_early_detection() {
    if (early_phase_start_frame_ == -1) {
        in_early_phase_ = true;
        early_phase_start_frame_ = frame_index_;
    }
}

double DynamicCorrection::clip_norm(double value, double low, double high) const {
    if (high <= low) {
        return 0.0;
    }
    const double clipped = std::max(low, std::min(high, value));
    return (clipped - low) / (high - low);
}

void DynamicCorrection::update_energy_profile(double energy, double speech_prob) {
    if (static_cast<int>(initial_energy_samples_.size()) < cfg_.initial_adapt_frames) {
        initial_energy_samples_.push_back(energy);
        if (static_cast<int>(initial_energy_samples_.size()) == cfg_.initial_adapt_frames) {
            auto sorted = initial_energy_samples_;
            std::sort(sorted.begin(), sorted.end());
            const size_t idx = sorted.size() / 10;
            noise_energy_ = sorted[idx];
        }
    }

    const double alpha = frame_index_ < cfg_.initial_adapt_frames
                             ? cfg_.initial_noise_alpha
                             : cfg_.noise_alpha;

    if (!state_ && speech_prob < 0.3) {
        noise_energy_ = (1.0 - alpha) * noise_energy_ + alpha * energy;
    }

    if (energy > peak_energy_) {
        peak_energy_ = energy;
    } else {
        peak_energy_ = (1.0 - cfg_.peak_decay) * peak_energy_ +
                       cfg_.peak_decay * noise_energy_;
    }
    peak_energy_ = std::max(peak_energy_, noise_energy_ + 1e-6);
}

bool DynamicCorrection::is_transition_period() const {
    if (prob_buf_.size() < 4) {
        return false;
    }
    auto minmax = std::minmax_element(prob_buf_.end() - 4, prob_buf_.end());
    return (*minmax.second - *minmax.first) > cfg_.transition_threshold;
}

std::pair<double, double> DynamicCorrection::calculate_foreground_variance() const {
    if (prob_buf_.size() < 2) {
        return {0.0, 0.0};
    }
    const double raw_var = population_variance(prob_buf_);

    if (!state_) {
        return {raw_var, 0.0};
    }

    std::vector<double> speech_probs;
    speech_probs.reserve(prob_buf_.size());
    for (double prob : prob_buf_) {
        if (prob > cfg_.speech_prob_threshold) {
            speech_probs.push_back(prob);
        }
    }
    if (static_cast<int>(speech_probs.size()) < cfg_.min_speech_frames) {
        return {raw_var, 0.0};
    }

    double foreground_var = population_variance(speech_probs);
    if (is_transition_period()) {
        std::vector<double> recent;
        const int max_count = 6;
        for (int i = static_cast<int>(prob_buf_.size()) - 1; i >= 0 && static_cast<int>(recent.size()) < max_count; --i) {
            const double prob = prob_buf_[static_cast<size_t>(i)];
            if (prob > cfg_.speech_prob_threshold) {
                recent.push_back(prob);
            }
        }
        if (recent.size() >= 3) {
            foreground_var = population_variance(recent);
        } else {
            foreground_var = 0.0;
        }
    }
    return {raw_var, foreground_var};
}

double DynamicCorrection::apply_early_detection_boost(double speech_prob) const {
    if (!in_early_phase_) {
        return speech_prob;
    }
    return std::min(1.0, speech_prob + cfg_.early_prob_boost);
}

double DynamicCorrection::get_dynamic_threshold() const {
    return in_early_phase_ ? cfg_.early_enter_thres : cfg_.enter_thres;
}

bool DynamicCorrection::process_frame(double speech_prob, double frame_energy) {
    update_energy_profile(frame_energy, speech_prob);

    const double adjusted_prob = apply_early_detection_boost(speech_prob);
    const double snr = frame_energy / (noise_energy_ + 1e-6);
    const double snr_n = clip_norm(snr, cfg_.snr_clip.first, cfg_.snr_clip.second);

    prob_buf_.push_back(adjusted_prob);
    if (prob_buf_.size() > static_cast<size_t>(cfg_.prob_window)) {
        prob_buf_.pop_front();
    }

    const auto vars = calculate_foreground_variance();
    const double fg_var = vars.second;
    const double fg_var_n = clip_norm(fg_var, cfg_.var_clip.first, cfg_.var_clip.second);

    double eng_n = 0.0;
    if (peak_energy_ > noise_energy_) {
        eng_n = (frame_energy - noise_energy_) /
                (peak_energy_ - noise_energy_ + 1e-6);
    } else {
        eng_n = frame_energy > noise_energy_ ? 0.5 : 0.0;
    }
    eng_n = std::min(1.0, std::max(0.0, eng_n));

    const double weight_sum = cfg_.w_prob + cfg_.w_snr + cfg_.w_var + cfg_.w_energy;
    double score = cfg_.w_prob * adjusted_prob +
                   cfg_.w_snr * snr_n +
                   cfg_.w_var * fg_var_n +
                   cfg_.w_energy * eng_n;
    score /= weight_sum > 0.0 ? weight_sum : 1.0;

    score_buf_.push_back(score);
    if (score_buf_.size() > static_cast<size_t>(cfg_.score_window)) {
        score_buf_.pop_front();
    }

    const double mean_score = mean_value(score_buf_);
    const double enter_thres = get_dynamic_threshold();
    if (!state_ && mean_score >= enter_thres) {
        state_ = true;
    } else if (state_ && mean_score <= cfg_.exit_thres) {
        state_ = false;
    }

    if (in_early_phase_) {
        if (state_) {
            in_early_phase_ = false;
        } else if (early_phase_start_frame_ >= 0 &&
                   frame_index_ >= early_phase_start_frame_ + cfg_.early_phase_frames) {
            in_early_phase_ = false;
        }
    }

    if (cfg_.debug) {
        logging::get_logger()->debug(with_kv(
            "VAD correction frame",
            {kv("frame", frame_index_),
             kv("prob", speech_prob),
             kv("score", mean_score),
             kv("state", state_ ? "SPEECH" : "SILENCE")}));
    }

    ++frame_index_;
    return state_;
}

}
}
