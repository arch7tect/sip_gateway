#include "sip_gateway/sip/call.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

#include "sip_gateway/logging.hpp"
#include "sip_gateway/metrics.hpp"
#include "sip_gateway/sip/app.hpp"
#include "sip_gateway/utils/async.hpp"
#include "sip_gateway/utils/text.hpp"

namespace sip_gateway {

SipCall::SipCall(SipApp& app, pj::Account& account, std::string backend_url, int call_id)
    : pj::Call(account, call_id),
      app_(app),
      ws_client_(std::move(backend_url)) {
    tts_pipeline_ = std::make_unique<TtsPipeline>(
        app_.config().tts_max_inflight,
        [this](const std::string& text,
               const std::shared_ptr<std::atomic<bool>>& canceled) {
            return synthesize_tts_text(text, canceled);
        },
        [this](const std::filesystem::path& path, const std::string& text) {
            logging::debug(
                "TTS ready for playback",
                {kv("text", text),
                 kv("session_id", session_id_.value_or(""))});
            if (!player_) {
                return;
            }
            player_->enqueue(path, true);
            player_->play();
            if (vad_processor_) {
                vad_processor_->reset_user_salience();
            }
        },
        [this]() { try_play_tts(); });
}

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

void SipCall::set_transfer_target(const std::string& to_uri, double delay_sec) {
    std::lock_guard<std::mutex> lock(transfer_mutex_);
    transfer_target_ = to_uri;
    transfer_delay_sec_ = delay_sec;
    transfer_started_ = false;
}

void SipCall::connect_ws(BackendWsClient::MessageHandler on_message,
                         BackendWsClient::EventHandler on_timeout,
                         BackendWsClient::EventHandler on_close) {
    if (!session_id_) {
        logging::warn("WebSocket connect skipped: session_id is not set");
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
        if (!app_.config().is_streaming) {
            logging::debug(
                "WebSocket message ignored (streaming disabled)",
                {kv("session_id", session_id_.value_or(""))});
            return;
        }
        std::optional<std::chrono::steady_clock::time_point> reply_start;
        {
            std::lock_guard<std::mutex> lock(generation_mutex_);
            reply_start = start_reply_generation_;
            start_reply_generation_.reset();
        }
        if (reply_start) {
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - *reply_start).count();
            Metrics::instance().observe_response_time("generate", elapsed);
        }
        const auto text = message.value("message", "");
        if (!text.empty()) {
            if (user_speaking_) {
                logging::debug(
                    "WebSocket message discarded (user speaking)",
                    {kv("session_id", session_id_.value_or(""))});
                return;
            }
            logging::debug(
                "TTS queued from websocket (streaming)",
                {kv("text", text),
                 kv("session_id", session_id_.value_or(""))});
            enqueue_tts_text(text);
        }
        return;
    }
    if (type == "eos") {
        logging::debug(
            "WebSocket end of stream",
            {kv("session_id", session_id_.value_or(""))});
        if (state_ == CallState::Finished) {
            handle_playback_finished();
            return;
        }
        return;
    }
    if (type == "eoc") {
        logging::debug(
            "WebSocket end of conversation",
            {kv("session_id", session_id_.value_or(""))});
        if (app_.config().sip_early_eoc && state_ != CallState::SpeculativeGenerate) {
            finished_ = true;
            set_state(CallState::Finished);
            handle_playback_finished();
        }
        return;
    }
    logging::debug(
        "WebSocket message received",
        {kv("message", message.dump()),
         kv("session_id", session_id_.value_or(""))});
}

void SipCall::handle_ws_timeout() {
    logging::info(
        "WebSocket timeout received",
        {kv("session_id", session_id_.value_or(""))});
}

void SipCall::handle_ws_close() {
    logging::info(
        "WebSocket close received",
        {kv("session_id", session_id_.value_or(""))});
}

