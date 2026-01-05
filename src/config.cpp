#include "sip_gateway/config.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace sip_gateway {

namespace {

std::string get_env_str(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

std::optional<std::string> get_env_optional(const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return std::nullopt;
    }
    std::string result(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

std::string get_env_required(const char* name) {
    const char* value = std::getenv(name);
    if (!value || std::string(value).empty()) {
        throw std::runtime_error(std::string(name) + " is required");
    }
    return std::string(value);
}

bool get_env_bool(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value) {
        return fallback;
    }
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "true";
}

int get_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value ? std::stoi(value) : fallback;
}

double get_env_double(const char* name, double fallback) {
    const char* value = std::getenv(name);
    return value ? std::stod(value) : fallback;
}

std::string trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](unsigned char ch) { return !is_space(ch); }).base(),
                value.end());
    return value;
}

std::vector<std::string> split_csv(const std::string& raw) {
    std::vector<std::string> result;
    std::stringstream stream(raw);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

std::map<std::string, int> parse_json_map(const std::string& raw,
                                          const std::map<std::string, int>& fallback) {
    if (raw.empty()) {
        return fallback;
    }
    auto json = nlohmann::json::parse(raw);
    if (!json.is_object()) {
        throw std::runtime_error("CODECS_PRIORITY must be a JSON object");
    }
    std::map<std::string, int> result;
    for (auto it = json.begin(); it != json.end(); ++it) {
        result[it.key()] = it.value().get<int>();
    }
    return result;
}

std::string timestamp_suffix() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_value{};
#if defined(_WIN32)
    localtime_s(&tm_value, &time_t);
#else
    localtime_r(&time_t, &tm_value);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm_value, "%Y%m%d_%H%M%S");
    return stream.str();
}

void set_env_value(const std::string& key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

std::string strip_quotes(std::string value) {
    if (value.size() < 2) {
        return value;
    }
    if ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

void load_dotenv() {
    const std::filesystem::path dotenv_path = std::filesystem::current_path() / ".env";
    if (!std::filesystem::exists(dotenv_path)) {
        return;
    }

    std::ifstream stream(dotenv_path);
    if (!stream.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line.rfind("#", 0) == 0) {
            continue;
        }

        if (line.rfind("export ", 0) == 0) {
            line = trim(line.substr(7));
        }

        const auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, eq_pos));
        std::string value = trim(line.substr(eq_pos + 1));
        if (key.empty()) {
            continue;
        }
        value = strip_quotes(value);
        set_env_value(key, value);
    }
}

}

