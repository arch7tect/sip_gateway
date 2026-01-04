#pragma once

#include <filesystem>
#include <memory>
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
    void handle_audio_frame(const std::vector<int16_t>& data);
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
};

}