void SipCall::onCallState(pj::OnCallStateParam& prm) {
    (void)prm;
    try {
        const auto info = getInfo();
        logging::debug(
            "Call state changed",
            {kv("call_id", info.callIdString),
             kv("uri", info.remoteUri),
             kv("state", static_cast<int>(info.state)),
             kv("state_text", info.stateText),
             kv("session_id", session_id_.value_or(""))});
        if (info.state == PJSIP_INV_STATE_CONFIRMED) {
            open_media();
        }
        if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
            close_media();
            std::optional<std::string> status = close_status_;
            if (!status) {
                const auto code = info.lastStatusCode;
                if (code == PJSIP_SC_DECLINE) {
                    status = "declined";
                } else if (code == PJSIP_SC_BUSY_HERE) {
                    status = "busy";
                } else if (code == PJSIP_SC_REQUEST_TERMINATED) {
                    status = "canceled";
                } else if (code == PJSIP_SC_TEMPORARILY_UNAVAILABLE ||
                           code == PJSIP_SC_REQUEST_TIMEOUT) {
                    status = "noanswer";
                } else if (code == PJSIP_SC_NOT_FOUND) {
                    status = "not_found";
                } else if (code == PJSIP_SC_SERVICE_UNAVAILABLE ||
                           code == PJSIP_SC_SERVER_TIMEOUT) {
                    status = "network_error";
                } else if (code == PJSIP_SC_OK) {
                    status = "completed";
                } else {
                    status = "unknown";
                }
            }
            if (session_id_) {
                const auto session_id = *session_id_;
                utils::run_async([this, session_id, status]() {
                    try {
                        app_.close_session(session_id, status);
                    } catch (const std::exception& ex) {
                        logging::error(
                            "Backend close failed",
                            {kv("error", ex.what()),
                             kv("session_id", session_id),
                             kv("status", status.value_or(""))});
                    }
                });
            }
            app_.handle_call_disconnected(getId());
        }
    } catch (const std::exception& ex) {
        logging::error(
            "Call state handler exception",
            {kv("error", ex.what())});
    }
}

void SipCall::onCallMediaState(pj::OnCallMediaStateParam& prm) {
    (void)prm;
    try {
        logging::debug(
            "Call media state changed",
            {kv("session_id", session_id_.value_or(""))});
        if (!media_active_) {
            open_media();
        }
    } catch (const std::exception& ex) {
        logging::error(
            "Call media handler exception",
            {kv("error", ex.what())});
    }
}

void SipCall::onCallTransferStatus(pj::OnCallTransferStatusParam& prm) {
    logging::info(
        "Transfer status",
        {kv("status", prm.statusCode),
         kv("reason", prm.reason),
         kv("final_notify", prm.finalNotify),
         kv("session_id", session_id_.value_or(""))});
    if (prm.finalNotify) {
        if (prm.statusCode >= 200 && prm.statusCode < 300) {
            hangup(PJSIP_SC_OK);
        }
        prm.cont = false;
    }
}

void SipCall::open_media() {
    if (media_active_) {
        return;
    }
    try {
        audio_media_ = std::make_unique<pj::AudioMedia>(getAudioMedia(-1));
    } catch (const pj::Error& ex) {
        logging::error(
            "Call media not available",
            {kv("reason", ex.reason),
             kv("status", ex.status),
             kv("session_id", session_id_.value_or(""))});
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
        logging::error("Failed to attach media port",
                       {kv("reason", ex.reason),
                        kv("status", ex.status),
                        kv("session_id", session_id_.value_or(""))});
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
            logging::error("Failed to start call recorder",
                           {kv("error", ex.what()),
                            kv("filename", filename.string()),
                            kv("session_id", session_id_.value_or(""))});
            recorder_.reset();
        }
    }

    auto* recorder_media = recorder_ ? recorder_->get_recorder() : nullptr;
    player_ = std::make_unique<audio::SmartPlayer>(
        *audio_media_,
        recorder_media,
        [this, session_id = session_id_.value_or("")]() {
            logging::debug("Audio playback finished",
                           {kv("session_id", session_id)});
            handle_playback_finished();
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
                app_.config().vad_speech_prob_window,
                app_.config().vad_use_dynamic_corrections,
                app_.config().vad_correction_debug,
                app_.config().vad_correction_enter_thres,
                app_.config().vad_correction_exit_thres);
            vad_processor_->set_on_speech_start(
                [this](const std::vector<float>& audio, double start, double duration) {
                    on_vad_speech_start(audio, start, duration);
                });
            vad_processor_->set_on_speech_end(
                [this](const std::vector<float>& audio, double start, double duration) {
                    on_vad_speech_end(audio, start, duration);
                });
            vad_processor_->set_on_short_pause(
                [this](const std::vector<float>& audio, double start, double duration) {
                    on_vad_short_pause(audio, start, duration);
                });
            vad_processor_->set_on_long_pause(
                [this](const std::vector<float>& audio, double start, double duration) {
                    on_vad_long_pause(audio, start, duration);
                });
            vad_processor_->set_on_user_silence_timeout(
                [this](double current_time) {
                    on_vad_user_silence_timeout(current_time);
                });
        }
    }
    media_active_ = true;
    if (greeting_ && !greeting_->empty()) {
        enqueue_tts_text(*greeting_, app_.config().greeting_delay_sec);
    }
    try_play_tts();
}

