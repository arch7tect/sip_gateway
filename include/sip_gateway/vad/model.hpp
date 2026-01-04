#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sip_gateway {
namespace vad {

class VadModel {
public:
    VadModel(const std::filesystem::path& model_path, int sampling_rate);
    ~VadModel();

    int sampling_rate() const;
    std::vector<float> initialize_state() const;
    float get_speech_prob(const std::vector<float>& audio,
                          std::vector<float>* state) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