Config Config::load() {
    load_dotenv();
    Config config;
    const auto cwd = std::filesystem::current_path();
    const auto audio_base = get_env_str("SIP_AUDIO_DIR", cwd.string());

    config.sip_user = get_env_str("SIP_USER", "user");
    config.sip_login = get_env_str("SIP_LOGIN", config.sip_user);
    config.sip_domain = get_env_str("SIP_DOMAIN", "sip.linphone.org");
    config.sip_password = get_env_str("SIP_PASSWORD", "password");
    config.sip_caller_id = get_env_optional("SIP_CALLER_ID");
    config.sip_null_device = get_env_bool("SIP_NULL_DEVICE", true);
    config.tmp_audio_dir = get_env_str("SIP_AUDIO_TMP_DIR", audio_base + "/tmp");
    config.sip_audio_dir = get_env_str("SIP_AUDIO_WAV_DIR", audio_base + "/wav");
    config.sip_port = get_env_int("SIP_PORT", 5060);
    config.sip_max_calls = get_env_int("SIP_MAX_CALLS", 32);
    config.sip_use_tcp = get_env_bool("SIP_USE_TCP", true);
    config.sip_use_ice = get_env_bool("SIP_USE_ICE", false);
    config.sip_stun_servers = split_csv(get_env_str("SIP_STUN_SERVERS", ""));
    config.sip_proxy_servers = split_csv(get_env_str("SIP_PROXY_SERVERS", ""));

    config.events_delay = get_env_double("EVENTS_DELAY", 0.010);
    config.async_delay = get_env_double("ASYNC_DELAY", 0.005);
    config.frame_time_usec = get_env_int("FRAME_TIME_USEC", 60000);
    config.vad_model_path = std::filesystem::path(get_env_str("VAD_MODEL_PATH", cwd.string())) /
                            "silero_vad.onnx";
    config.vad_model_url = get_env_str(
        "VAD_MODEL_URL",
        "https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx");
    config.ua_zero_thread_cnt = get_env_bool("UA_ZERO_THREAD_CNT", true);
    config.ua_main_thread_only = get_env_bool("UA_MAIN_THREAD_ONLY", true);
    config.ec_tail_len = get_env_int("EC_TAIL_LEN", 200);
    config.ec_no_vad = get_env_bool("EC_NO_VAD", false);
    config.sip_media_thread_cnt = get_env_int("SIP_MEDIA_THREAD_CNT", 1);
    config.log_level = get_env_str("LOG_LEVEL", "INFO");

    const auto log_filename_raw = get_env_str("LOG_FILENAME", "");
    if (!log_filename_raw.empty()) {
        const std::filesystem::path log_path(log_filename_raw);
        const auto stamped = log_path.stem().string() + "_" + timestamp_suffix() +
                             log_path.extension().string();
        if (const auto log_dir = get_env_optional("LOGS_DIR")) {
            config.logs_dir = std::filesystem::path(*log_dir);
            config.log_filename = (std::filesystem::path(*log_dir) / stamped).string();
        } else {
            config.log_filename = stamped;
        }
    }

    config.pjsip_log_level = get_env_int("PJSIP_LOG_LEVEL", 1);
    const int default_console_level = config.log_filename ? 0 : config.pjsip_log_level;
    config.pjsip_console_log_level =
        get_env_int("PJSIP_CONSOLE_LOG_LEVEL", default_console_level);

    config.vad_sampling_rate = get_env_int("VAD_SAMPLING_RATE", 16000);
    config.vad_threshold = get_env_double("VAD_THRESHOLD", 0.65);
    config.vad_min_speech_duration_ms = get_env_int("VAD_MIN_SPEECH_DURATION_MS", 150);
    config.vad_min_silence_duration_ms = get_env_int("VAD_MIN_SILENCE_DURATION_MS", 300);
    config.vad_speech_pad_ms = get_env_int("VAD_SPEECH_PAD_MS", 700);
    config.vad_speech_prob_window = get_env_int("VAD_SPEECH_PROB_WINDOW", 3);
    config.vad_correction_debug = get_env_bool("VAD_CORRECTION_DEBUG", false);
    config.vad_correction_enter_thres = get_env_double("VAD_CORRECTION_ENTER_THRESHOLD", 0.6);
    config.vad_correction_exit_thres = get_env_double("VAD_CORRECTION_EXIT_THRESHOLD", 0.4);

    config.short_pause_offset_ms = get_env_int("SHORT_PAUSE_OFFSET_MS", 200);
    config.long_pause_offset_ms = get_env_int("LONG_PAUSE_OFFSET_MS", 850);
    config.user_silence_timeout_ms = get_env_int("USER_SILENCE_TIMEOUT_MS", 60000);
    config.min_speech_duration_sec = get_env_double("MIN_SPEECH_DURATION_SEC", 1.5);

    config.call_connection_timeout = get_env_int("CALL_CONNECTION_TIMEOUT", 10);
    config.sip_rest_api_port = get_env_int("SIP_REST_API_PORT", 8000);

    config.use_local_stt = get_env_bool("USE_LOCAL_STT", false);
    config.local_stt_url = get_env_str("LOCAL_STT_URL", "");
    config.local_stt_lang = get_env_str("LOCAL_STT_LANG", "en");

    config.greeting_delay_sec = get_env_double("GREETING_DELAY_SEC", 0.0);

    const std::map<std::string, int> default_codecs = {{"opus/48000", 254}, {"G722/16000", 253}};
    config.codecs_priority = parse_json_map(get_env_str("CODECS_PRIORITY", ""), default_codecs);

    config.interruptions_are_allowed = get_env_bool("INTERRUPTIONS_ARE_ALLOWED", true);
    config.record_audio_parts = get_env_bool("RECORD_AUDIO_PARTS", false);

    config.flametree_callback_url = get_env_optional("FLAMETREE_CALLBACK_URL");
    config.flametree_callback_port = get_env_int("FLAMETREE_CALLBACK_PORT", 8088);

    config.backend_url = get_env_required("BACKEND_URL");
    config.rewrite_root = get_env_bool("REWRITE_ROOT", true);
    config.sip_early_eoc = get_env_bool("SIP_EARLY_EOC", false);
    config.vad_use_dynamic_corrections = get_env_bool("VAD_USE_DYNAMIC_CORRECTIONS", true);

    config.authorization_token = get_env_optional("AUTHORIZATION_TOKEN");
    config.backend_request_timeout = get_env_double("BACKEND_REQUEST_TIMEOUT", 60.0);
    config.backend_connect_timeout = get_env_double("BACKEND_CONNECT_TIMEOUT", 60.0);
    config.backend_sock_read_timeout = get_env_double("BACKEND_SOCK_READ_TIMEOUT", 60.0);

    config.session_type = get_env_str("SESSION_TYPE", "inbound");
    const bool streaming_flag = get_env_bool("IS_STREAMING", true);
    config.is_streaming = config.session_type != "inbound" && config.session_type != "outbound" &&
                          streaming_flag;
    config.show_waiting_messages = get_env_bool("SHOW_WAITING_MESSAGES", false);

    config.log_name = get_env_str("LOG_NAME", "sip_gateway");
    config.tts_max_inflight = get_env_int("TTS_MAX_INFLIGHT", 3);

    return config;
}

void Config::validate() const {
    if (sip_user.empty()) {
        throw std::runtime_error("SIP_USER is required");
    }
    if (sip_domain.empty()) {
        throw std::runtime_error("SIP_DOMAIN is required");
    }
    if (sip_password.empty()) {
        throw std::runtime_error("SIP_PASSWORD is required");
    }
    if (backend_url.empty()) {
        throw std::runtime_error("BACKEND_URL is required");
    }
    if (sip_port <= 0) {
        throw std::runtime_error("SIP_PORT must be positive");
    }
    if (sip_rest_api_port <= 0) {
        throw std::runtime_error("SIP_REST_API_PORT must be positive");
    }
    if (sip_max_calls <= 0) {
        throw std::runtime_error("SIP_MAX_CALLS must be positive");
    }
    if (sip_media_thread_cnt < 0) {
        throw std::runtime_error("SIP_MEDIA_THREAD_CNT must be zero or positive");
    }
    if (tts_max_inflight <= 0) {
        throw std::runtime_error("TTS_MAX_INFLIGHT must be positive");
    }
}

}