void SipCall::close_media() {
    if (!media_active_) {
        return;
    }
    cancel_tts_queue();
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
    if (finished_) {
        return;
    }
    if (!app_.config().interruptions_are_allowed && is_active_ai_speech()) {
        return;
    }
    if (vad_processor_) {
        vad_processor_->process_samples(data);
    }
}

void SipCall::on_vad_speech_start(const std::vector<float>& audio,
                                  double start,
                                  double duration) {
    (void)audio;
    logging::debug(
        "VAD speech start",
        {kv("start_sec", start),
         kv("duration_sec", duration),
         kv("session_id", session_id_.value_or(""))});

    user_speaking_ = true;
    if (player_) {
        player_->interrupt();
    }
    cancel_tts_queue();
    if (vad_processor_) {
        vad_processor_->cancel_user_salience();
    }
    set_state(CallState::WaitForUser);
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        short_pause_handled_ = false;
        long_pause_handled_ = false;
        last_unstable_transcription_.clear();
    }

    utils::run_async([this]() {
        bool allow_rollback = false;
        {
            std::lock_guard<std::mutex> lock(generation_mutex_);
            allow_rollback = !commit_in_flight_ && spec_active_;
        }
        if (!allow_rollback) {
            return;
        }
        try {
            rollback_session();
        } catch (const std::exception& ex) {
            logging::warn(
                "Rollback failed",
                {kv("error", ex.what()),
                 kv("session_id", session_id_.value_or(""))});
        }
    });
}

void SipCall::on_vad_speech_end(const std::vector<float>& audio,
                                double start,
                                double duration) {
    (void)audio;
    logging::debug(
        "VAD speech end",
        {kv("start_sec", start),
         kv("duration_sec", duration),
         kv("session_id", session_id_.value_or(""))});
    user_speaking_ = false;
}

