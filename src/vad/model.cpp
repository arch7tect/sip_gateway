#include "sip_gateway/vad/model.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#ifdef SIPGATEWAY_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace sip_gateway {
namespace vad {

#ifdef SIPGATEWAY_HAS_ONNX

namespace {

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "sip_gateway_vad");
    return env;
}

std::vector<std::string> get_input_names(const Ort::Session& session) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = session.GetInputCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        names.emplace_back(name ? name.get() : "");
    }
    return names;
}

std::vector<std::string> get_output_names(const Ort::Session& session) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = session.GetOutputCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto name = session.GetOutputNameAllocated(i, allocator);
        names.emplace_back(name ? name.get() : "");
    }
    return names;
}

bool has_name(const std::vector<std::string>& names, const std::string& needle) {
    return std::find(names.begin(), names.end(), needle) != names.end();
}

}

struct VadModel::Impl {
    Ort::Session session;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    int sampling_rate;
    bool has_sr;
    bool has_state;
    bool has_state_out;

    Impl(const std::filesystem::path& model_path, int sampling_rate_in)
        : session(ort_env(), model_path.string().c_str(), Ort::SessionOptions{}),
          input_names(get_input_names(session)),
          output_names(get_output_names(session)),
          sampling_rate(sampling_rate_in),
          has_sr(has_name(input_names, "sr")),
          has_state(has_name(input_names, "state")),
          has_state_out(has_name(output_names, "stateN")) {
        if (!has_name(input_names, "input")) {
            throw std::runtime_error("VAD model missing input node 'input'");
        }
        if (!has_name(output_names, "output")) {
            throw std::runtime_error("VAD model missing output node 'output'");
        }
    }
};

VadModel::VadModel(const std::filesystem::path& model_path, int sampling_rate)
    : impl_(std::make_unique<Impl>(model_path, sampling_rate)) {}

VadModel::~VadModel() = default;

int VadModel::sampling_rate() const {
    return impl_->sampling_rate;
}

std::vector<float> VadModel::initialize_state() const {
    if (!impl_->has_state) {
        return {};
    }
    return std::vector<float>(2 * 1 * 128, 0.0f);
}

float VadModel::get_speech_prob(const std::vector<float>& audio,
                                std::vector<float>* state) const {
    if (audio.empty()) {
        return 0.0f;
    }

    Ort::AllocatorWithDefaultOptions allocator;
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<int64_t> input_shape{1, static_cast<int64_t>(audio.size())};
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        mem_info, const_cast<float*>(audio.data()),
        audio.size(), input_shape.data(), input_shape.size());

    std::vector<Ort::Value> inputs;
    std::vector<const char*> input_names;
    inputs.reserve(3);
    input_names.reserve(3);
    input_names.push_back("input");
    inputs.emplace_back(std::move(input_tensor));

    std::vector<int64_t> sr_shape{1};
    std::array<int64_t, 1> sr_value{impl_->sampling_rate};
    Ort::Value sr_tensor(nullptr);
    if (impl_->has_sr) {
        sr_tensor = Ort::Value::CreateTensor<int64_t>(
            mem_info, sr_value.data(), sr_value.size(),
            sr_shape.data(), sr_shape.size());
        input_names.push_back("sr");
        inputs.emplace_back(std::move(sr_tensor));
    }

    std::vector<int64_t> state_shape{2, 1, 128};
    std::vector<float> local_state;
    if (impl_->has_state) {
        if (state && !state->empty()) {
            local_state = *state;
        } else {
            local_state = initialize_state();
        }
        Ort::Value state_tensor = Ort::Value::CreateTensor<float>(
            mem_info, local_state.data(), local_state.size(),
            state_shape.data(), state_shape.size());
        input_names.push_back("state");
        inputs.emplace_back(std::move(state_tensor));
    }

    std::vector<const char*> output_names;
    output_names.reserve(2);
    output_names.push_back("output");
    if (impl_->has_state_out) {
        output_names.push_back("stateN");
    }

    auto outputs = impl_->session.Run(
        Ort::RunOptions{nullptr},
        input_names.data(), inputs.data(), inputs.size(),
        output_names.data(), output_names.size());

    float prob = 0.0f;
    if (!outputs.empty() && outputs[0].IsTensor()) {
        auto* data = outputs[0].GetTensorMutableData<float>();
        prob = data ? data[0] : 0.0f;
    }

    if (impl_->has_state_out && outputs.size() > 1 && outputs[1].IsTensor()) {
        auto* data = outputs[1].GetTensorMutableData<float>();
        const auto count = outputs[1].GetTensorTypeAndShapeInfo().GetElementCount();
        if (state && data && count > 0) {
            state->assign(data, data + count);
        }
    }

    return prob;
}

#else

struct VadModel::Impl {
    explicit Impl(int sampling_rate_in) : sampling_rate(sampling_rate_in) {}
    int sampling_rate;
};

VadModel::VadModel(const std::filesystem::path&, int sampling_rate)
    : impl_(std::make_unique<Impl>(sampling_rate)) {
    throw std::runtime_error("ONNX Runtime not enabled");
}

VadModel::~VadModel() = default;

int VadModel::sampling_rate() const {
    return impl_->sampling_rate;
}

std::vector<float> VadModel::initialize_state() const {
    return {};
}

float VadModel::get_speech_prob(const std::vector<float>&,
                                std::vector<float>*) const {
    return 0.0f;
}

#endif

}
}
