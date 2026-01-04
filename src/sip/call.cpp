#include "sip_gateway/sip/call.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/sip/app.hpp"
#include "sip_gateway/utils/async.hpp"
#include "sip_gateway/vad/model.hpp"

namespace sip_gateway {

SipCall::SipCall(SipApp& app, pj::Account& account, std::string backend_url, int call_id)
    : pj::Call(account, call_id),
      app_(app),
      ws_client_(std::move(backend_url)) {}

SipCall::~SipCall() {
    close_media();
    stop_ws();
}

void SipCall::set_session_id(const std::string& session_id) {
    session_id_ = session_id;
}

const std::optional<std::string>& SipCall::session_id() const {
    return session_id_;
}

void SipCall::make_call(const std::string& to_uri) {
    to_uri_ = to_uri;
    pj::CallOpParam prm(true);
    pj::Call::makeCall(to_uri, prm);
}

void SipCall::answer(int status_code) {
    pj::CallOpParam prm(true);
    prm.statusCode = static_cast<pjsip_status_code>(status_code);
    pj::Call::answer(prm);
}

void SipCall::hangup(int status_code) {
    pj::CallOpParam prm(true);
    prm.statusCode = static_cast<pjsip_status_code>(status_code);
    pj::Call::hangup(prm);
}

void SipCall::connect_ws(BackendWsClient::MessageHandler on_message,
                         BackendWsClient::EventHandler on_timeout,
                         BackendWsClient::EventHandler on_close) {
    if (!session_id_) {
        logging::get_logger()->warn("WebSocket connect skipped: session_id is not set");
        return;
    }
    ws_client_.connect(*session_id_, std::move(on_message), std::move(on_timeout),
                       std::move(on_close));
}

void SipCall::stop_ws() {
    ws_client_.stop();
}

void SipCall::set_greeting(std::optional<std::string> greeting) {
    greeting_ = std::move(greeting);
}

void SipCall::handle_ws_message(const nlohmann::json& message) {
    const auto type = message.value("type", "");
    if (type == "message") {
        const auto text = message.value("message", "");
        if (!text.empty()) {
            enqueue_tts_text(text);
        }
        return;
    }
    if (type == "eos") {
        logging::get_logger()->debug(with_kv(
            "WebSocket end of stream",
            {kv("session_id", session_id_.value_or(""))}));
        return;
    }
    if (type == "eoc") {
        logging::get_logger()->debug(with_kv(
            "WebSocket end of conversation",
            {kv("session_id", session_id_.value_or(""))}));
        return;
    }
    logging::get_logger()->debug(with_kv(
        "WebSocket message received",
        {kv("message", message.dump()),
         kv("session_id", session_id_.value_or(""))}));
}

void SipCall::handle_ws_timeout() {
    logging::get_logger()->info(with_kv(
        "WebSocket timeout received",
        {kv("session_id", session_id_.value_or(""))}));
}

void SipCall::handle_ws_close() {
    logging::get_logger()->info(with_kv(
        "WebSocket close received",
        {kv("session_id", session_id_.value_or(""))}));
}

void SipCall::onCallState(pj::OnCallStateParam& prm) {
    (void)prm;
    try {
        const auto info = getInfo();
        logging::get_logger()->debug(with_kv(
            "Call state changed",
            {kv("call_id", info.callIdString),
             kv("uri", info.remoteUri),
             kv("state", static_cast<int>(info.state)),
             kv("state_text", info.stateText),
             kv("session_id", session_id_.value_or(""))}));
        if (info.state == PJSIP_INV_STATE_CONFIRMED) {
            open_media();
        }
        if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
            close_media();
            app_.handle_call_disconnected(getId());
        }
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "Call state handler exception",
            {kv("error", ex.what())}));
    }
}

void SipCall::onCallMediaState(pj::OnCallMediaStateParam& prm) {
    (void)prm;
    try {
        logging::get_logger()->debug(with_kv(
            "Call media state changed",
            {kv("session_id", session_id_.value_or(""))}));
        if (!media_active_) {
            open_media();
        }
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "Call media handler exception",
            {kv("error", ex.what())}));
    }
}