void SipCall::on_vad_short_pause(const std::vector<float>& audio,
                                 double start,
                                 double duration) {
    if (duration < app_.config().min_speech_duration_sec) {
        logging::debug(
            "Short pause ignored (speech too short)",
            {kv("duration_sec", duration),
             kv("min_required_sec", app_.config().min_speech_duration_sec),
             kv("session_id", session_id_.value_or(""))});
        return;
    }
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        if (start_in_flight_ || commit_in_flight_ || short_pause_handled_ || long_pause_handled_) {
            return;
        }
        start_in_flight_ = true;
    }
    logging::debug(
        "VAD short pause",
        {kv("start_sec", start),
         kv("duration_sec", duration),
         kv("session_id", session_id_.value_or(""))});

    auto audio_copy = audio;
    utils::run_async([this, audio_copy = std::move(audio_copy)]() mutable {
        try {
            if (!media_active_.load()) {
                logging::debug(
                    "Short pause skipped (call disconnected)",
                    {kv("session_id", session_id_.value_or(""))});
                std::lock_guard<std::mutex> lock(generation_mutex_);
                start_in_flight_ = false;
                return;
            }
            rollback_session();
            const auto text = transcribe_audio(audio_copy);
            if (!text.empty()) {
                if (is_same_unstable_text(text)) {
                    logging::debug(
                        "Speculation skipped (text unchanged)",
                        {kv("session_id", session_id_.value_or(""))});
                    std::lock_guard<std::mutex> lock(generation_mutex_);
                    start_in_flight_ = false;
                    return;
                }
                if (!media_active_.load()) {
                    logging::debug(
                        "Backend start skipped (call disconnected)",
                        {kv("session_id", session_id_.value_or(""))});
                    std::lock_guard<std::mutex> lock(generation_mutex_);
                    start_in_flight_ = false;
                    return;
                }
                start_session_text(text);
                std::lock_guard<std::mutex> lock(generation_mutex_);
                spec_active_ = true;
                short_pause_handled_ = true;
                set_state(CallState::SpeculativeGenerate);
            }
        } catch (const std::exception& ex) {
            logging::error(
                "Short pause handling failed",
                {kv("error", ex.what()),
                 kv("session_id", session_id_.value_or(""))});
        }
        std::lock_guard<std::mutex> lock(generation_mutex_);
        start_in_flight_ = false;
    });
}

void SipCall::on_vad_long_pause(const std::vector<float>& audio,
                                double start,
                                double duration) {
    if (audio.empty()) {
        logging::debug(
            "Long pause ignored (empty buffer)",
            {kv("session_id", session_id_.value_or(""))});
        return;
    }
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        if (commit_in_flight_ || long_pause_handled_) {
            return;
        }
        commit_in_flight_ = true;
    }
    logging::debug(
        "VAD long pause",
        {kv("start_sec", start),
         kv("duration_sec", duration),
         kv("session_id", session_id_.value_or(""))});

    auto audio_copy = audio;
    utils::run_async([this, audio_copy = std::move(audio_copy)]() mutable {
        if (vad_processor_) {
            vad_processor_->set_long_pause_suspended(true);
        }
        try {
            if (!media_active_.load()) {
                logging::debug(
                    "Long pause skipped (call disconnected)",
                    {kv("session_id", session_id_.value_or(""))});
                std::lock_guard<std::mutex> lock(generation_mutex_);
                commit_in_flight_ = false;
                if (vad_processor_) {
                    vad_processor_->set_long_pause_suspended(false);
                }
                return;
            }

            for (int i = 0; i < 200; ++i) {
                {
                    std::lock_guard<std::mutex> lock(generation_mutex_);
                    if (!start_in_flight_) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!media_active_.load()) {
                logging::debug(
                    "Long pause skipped (call disconnected)",
                    {kv("session_id", session_id_.value_or(""))});
                std::lock_guard<std::mutex> lock(generation_mutex_);
                commit_in_flight_ = false;
                if (vad_processor_) {
                    vad_processor_->set_long_pause_suspended(false);
                }
                return;
            }

            bool has_start = false;
            {
                std::lock_guard<std::mutex> lock(generation_mutex_);
                has_start = spec_active_;
            }
            if (!has_start) {
                const auto text = transcribe_audio(audio_copy);
                if (text.empty()) {
                    std::lock_guard<std::mutex> lock(generation_mutex_);
                    commit_in_flight_ = false;
                    if (vad_processor_) {
                        vad_processor_->set_long_pause_suspended(false);
                    }
                    return;
                }
                if (!media_active_.load()) {
                    logging::debug(
                        "Backend start skipped (call disconnected)",
                        {kv("session_id", session_id_.value_or(""))});
                    std::lock_guard<std::mutex> lock(generation_mutex_);
                    commit_in_flight_ = false;
                    if (vad_processor_) {
                        vad_processor_->set_long_pause_suspended(false);
                    }
                    return;
                }
                start_session_text(text);
                std::lock_guard<std::mutex> lock(generation_mutex_);
                spec_active_ = true;
                short_pause_handled_ = true;
                set_state(CallState::SpeculativeGenerate);
            }
            if (!media_active_.load()) {
                logging::debug(
                    "Backend commit skipped (call disconnected)",
                    {kv("session_id", session_id_.value_or(""))});
                std::lock_guard<std::mutex> lock(generation_mutex_);
                commit_in_flight_ = false;
                if (vad_processor_) {
                    vad_processor_->set_long_pause_suspended(false);
                }
                return;
            }
            set_state(CallState::CommitGenerate);
            user_speaking_ = false;
            commit_session();
            std::lock_guard<std::mutex> lock(generation_mutex_);
            spec_active_ = false;
            long_pause_handled_ = true;
        } catch (const std::exception& ex) {
            logging::error(
                "Long pause handling failed",
                {kv("error", ex.what()),
                 kv("session_id", session_id_.value_or(""))});
        }
        std::lock_guard<std::mutex> lock(generation_mutex_);
        commit_in_flight_ = false;
        if (vad_processor_) {
            vad_processor_->set_long_pause_suspended(false);
        }
    });
}

