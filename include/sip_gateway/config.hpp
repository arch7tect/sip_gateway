#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sip_gateway {

struct Config {
    std::string sip_user;
    std::string sip_login;
    std::string sip_domain;
    std::string sip_password;
    std::optional<std::string> sip_caller_id;
    bool sip_null_device = true;
    std::filesystem::path tmp_audio_dir;
    std::filesystem::path sip_audio_dir;
    int sip_port = 5060;
    bool sip_use_tcp = true;
    bool sip_use_ice = false;
    std::vector<std::string> sip_stun_servers;
    std::vector<std::string> sip_proxy_servers;
    double events_delay = 0.010;
    double async_delay = 0.005;
    int frame_time_usec = 60000;
    std::filesystem::path vad_model_path;
    std::string vad_model_url;
    bool ua_zero_thread_cnt = true;
    bool ua_main_thread_only = true;
    int ec_tail_len = 200;
    bool ec_no_vad = false;
    std::string log_level = "INFO";
    std::optional<std::string> log_filename;
    std::optional<std::filesystem::path> logs_dir;
    int pjsip_log_level = 1;
    int vad_sampling_rate = 16000;
    double vad_threshold = 0.65;
    int vad_min_speech_duration_ms = 150;
    int vad_min_silence_duration_ms = 300;
    int vad_speech_pad_ms = 700;
    int vad_speech_prob_window = 3;
    bool vad_correction_debug = false;
    double vad_correction_enter_thres = 0.6;
    double vad_correction_exit_thres = 0.4;
    int short_pause_offset_ms = 200;
    int long_pause_offset_ms = 850;
    int user_silence_timeout_ms = 60000;
    int call_connection_timeout = 10;
    int sip_rest_api_port = 8000;
    bool use_local_stt = false;
    std::string local_stt_url;
    std::string local_stt_lang = "en";
    double greeting_delay_sec = 0.0;
    std::map<std::string, int> codecs_priority;
    bool interruptions_are_allowed = true;
    bool record_audio_parts = false;
    std::optional<std::string> flametree_callback_url;
    int flametree_callback_port = 8088;
    std::string backend_url;
    bool rewrite_root = true;
    bool sip_early_eoc = false;
    bool vad_use_dynamic_corrections = true;
    std::optional<std::string> authorization_token;
    double backend_request_timeout = 60.0;
    double backend_connect_timeout = 60.0;
    double backend_sock_read_timeout = 60.0;
    std::string session_type = "inbound";
    bool is_streaming = false;
    bool show_waiting_messages = false;
    std::string log_name = "sip_gateway";

    static Config load();
    void validate() const;
};

}