void SipCall::open_media() {
    if (media_active_) {
        return;
    }
    auto logger = logging::get_logger();
    try {
        audio_media_ = std::make_unique<pj::AudioMedia>(getAudioMedia(-1));
    } catch (const pj::Error& ex) {
        logger->error(with_kv(
            "Call media not available",
            {kv("reason", ex.reason),
             kv("status", ex.status),
             kv("session_id", session_id_.value_or(""))}));
        return;
    }

    pj::MediaFormatAudio format;
    format.type = PJMEDIA_TYPE_AUDIO;
    format.clockRate = app_.config().vad_sampling_rate;
    format.channelCount = 1;
    format.bitsPerSample = 16;
    format.frameTimeUsec = app_.config().frame_time_usec;

    media_port_ = std::make_unique<audio::AudioMediaPort>();
    media_port_->createPort("port/input/" + recording_basename(), format);
    media_port_->set_on_frame_received(
        [this](const std::vector<int16_t>& data) { handle_audio_frame(data); });

    try {
        audio_media_->startTransmit(*media_port_);
    } catch (const pj::Error& ex) {
        logger->error(with_kv(
            "Failed to attach media port",
            {kv("reason", ex.reason),
             kv("status", ex.status),
             kv("session_id", session_id_.value_or(""))}));
    }

    if (app_.config().record_audio_parts) {
        recorder_ = std::make_unique<audio::CallRecorder>();
        const auto filename = app_.config().sip_audio_dir /
                              (recording_basename() + ".wav");
        try {
            recorder_->start_recording(filename);
            if (auto* recorder_media = recorder_->get_recorder()) {
                audio_media_->startTransmit(*recorder_media);
            }
        } catch (const std::exception& ex) {
            logger->error(with_kv(
                "Failed to start call recorder",
                {kv("error", ex.what()),
                 kv("filename", filename.string()),
                 kv("session_id", session_id_.value_or(""))}));
            recorder_.reset();
        }
    }

    auto* recorder_media = recorder_ ? recorder_->get_recorder() : nullptr;
    player_ = std::make_unique<audio::SmartPlayer>(
        *audio_media_,
        recorder_media,
        [logger, session_id = session_id_.value_or("")]() {
            logger->debug(with_kv(
                "Audio playback finished",
                {kv("session_id", session_id)}));
        });
    if (!vad_processor_) {
        auto model = app_.vad_model();
        if (model) {
            vad_processor_ = std::make_unique<vad::StreamingVadProcessor>(
                model,
                static_cast<float>(app_.config().vad_threshold),
                app_.config().vad_min_speech_duration_ms,
                app_.config().vad_min_silence_duration_ms,
                app_.config().vad_speech_pad_ms,
                app_.config().short_pause_offset_ms,
                app_.config().long_pause_offset_ms,
                app_.config().user_silence_timeout_ms,
                app_.config().vad_speech_prob_window);
            vad_processor_->set_on_speech_start(
                [logger, session_id = session_id_.value_or("")](const std::vector<float>&,
                                                                double start,
                                                                double duration) {
                    logger->debug(with_kv(
                        "VAD speech start",
                        {kv("start_sec", start),
                         kv("duration_sec", duration),
                         kv("session_id", session_id)}));
                });
            vad_processor_->set_on_speech_end(
                [logger, session_id = session_id_.value_or("")](const std::vector<float>&,
                                                                double start,
                                                                double duration) {
                    logger->debug(with_kv(
                        "VAD speech end",
                        {kv("start_sec", start),
                         kv("duration_sec", duration),
                         kv("session_id", session_id)}));
                });
            vad_processor_->set_on_short_pause(
                [logger, session_id = session_id_.value_or("")](const std::vector<float>&,
                                                                double start,
                                                                double duration) {
                    logger->debug(with_kv(
                        "VAD short pause",
                        {kv("start_sec", start),
                         kv("duration_sec", duration),
                         kv("session_id", session_id)}));
                });
            vad_processor_->set_on_long_pause(
                [logger, session_id = session_id_.value_or("")](const std::vector<float>&,
                                                                double start,
                                                                double duration) {
                    logger->debug(with_kv(
                        "VAD long pause",
                        {kv("start_sec", start),
                         kv("duration_sec", duration),
                         kv("session_id", session_id)}));
                });
            vad_processor_->set_on_user_silence_timeout(
                [logger, session_id = session_id_.value_or("")](double current_time) {
                    logger->debug(with_kv(
                        "VAD user silence timeout",
                        {kv("time_sec", current_time),
                         kv("session_id", session_id)}));
                });
        }
    }
    media_active_ = true;
    if (greeting_ && !greeting_->empty()) {
        enqueue_tts_text(*greeting_, app_.config().greeting_delay_sec);
    }
    play_pending_tts();
}