void SipCall::on_vad_user_silence_timeout(double current_time) {
    logging::debug(
        "VAD user silence timeout",
        {kv("time_sec", current_time),
         kv("session_id", session_id_.value_or(""))});
    finished_ = true;
    set_state(CallState::Finished);
    handle_playback_finished();
}

std::string SipCall::encode_wav(const std::vector<float>& audio) const {
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t sample_rate = static_cast<uint32_t>(app_.config().vad_sampling_rate);
    const uint16_t block_align = channels * (bits_per_sample / 8);
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_size = static_cast<uint32_t>(audio.size() * sizeof(int16_t));
    const uint32_t chunk_size = 36 + data_size;

    std::string result;
    result.reserve(44 + data_size);
    auto append = [&result](const void* data, size_t size) {
        result.append(static_cast<const char*>(data), size);
    };
    auto append_u16 = [&append](uint16_t value) {
        const uint8_t bytes[2] = {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF)
        };
        append(bytes, sizeof(bytes));
    };
    auto append_u32 = [&append](uint32_t value) {
        const uint8_t bytes[4] = {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF)
        };
        append(bytes, sizeof(bytes));
    };

    append("RIFF", 4);
    append_u32(chunk_size);
    append("WAVE", 4);
    append("fmt ", 4);
    append_u32(16);
    append_u16(1);
    append_u16(channels);
    append_u32(sample_rate);
    append_u32(byte_rate);
    append_u16(block_align);
    append_u16(bits_per_sample);
    append("data", 4);
    append_u32(data_size);

    for (float sample : audio) {
        const float clamped = std::max(-1.0f, std::min(1.0f, sample));
        const int16_t pcm = static_cast<int16_t>(
            clamped * std::numeric_limits<int16_t>::max());
        append(&pcm, sizeof(pcm));
    }

    return result;
}

std::string SipCall::transcribe_audio(const std::vector<float>& audio) const {
    if (!session_id_) {
        return "";
    }
    const auto wav_bytes = encode_wav(audio);
    const auto start = std::chrono::steady_clock::now();
    const auto text = app_.transcribe_audio(wav_bytes);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    Metrics::instance().observe_response_time("transcribe", elapsed);
    return text;
}

void SipCall::start_session_text(const std::string& text) {
    if (!session_id_) {
        return;
    }
    if (text.empty()) {
        return;
    }
    cancel_tts_queue();
    if (player_) {
        player_->interrupt();
    }
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        last_unstable_transcription_ = text;
        start_reply_generation_ = std::chrono::steady_clock::now();
        start_response_generation_ = *start_reply_generation_;
    }
    app_.start_session_text(*session_id_, text);
}

