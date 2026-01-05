#pragma once

#include <atomic>
#include <filesystem>
#include <chrono>
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
#include "sip_gateway/sip/tts_pipeline.hpp"
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
    void set_transfer_target(const std::string& to_uri, double delay_sec);

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
    void onCallTransferStatus(pj::OnCallTransferStatusParam& prm) override;

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
    bool start_transfer();
    void schedule_soft_hangup();
    bool ai_can_speak() const;
    bool is_active_ai_speech() const;
    bool has_tts_queue() const;
    bool is_same_unstable_text(const std::string& text) const;
    void cancel_tts_queue();
    void enqueue_tts_text(const std::string& text, double delay_sec = 0.0);
    void try_play_tts();
    std::filesystem::path make_tts_path() const;
    std::string recording_basename() const;
    std::optional<std::filesystem::path> synthesize_tts_text(
        const std::string& text,
        const std::shared_ptr<std::atomic<bool>>& canceled);

    SipApp& app_;
    BackendWsClient ws_client_;
    std::optional<std::string> session_id_;
    std::optional<std::string> greeting_;
    std::string to_uri_;
    std::optional<std::string> transfer_target_;
    double transfer_delay_sec_ = 1.0;
    bool transfer_started_ = false;
    std::optional<std::string> close_status_;
    std::mutex transfer_mutex_;
    std::atomic<bool> media_active_ = false; // Media is attached and active.
    bool user_speaking_ = false; // VAD currently reports user speech.
    bool soft_hangup_pending_ = false; // Hangup timer scheduled.
    std::string last_unstable_transcription_;
    std::optional<std::chrono::steady_clock::time_point> start_reply_generation_;
    std::optional<std::chrono::steady_clock::time_point> start_response_generation_;
    std::unique_ptr<pj::AudioMedia> audio_media_;
    std::unique_ptr<audio::AudioMediaPort> media_port_;
    std::unique_ptr<audio::CallRecorder> recorder_;
    std::unique_ptr<audio::SmartPlayer> player_;
    std::unique_ptr<TtsPipeline> tts_pipeline_;
    std::unique_ptr<vad::StreamingVadProcessor> vad_processor_;
    std::mutex generation_mutex_;
    bool start_in_flight_ = false; // Speculative start request in progress.
    bool commit_in_flight_ = false; // Commit request in progress.
    bool spec_active_ = false; // Speculative session is active.
    bool short_pause_handled_ = false; // Short pause already processed.
    bool long_pause_handled_ = false; // Long pause already processed.
    bool finished_ = false; // Backend indicates session end.
    CallState state_ = CallState::WaitForUser;
};

}
