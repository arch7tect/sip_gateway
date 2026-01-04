#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <pjsua2.hpp>
#include <nlohmann/json.hpp>

#include "sip_gateway/audio/player.hpp"
#include "sip_gateway/audio/port.hpp"
#include "sip_gateway/audio/recorder.hpp"
#include "sip_gateway/backend/ws_client.hpp"
#include "sip_gateway/vad/processor.hpp"

namespace sip_gateway {

class SipApp;

class SipCall : public pj::Call {
public:
    enum class CallState {
        WaitForUser,
        SpeculativeGenerate,
        CommitGenerate,
        Finished
    };

    SipCall(SipApp& app,
            pj::Account& account,
            std::string backend_url,
            int call_id = PJSUA_INVALID_ID);
    ~SipCall() override;

    void set_session_id(const std::string& session_id);
    const std::optional<std::string>& session_id() const;

    void make_call(const std::string& to_uri);
    void answer(int status_code);
    void hangup(int status_code);

    void connect_ws(BackendWsClient::MessageHandler on_message,
                    BackendWsClient::EventHandler on_timeout,
                    BackendWsClient::EventHandler on_close);
    void stop_ws();
    void set_greeting(std::optional<std::string> greeting);

    void handle_ws_message(const nlohmann::json& message);
    void handle_ws_timeout();
    void handle_ws_close();

    void onCallState(pj::OnCallStateParam& prm) override;
    void onCallMediaState(pj::OnCallMediaStateParam& prm) override;

private:
    void open_media();
    void close_media();
    void set_state(CallState state);
    void handle_audio_frame(const std::vector<int16_t>& data);
    void on_vad_speech_start(const std::vector<float>& audio, double start, double duration);
    void on_vad_speech_end(const std::vector<float>& audio, double start, double duration);
    void on_vad_short_pause(const std::vector<float>& audio, double start, double duration);
    void on_vad_long_pause(const std::vector<float>& audio, double start, double duration);
    void on_vad_user_silence_timeout(double current_time);
    std::string encode_wav(const std::vector<float>& audio) const;
    std::string transcribe_audio(const std::vector<float>& audio) const;
    void start_session_text(const std::string& text);
    void commit_session();
    void rollback_session();
    void handle_playback_finished();
    void clear_pending_tts();
    void enqueue_tts_text(const std::string& text, double delay_sec = 0.0);
    void play_pending_tts();
    std::filesystem::path make_tts_path() const;
    std::string recording_basename() const;

    SipApp& app_;
    BackendWsClient ws_client_;
    std::optional<std::string> session_id_;
    std::optional<std::string> greeting_;
    std::string to_uri_;
    bool media_active_ = false;
    std::unique_ptr<pj::AudioMedia> audio_media_;
    std::unique_ptr<audio::AudioMediaPort> media_port_;
    std::unique_ptr<audio::CallRecorder> recorder_;
    std::unique_ptr<audio::SmartPlayer> player_;
    std::vector<std::string> pending_tts_;
    std::unique_ptr<vad::StreamingVadProcessor> vad_processor_;
    std::mutex generation_mutex_;
    bool start_in_flight_ = false;
    bool commit_in_flight_ = false;
    bool start_sent_ = false;
    bool finished_ = false;
    CallState state_ = CallState::WaitForUser;
};

}