void SipCall::commit_session() {
    if (!session_id_) {
        return;
    }
    try {
        auto response = app_.commit_session(*session_id_);
        if (!app_.config().is_streaming &&
            response.contains("response") && response["response"].is_string()) {
            const auto text = response["response"].get<std::string>();
            if (!text.empty()) {
                logging::debug(
                    "TTS queued from commit response",
                    {kv("text", text),
                     kv("session_id", session_id_.value_or(""))});
                enqueue_tts_text(text);
            }
        }
        if (response.contains("metadata") && response["metadata"].is_object()) {
            const auto& metadata = response["metadata"];
            if (metadata.contains("SESSION_ENDS") && metadata["SESSION_ENDS"].is_boolean()) {
                if (metadata["SESSION_ENDS"].get<bool>()) {
                    finished_ = true;
                    set_state(CallState::Finished);
                }
            }
        }
        if (!finished_) {
            set_state(CallState::WaitForUser);
        }
        if (finished_) {
            handle_playback_finished();
        }
    } catch (const std::exception& ex) {
        logging::error(
            "Commit failed",
            {kv("error", ex.what()),
             kv("session_id", session_id_.value_or(""))});
        set_state(CallState::WaitForUser);
    }
    last_unstable_transcription_.clear();
}

void SipCall::rollback_session() {
    bool needs_rollback = false;
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        if (spec_active_ && !commit_in_flight_) {
            needs_rollback = true;
            spec_active_ = false;
            short_pause_handled_ = false;
            start_in_flight_ = false;
        }
    }
    if (!session_id_ || !needs_rollback) {
        return;
    }
    app_.rollback_session(*session_id_);
}

void SipCall::handle_playback_finished() {
    if (!finished_) {
        if (vad_processor_) {
            vad_processor_->start_user_silence();
        }
        return;
    }
    if (player_ && player_->is_active()) {
        return;
    }
    if (has_tts_queue()) {
        return;
    }
    schedule_soft_hangup();
}

bool SipCall::ai_can_speak() const {
    return state_ == CallState::WaitForUser ||
           state_ == CallState::CommitGenerate ||
           state_ == CallState::Finished;
}

bool SipCall::is_active_ai_speech() const {
    const bool player_active = player_ && player_->is_active();
    const bool has_queued = has_tts_queue() && ai_can_speak();
    return player_active || has_queued || commit_in_flight_;
}

bool SipCall::is_same_unstable_text(const std::string& text) const {
    if (last_unstable_transcription_.empty()) {
        return false;
    }
    return utils::normalize_text(last_unstable_transcription_) ==
           utils::normalize_text(text);
}

void SipCall::schedule_soft_hangup() {
    if (soft_hangup_pending_) {
        return;
    }
    soft_hangup_pending_ = true;
    utils::run_async([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        soft_hangup_pending_ = false;
        if (!finished_) {
            return;
        }
        if (player_ && player_->is_active()) {
            return;
        }
        if (has_tts_queue()) {
            return;
        }
        if (start_transfer()) {
            return;
        }
        hangup(PJSIP_SC_OK);
    });
}

bool SipCall::start_transfer() {
    std::string target;
    double delay_sec = 1.0;
    {
        std::lock_guard<std::mutex> lock(transfer_mutex_);
        if (!transfer_target_ || transfer_target_->empty() || transfer_started_) {
            return false;
        }
        transfer_started_ = true;
        target = *transfer_target_;
        delay_sec = transfer_delay_sec_;
    }
    logging::info(
        "Transfer initiated",
        {kv("to_uri", target),
         kv("delay_sec", delay_sec),
         kv("session_id", session_id_.value_or(""))});
    close_status_ = "transferred";

    if (target.rfind("dtmf:", 0) == 0) {
        const auto digits = target.substr(5);
        if (!digits.empty()) {
            try {
                dialDtmf(digits);
            } catch (const pj::Error& ex) {
                logging::error(
                    "DTMF transfer failed",
                    {kv("reason", ex.reason),
                     kv("status", ex.status),
                     kv("session_id", session_id_.value_or(""))});
            }
        }
        utils::run_async([this, delay_sec]() {
            std::this_thread::sleep_for(std::chrono::duration<double>(delay_sec));
            try {
                hangup(PJSIP_SC_OK);
            } catch (const pj::Error&) {
            }
        });
        return true;
    }

    pj::CallOpParam prm(true);
    try {
        xfer(target, prm);
    } catch (const pj::Error& ex) {
        logging::error(
            "Transfer failed",
            {kv("reason", ex.reason),
             kv("status", ex.status),
             kv("session_id", session_id_.value_or(""))});
        return false;
    }
    return true;
}

