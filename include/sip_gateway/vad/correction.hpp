#pragma once

#include <deque>
#include <utility>
#include <vector>

namespace sip_gateway {
namespace vad {

struct VADCorrectionConfig {
    int score_window = 5;
    int prob_window = 15;

    double enter_thres = 0.40;
    double exit_thres = 0.25;

    double early_enter_thres = 0.30;
    int early_phase_frames = 200;
    double early_prob_boost = 0.20;

    double w_prob = 0.60;
    double w_snr = 0.15;
    double w_var = 0.05;
    double w_energy = 0.20;

    double speech_prob_threshold = 0.3;
    int min_speech_frames = 3;
    double transition_threshold = 0.4;

    std::pair<double, double> snr_clip = {0.0, 20.0};
    std::pair<double, double> var_clip = {0.0, 0.05};

    double noise_alpha = 0.02;
    double peak_decay = 0.05;

    double initial_noise_alpha = 0.15;
    int initial_adapt_frames = 50;

    bool debug = false;
};

class DynamicCorrection {
public:
    explicit DynamicCorrection(VADCorrectionConfig cfg = {});

    void start_early_detection();
    bool process_frame(double speech_prob, double frame_energy);

private:
    double clip_norm(double value, double low, double high) const;
    void update_energy_profile(double energy, double speech_prob);
    bool is_transition_period() const;
    std::pair<double, double> calculate_foreground_variance() const;
    double apply_early_detection_boost(double speech_prob) const;
    double get_dynamic_threshold() const;

    VADCorrectionConfig cfg_;
    std::deque<double> score_buf_;
    std::deque<double> prob_buf_;
    double noise_energy_ = 0.01;
    double peak_energy_ = 0.1;
    std::vector<double> initial_energy_samples_;
    bool state_ = false;
    int frame_index_ = 0;
    bool in_early_phase_ = false;
    int early_phase_start_frame_ = -1;
};

}
}