void SipCall::close_media() {
    if (!media_active_) {
        return;
    }
    if (player_) {
        player_->interrupt();
    }
    if (audio_media_ && recorder_ && recorder_->get_recorder()) {
        try {
            audio_media_->stopTransmit(*recorder_->get_recorder());
        } catch (const pj::Error&) {
        }
    }
    if (audio_media_ && media_port_) {
        try {
            audio_media_->stopTransmit(*media_port_);
        } catch (const pj::Error&) {
        }
    }
    if (recorder_) {
        recorder_->stop_recording();
    }
    if (vad_processor_) {
        vad_processor_->finalize();
    }
    player_.reset();
    recorder_.reset();
    media_port_.reset();
    audio_media_.reset();
    media_active_ = false;
}

void SipCall::handle_audio_frame(const std::vector<int16_t>& data) {
    if (vad_processor_) {
        vad_processor_->process_samples(data);
    }
}

void SipCall::enqueue_tts_text(const std::string& text, double delay_sec) {
    if (text.empty()) {
        return;
    }
    if (delay_sec > 0.0) {
        utils::run_async([this, text, delay_sec]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(delay_sec));
            enqueue_tts_text(text, 0.0);
        });
        return;
    }
    if (!media_active_ || !player_) {
        pending_tts_.push_back(text);
        return;
    }
    if (!session_id_) {
        logging::get_logger()->warn(with_kv(
            "TTS skipped: session_id missing",
            {kv("text", text)}));
        return;
    }
    try {
        const auto blob = app_.synthesize_session_audio(*session_id_, text);
        if (blob.size() < 364) {
            logging::get_logger()->info(with_kv(
                "TTS audio too short",
                {kv("blob_size", static_cast<int>(blob.size())),
                 kv("session_id", *session_id_)}));
            return;
        }
        const auto filename = make_tts_path();
        std::error_code ec;
        std::filesystem::create_directories(filename.parent_path(), ec);
        std::ofstream out(filename, std::ios::binary);
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.close();
        player_->enqueue(filename, true);
        player_->play();
    } catch (const std::exception& ex) {
        logging::get_logger()->error(with_kv(
            "TTS synthesize failed",
            {kv("error", ex.what()),
             kv("session_id", session_id_.value_or(""))}));
    }
}

void SipCall::play_pending_tts() {
    if (pending_tts_.empty()) {
        return;
    }
    auto pending = std::move(pending_tts_);
    pending_tts_.clear();
    for (const auto& text : pending) {
        enqueue_tts_text(text);
    }
}

std::filesystem::path SipCall::make_tts_path() const {
    static std::atomic<uint64_t> counter{0};
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    const auto prefix = session_id_.value_or("call");
    return app_.config().tmp_audio_dir /
           ("tts-" + prefix + "-" + std::to_string(stamp) + "-" + std::to_string(id) + ".wav");
}

std::string SipCall::recording_basename() const {
    if (session_id_) {
        return *session_id_;
    }
    return "call_" + std::to_string(getId());
}

}