void SipCall::cancel_tts_queue() {
    if (tts_pipeline_) {
        tts_pipeline_->cancel();
    }
}

void SipCall::enqueue_tts_text(const std::string& text, double delay_sec) {
    const auto sanitized = utils::remove_emojis(text);
    if (sanitized.empty()) {
        return;
    }
    if (!session_id_) {
        logging::warn(
            "TTS skipped: session_id missing",
            {kv("text", sanitized)});
        return;
    }
    logging::debug(
        "TTS queued for synthesis",
        {kv("text", sanitized),
         kv("session_id", session_id_.value_or(""))});
    if (tts_pipeline_) {
        tts_pipeline_->enqueue(sanitized, delay_sec);
    }
}

std::optional<std::filesystem::path> SipCall::synthesize_tts_text(
    const std::string& text,
    const std::shared_ptr<std::atomic<bool>>& canceled) {
    if (!session_id_) {
        return std::nullopt;
    }
    if (canceled && canceled->load()) {
        return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::time_point> response_start;
    {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        response_start = start_response_generation_;
    }
    try {
        const auto synth_start = std::chrono::steady_clock::now();
        const auto blob = app_.synthesize_session_audio(*session_id_, text);
        const auto synth_elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - synth_start).count();
        if (response_start) {
            Metrics::instance().observe_response_time("synthesize", synth_elapsed);
        }
        if (canceled && canceled->load()) {
            return std::nullopt;
        }
        if (blob.size() < 364) {
            logging::info(
                "TTS audio too short",
                {kv("blob_size", static_cast<int>(blob.size())),
                 kv("session_id", session_id_.value_or(""))});
            return std::nullopt;
        }
        if (response_start) {
            const auto response_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - *response_start).count();
            Metrics::instance().observe_response_time("play_queue", response_elapsed);
            Metrics::instance().observe_response_summary("play_queue", response_elapsed);
            logging::debug(
                "Response ready",
                {kv("elapsed_sec", response_elapsed),
                 kv("session_id", session_id_.value_or(""))});
            std::lock_guard<std::mutex> lock(generation_mutex_);
            start_response_generation_.reset();
        }
        const auto filename = make_tts_path();
        std::error_code ec;
        std::filesystem::create_directories(filename.parent_path(), ec);
        std::ofstream out(filename, std::ios::binary);
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.close();
        return filename;
    } catch (const std::exception& ex) {
        logging::error(
            "TTS synthesize failed",
            {kv("error", ex.what()),
             kv("session_id", session_id_.value_or(""))});
        return std::nullopt;
    }
}

void SipCall::try_play_tts() {
    if (!tts_pipeline_) {
        return;
    }
    tts_pipeline_->try_play(media_active_ && player_);
}

bool SipCall::has_tts_queue() const {
    if (!tts_pipeline_) {
        return false;
    }
    return tts_pipeline_->has_queue();
}

void SipCall::set_state(CallState state) {
    if (state_ == CallState::Finished && state != CallState::Finished) {
        return;
    }
    if (state_ == state) {
        return;
    }
    state_ = state;
    const char* name = "unknown";
    switch (state_) {
        case CallState::WaitForUser:
            name = "WAIT_FOR_USER";
            break;
        case CallState::SpeculativeGenerate:
            name = "SPECULATIVE_GENERATE";
            break;
        case CallState::CommitGenerate:
            name = "COMMIT_GENERATE";
            break;
        case CallState::Finished:
            name = "FINISHED";
            break;
    }
    logging::debug(
        "Call state change",
        {kv("state", name),
         kv("session_id", session_id_.value_or(""))});
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
