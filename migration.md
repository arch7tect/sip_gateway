# SIP Integration Migration to C++17 - Standalone Microservice

## Executive Summary

This document outlines the plan for migrating the Python-based SIP integration to a **standalone C++17 microservice**. The migration eliminates Python entirely from the SIP stack, creating a high-performance, low-latency voice gateway that communicates with the existing Python backend via REST/WebSocket APIs. This architecture provides better performance, easier deployment, and cleaner separation of concerns.

## Current Architecture Analysis

### Python Components

The current SIP integration consists of the following key components:

1. **PjApp** (`pjapp.py`) - Main application managing PJSIP endpoint and FastAPI server
2. **PjCall** (`pjcall.py`) - Call session controller with state machine and audio processing
3. **PjAccount** (`pjaccount.py`) - SIP account management
4. **Audio Components**:
   - `audio_media_player.py` - Smart audio playback queue with automatic file management
   - `recording_port.py` - Audio recording port for WAV capture
   - `audio_message.py` - Audio message abstraction
5. **VAD System** (`vad/processor6.py`) - Voice Activity Detection with ONNX Runtime
6. **Support Modules**:
   - `async_callback.py` - Async callback job dispatcher
   - `task_manager.py` - Task lifecycle management
   - `sip_config.py` - Configuration management
   - `sip_mixin.py` - Mixin for SIP functionality
   - `wav_writer.py` - WAV file writing utilities

### Dependencies

- **PJSIP** (pjsua2 Python bindings) - SIP protocol stack
- **ONNX Runtime** - VAD model inference
- **NumPy/SciPy** - Audio buffer processing
- **aiohttp** - HTTP/WebSocket server
- **asyncio** - Asynchronous I/O

### Key Challenges

1. **Real-time Audio Processing** - Low-latency audio frame processing (60ms frames)
2. **Async Integration** - Bridge between PJSIP C callbacks and Python asyncio
3. **State Management** - Complex call state machine with multiple concurrent operations
4. **Memory Management** - Large audio buffers and ONNX model state
5. **Backend Integration** - WebSocket communication with Python backend

## Migration Goals

### Performance Targets

- **Reduce audio processing latency** by 30-50% (eliminate Python overhead)
- **Decrease memory footprint** by 40-60% (native C++ memory management)
- **Improve VAD inference speed** by 20-30% (C++ ONNX Runtime API)
- **Lower CPU usage** by 25-40% (compiled code, better thread management)

### Quality Goals

- **Type Safety** - Leverage C++17 type system and compile-time checks
- **Memory Safety** - Use smart pointers, RAII, and modern C++ idioms
- **Maintainability** - Clear separation of concerns, well-documented interfaces
- **Testability** - Unit tests for core components, integration tests for call flow

## Migration Strategy

### Implementation Prerequisites (Must Be Frozen Before Phase 6)

1. **Backend API Contract**: No backend integration code may be written until the REST/WS contract is frozen. The contract must define routes, JSON schemas, WS message types, auth, and error/retry semantics. Reference an authoritative spec (OpenAPI) or add `docs/backend_api.md`.
2. **Environment Compatibility**: Compatibility means same env var names and precedence rules; defaults must match Python behavior or be explicitly documented as deviations. Add `docs/env_compat.md` with identical behavior, equivalent behavior, and intentional deviations.
3. **PJSIP Threading Model**: Define production threading vs. debug/main-thread-only behavior before implementation. Document in `docs/architecture.md` and make threading mode configurable.
4. **Dependency Strategy**: Choose a reproducible dependency strategy (CMake `FetchContent`, submodules, or system packages) and list all third-party libs (e.g., spdlog, nlohmann/json, HTTP/WS, test framework).

**Selected strategy**:
- Use CMake `FetchContent` for CMake-native libs (spdlog, nlohmann/json, Catch2/GoogleTest, httplib/websocketpp).
- Use `ExternalProject` (or scripted builds) for PJSIP and ONNX Runtime to control configure flags and codecs.

**Pinned versions (initial proposal)**:
- PJSIP: 2.16
- ONNX Runtime: 1.23.2
- spdlog: 1.13.0
- nlohmann/json: 3.11.3
- cpp-httplib: 0.16.0
- websocketpp: 0.8.2
- Catch2: 3.5.4

### Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    SIP C++ Microservice                  │
│  ┌────────────┐  ┌──────────┐  ┌─────────────────────┐ │
│  │   PJSIP    │  │   VAD    │  │   HTTP/WebSocket    │ │
│  │  Endpoint  │  │  Engine  │  │      Client         │ │
│  └────────────┘  └──────────┘  └─────────────────────┘ │
│         │              │                   │            │
│         └──────┬───────┴───────────────────┘            │
│                │                                         │
│         ┌──────▼──────┐                                 │
│         │ Call Manager│                                 │
│         └─────────────┘                                 │
│                                                          │
│  Exposes: POST /call, POST /transfer, GET /health       │
└──────────────────────────┬───────────────────────────────┘
                           │ REST/WebSocket
                           │
┌──────────────────────────▼───────────────────────────────┐
│              Backend Python FastAPI Service              │
│  ┌──────────┐  ┌──────────┐  ┌────────┐  ┌──────────┐  │
│  │   LLM    │  │  Skills  │  │  Tools │  │ Sessions │  │
│  │  Chain   │  │  Engine  │  │   API  │  │ Manager  │  │
│  └──────────┘  └──────────┘  └────────┘  └──────────┘  │
│                                                          │
│  Exposes: /session, /synthesize, /transcribe, /ws/*     │
└──────────────────────────────────────────────────────────┘
```

## Environment Variable Compatibility

**Critical Design Decision**: The C++ implementation will maintain **100% compatibility** with the existing Python environment variables. This ensures:

- **Seamless Migration** - Same `.env` files work for both versions
- **Drop-in Replacement** - No configuration changes required
- **Side-by-Side Testing** - Easy comparison during migration
- **Operational Continuity** - No retraining required for operators

**Compatibility definition**:
- Same variable names and precedence rules.
- Defaults must match Python behavior exactly or be documented as intentional deviations.

### Complete Environment Variable Reference

All environment variables from the Python `sip_config.py` will be supported:

#### SIP Account Settings
```bash
SIP_USER=user                    # SIP username
SIP_LOGIN=user                   # SIP login (defaults to SIP_USER)
SIP_DOMAIN=sip.linphone.org      # SIP domain
SIP_PASSWORD=password            # SIP password
SIP_CALLER_ID=                   # Optional caller ID display name
SIP_PORT=5060                    # SIP port (default: 5060)
SIP_USE_TCP=True                 # Enable TCP transport
SIP_USE_ICE=False                # Enable ICE for NAT traversal
SIP_STUN_SERVERS=                # Comma-separated STUN servers
SIP_PROXY_SERVERS=               # Comma-separated proxy servers
```

#### Audio Settings
```bash
SIP_NULL_DEVICE=True             # Use null audio device (no sound card)
SIP_AUDIO_DIR=.                  # Base audio directory
SIP_AUDIO_TMP_DIR=./tmp          # Temporary audio files
SIP_AUDIO_WAV_DIR=./wav          # WAV recording directory
FRAME_TIME_USEC=60000            # Audio frame time in microseconds (60ms)
CODECS_PRIORITY={"opus/48000":254,"G722/16000":253}  # Codec priorities (JSON)
RECORD_AUDIO_PARTS=false         # Record individual audio parts
```

#### PJSIP Internal Settings
```bash
UA_ZERO_THREAD_CNT=True          # Use zero worker threads
UA_MAIN_THREAD_ONLY=True         # Run on main thread only
EC_TAIL_LEN=200                  # Echo cancellation tail length
EC_NO_VAD=False                  # Disable PJSIP internal VAD
PJSIP_LOG_LEVEL=1                # PJSIP library log level (0-6)
```

#### VAD (Voice Activity Detection) Settings
```bash
VAD_MODEL_PATH=./silero_vad.onnx # Path to ONNX VAD model
VAD_MODEL_URL=https://...        # URL to download VAD model
VAD_SAMPLING_RATE=16000          # VAD sampling rate (Hz)
VAD_THRESHOLD=0.65               # Speech probability threshold
VAD_MIN_SPEECH_DURATION_MS=150   # Minimum speech duration
VAD_MIN_SILENCE_DURATION_MS=300  # Minimum silence duration
VAD_SPEECH_PAD_MS=700            # Speech padding duration
VAD_SPEECH_PROB_WINDOW=3         # Speech probability window size
VAD_CORRECTION_DEBUG=false       # Enable VAD correction debugging
VAD_CORRECTION_ENTER_THRESHOLD=0.6   # Correction enter threshold
VAD_CORRECTION_EXIT_THRESHOLD=0.4    # Correction exit threshold
```

#### Timing & Pause Detection
```bash
EVENTS_DELAY=0.010               # PJSIP event polling delay (seconds)
ASYNC_DELAY=0.005                # Async loop sleep delay (seconds)
SHORT_PAUSE_OFFSET_MS=200        # Short pause detection offset
LONG_PAUSE_OFFSET_MS=850         # Long pause detection offset
USER_SILENCE_TIMEOUT_MS=60000    # User silence timeout (1 minute)
GREETING_DELAY_SEC=0.0           # Delay before playing greeting
```

#### Logging Settings
```bash
LOG_LEVEL=INFO                   # Application log level
LOG_FILENAME=                    # PJSIP log filename (timestamped)
LOGS_DIR=                        # Directory for log files
```

#### REST API & Integration
```bash
SIP_REST_API_PORT=8000           # REST API server port
CALL_CONNECTION_TIMEOUT=10       # Call connection timeout (seconds)
BACKEND_URL=http://...           # Backend service URL (from CLAUDE.md)
INTERRUPTIONS_ARE_ALLOWED=true   # Allow user interruptions
```

#### Local STT (Speech-to-Text) - Optional
```bash
USE_LOCAL_STT=false              # Use local STT instead of backend
LOCAL_STT_URL=                   # Local STT service URL
LOCAL_STT_LANG=en                # STT language code
```

### C++ Implementation

The `SipConfig` class will use the same environment variable names:

```cpp
// include/sip/utils/config.hpp
#pragma once

#include <string>
#include <optional>
#include <filesystem>
#include <map>

namespace sip {

class SipConfig {
public:
    static SipConfig& instance();

    // SIP Account Settings
    std::string sip_user;
    std::string sip_login;
    std::string sip_domain;
    std::string sip_password;
    std::optional<std::string> sip_caller_id;
    int sip_port;
    bool sip_use_tcp;
    bool sip_use_ice;
    std::vector<std::string> sip_stun_servers;
    std::vector<std::string> sip_proxy_servers;

    // Audio Settings
    bool sip_null_device;
    std::filesystem::path tmp_audio_dir;
    std::filesystem::path sip_audio_dir;
    int frame_time_usec;
    std::map<std::string, int> codecs_priority;
    bool record_audio_parts;

    // PJSIP Settings
    bool ua_zero_thread_cnt;
    bool ua_main_thread_only;
    int ec_tail_len;
    bool ec_no_vad;
    int pjsip_log_level;

    // VAD Settings
    std::filesystem::path vad_model_path;
    std::string vad_model_url;
    int vad_sampling_rate;
    float vad_threshold;
    int vad_min_speech_duration_ms;
    int vad_min_silence_duration_ms;
    int vad_speech_pad_ms;
    int vad_speech_prob_window;
    bool vad_correction_debug;
    float vad_correction_enter_thres;
    float vad_correction_exit_thres;

    // Timing Settings
    float events_delay;
    float async_delay;
    int short_pause_offset_ms;
    int long_pause_offset_ms;
    int user_silence_timeout_ms;
    float greeting_delay_sec;

    // Logging Settings
    std::string log_level;
    std::optional<std::string> log_filename;
    std::optional<std::filesystem::path> logs_dir;

    // REST API Settings
    int sip_rest_api_port;
    int call_connection_timeout;
    std::string backend_url;
    bool interruptions_are_allowed;

    // Local STT Settings
    bool use_local_stt;
    std::string local_stt_url;
    std::string local_stt_lang;

    void load_from_env();
    void validate() const;

private:
    SipConfig() = default;

    // Helper methods for environment variable parsing
    std::string get_env(const char* name, const std::string& default_value = "");
    int get_env_int(const char* name, int default_value);
    float get_env_float(const char* name, float default_value);
    bool get_env_bool(const char* name, bool default_value);
    std::vector<std::string> get_env_list(const char* name);
    std::map<std::string, int> get_env_json_map(const char* name);
};

} // namespace sip
```

```cpp
// src/utils/config.cpp
#include "sip/utils/config.hpp"
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace sip {

SipConfig& SipConfig::instance() {
    static SipConfig config;
    return config;
}

void SipConfig::load_from_env() {
    // SIP Account Settings
    sip_user = get_env("SIP_USER", "user");
    sip_login = get_env("SIP_LOGIN", sip_user);  // Defaults to sip_user
    sip_domain = get_env("SIP_DOMAIN", "sip.linphone.org");
    sip_password = get_env("SIP_PASSWORD", "password");

    auto caller_id = get_env("SIP_CALLER_ID");
    if (!caller_id.empty()) {
        sip_caller_id = caller_id;
    }

    sip_port = get_env_int("SIP_PORT", 5060);
    sip_use_tcp = get_env_bool("SIP_USE_TCP", true);
    sip_use_ice = get_env_bool("SIP_USE_ICE", false);
    sip_stun_servers = get_env_list("SIP_STUN_SERVERS");
    sip_proxy_servers = get_env_list("SIP_PROXY_SERVERS");

    // Audio Settings
    sip_null_device = get_env_bool("SIP_NULL_DEVICE", true);

    std::string base_audio_dir = get_env("SIP_AUDIO_DIR", ".");
    tmp_audio_dir = get_env("SIP_AUDIO_TMP_DIR", base_audio_dir + "/tmp");
    sip_audio_dir = get_env("SIP_AUDIO_WAV_DIR", base_audio_dir + "/wav");

    frame_time_usec = get_env_int("FRAME_TIME_USEC", 60000);
    codecs_priority = get_env_json_map("CODECS_PRIORITY");
    if (codecs_priority.empty()) {
        // Default codec priorities
        codecs_priority = {{"opus/48000", 254}, {"G722/16000", 253}};
    }
    record_audio_parts = get_env_bool("RECORD_AUDIO_PARTS", false);

    // PJSIP Settings
    ua_zero_thread_cnt = get_env_bool("UA_ZERO_THREAD_CNT", true);
    ua_main_thread_only = get_env_bool("UA_MAIN_THREAD_ONLY", true);
    ec_tail_len = get_env_int("EC_TAIL_LEN", 200);
    ec_no_vad = get_env_bool("EC_NO_VAD", false);
    pjsip_log_level = get_env_int("PJSIP_LOG_LEVEL", 1);

    // VAD Settings
    vad_model_path = get_env("VAD_MODEL_PATH", "./silero_vad.onnx");
    vad_model_url = get_env("VAD_MODEL_URL",
        "https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx");
    vad_sampling_rate = get_env_int("VAD_SAMPLING_RATE", 16000);
    vad_threshold = get_env_float("VAD_THRESHOLD", 0.65f);
    vad_min_speech_duration_ms = get_env_int("VAD_MIN_SPEECH_DURATION_MS", 150);
    vad_min_silence_duration_ms = get_env_int("VAD_MIN_SILENCE_DURATION_MS", 300);
    vad_speech_pad_ms = get_env_int("VAD_SPEECH_PAD_MS", 700);
    vad_speech_prob_window = get_env_int("VAD_SPEECH_PROB_WINDOW", 3);
    vad_correction_debug = get_env_bool("VAD_CORRECTION_DEBUG", false);
    vad_correction_enter_thres = get_env_float("VAD_CORRECTION_ENTER_THRESHOLD", 0.6f);
    vad_correction_exit_thres = get_env_float("VAD_CORRECTION_EXIT_THRESHOLD", 0.4f);

    // Timing Settings
    events_delay = get_env_float("EVENTS_DELAY", 0.010f);
    async_delay = get_env_float("ASYNC_DELAY", 0.005f);
    short_pause_offset_ms = get_env_int("SHORT_PAUSE_OFFSET_MS", 200);
    long_pause_offset_ms = get_env_int("LONG_PAUSE_OFFSET_MS", 850);
    user_silence_timeout_ms = get_env_int("USER_SILENCE_TIMEOUT_MS", 60000);
    greeting_delay_sec = get_env_float("GREETING_DELAY_SEC", 0.0f);

    // Logging Settings
    log_level = get_env("LOG_LEVEL", "INFO");
    auto log_fn = get_env("LOG_FILENAME");
    if (!log_fn.empty()) {
        // Add timestamp to log filename (matching Python behavior)
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");

        std::filesystem::path p(log_fn);
        std::string stem = p.stem().string();
        std::string ext = p.extension().string();
        log_filename = stem + "_" + ss.str() + ext;
    }

    auto ld = get_env("LOGS_DIR");
    if (!ld.empty()) {
        logs_dir = ld;
        if (log_filename) {
            log_filename = (*logs_dir / *log_filename).string();
        }
    }

    // REST API Settings
    sip_rest_api_port = get_env_int("SIP_REST_API_PORT", 8000);
    call_connection_timeout = get_env_int("CALL_CONNECTION_TIMEOUT", 10);
    backend_url = get_env("BACKEND_URL", "http://localhost:8080");
    interruptions_are_allowed = get_env_bool("INTERRUPTIONS_ARE_ALLOWED", true);

    // Local STT Settings
    use_local_stt = get_env_bool("USE_LOCAL_STT", false);
    local_stt_url = get_env("LOCAL_STT_URL");
    local_stt_lang = get_env("LOCAL_STT_LANG", "en");
}

void SipConfig::validate() const {
    if (sip_user.empty()) throw std::runtime_error("SIP_USER is required");
    if (sip_domain.empty()) throw std::runtime_error("SIP_DOMAIN is required");
    if (sip_password.empty()) throw std::runtime_error("SIP_PASSWORD is required");
    if (backend_url.empty()) throw std::runtime_error("BACKEND_URL is required");

    if (!std::filesystem::exists(vad_model_path)) {
        throw std::runtime_error("VAD model not found: " + vad_model_path.string());
    }
}

// Helper method implementations
std::string SipConfig::get_env(const char* name, const std::string& default_value) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : default_value;
}

int SipConfig::get_env_int(const char* name, int default_value) {
    const char* value = std::getenv(name);
    return value ? std::stoi(value) : default_value;
}

float SipConfig::get_env_float(const char* name, float default_value) {
    const char* value = std::getenv(name);
    return value ? std::stof(value) : default_value;
}

bool SipConfig::get_env_bool(const char* name, bool default_value) {
    const char* value = std::getenv(name);
    if (!value) return default_value;

    std::string str_value(value);
    std::transform(str_value.begin(), str_value.end(), str_value.begin(), ::tolower);
    return str_value == "true" || str_value == "1" || str_value == "yes";
}

std::vector<std::string> SipConfig::get_env_list(const char* name) {
    std::vector<std::string> result;
    const char* value = std::getenv(name);
    if (!value) return result;

    std::string str_value(value);
    std::stringstream ss(str_value);
    std::string item;

    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);

        if (!item.empty()) {
            result.push_back(item);
        }
    }

    return result;
}

std::map<std::string, int> SipConfig::get_env_json_map(const char* name) {
    std::map<std::string, int> result;
    const char* value = std::getenv(name);
    if (!value) return result;

    try {
        auto json_obj = nlohmann::json::parse(value);
        for (auto& [key, val] : json_obj.items()) {
            result[key] = val.get<int>();
        }
    } catch (const std::exception& e) {
        // Log error but don't fail - return empty map
        spdlog::error("Failed to parse JSON from {}: {}", name, e.what());
    }

    return result;
}

} // namespace sip
```

### Usage Example

The same `.env` file works for both Python and C++ versions:

```bash
# .env - works for both Python and C++ implementations
SIP_USER=myuser
SIP_DOMAIN=sip.example.com
SIP_PASSWORD=secret123
SIP_CALLER_ID="AI Assistant"
SIP_PORT=5060
SIP_USE_TCP=True

VAD_THRESHOLD=0.7
VAD_MIN_SPEECH_DURATION_MS=200
LONG_PAUSE_OFFSET_MS=900

BACKEND_URL=http://localhost:8001
LOG_LEVEL=DEBUG
```

### Phase 1: Core Infrastructure (Weeks 1-2)

**Status**: Completed in repo (build scaffolding, env config with dotenv override, logging, scripts, Docker).

**Current state note (build flow)**:
- The current build is two-phase: build/install pjproject, then re-configure to pick up pkg-config output. It works but is not optimal.
- **TODO (later)**: refactor CMake to a single-configure flow by wiring pjproject include/lib paths directly or generating the .pc file at configure time.

#### 1.1 Build System Setup

- **CMake Configuration**
  - Create `CMakeLists.txt` with C++17 standard
  - Configure PJSIP library linking
  - Set up ONNX Runtime C++ API
  - Configure compiler flags for optimization
  - Docker multi-stage build for deployment

- **Directory Structure**
  ```
  .
  ├── CMakeLists.txt
  ├── Dockerfile
  ├── include/
  │   └── sip_gateway/
  │       ├── backend/
  │       │   ├── client.hpp
  │       │   └── ws_client.hpp
  │       ├── server/
  │       │   └── rest_server.hpp
  │       ├── audio/
  │       │   ├── player.hpp
  │       │   ├── recorder.hpp
  │       │   └── port.hpp
  │       ├── sip/
  │       │   ├── app.hpp
  │       │   ├── account.hpp
  │       │   └── call.hpp
  │       ├── utils/
  │       │   └── async.hpp
  │       ├── config.hpp
  │       └── logging.hpp
  ├── src/
  │   ├── main.cpp
  │   ├── config.cpp
  │   ├── logging.cpp
  │   ├── backend/
  │   │   ├── client.cpp
  │   │   └── ws_client.cpp
  │   ├── server/
  │   │   └── rest_server.cpp
  │   ├── audio/
  │   │   ├── player.cpp
  │   │   ├── recorder.cpp
  │   │   └── port.cpp
  │   ├── sip/
  │   │   ├── app.cpp
  │   │   ├── account.cpp
  │   │   └── call.cpp
  │   └── utils/
  │       └── async.cpp
  ├── docs/
  └── python/
  ```

- **Dependencies Management**

  **Build from Source (ExternalProject)**:
  - **pjproject 2.16** - SIP protocol stack ([releases](https://github.com/pjsip/pjproject/releases))
    - Built via `ExternalProject` into `build/deps/pjproject`
    - Configure flags set in `cmake/Dependencies.cmake` (WebRTC AEC, SRTP, Opus)

  **CMake FetchContent**:
  - **spdlog 1.13.0**
  - **nlohmann/json 3.11.3**
  - **cpp-httplib 0.16.0**
  - **websocketpp 0.8.2**
  - **Asio** (standalone)
  - **Catch2 3.5.4** (when tests land)

  **System Dependencies** (via package manager):
  - **ONNX Runtime** (pending VAD phase)

- **CMakeLists.txt** (Root)

  ```cmake
  cmake_minimum_required(VERSION 3.16)
  project(sip-gateway VERSION 1.0.0 LANGUAGES CXX C)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  # Compiler flags
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O3")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
  endif()

  # Build pjproject from submodule
  set(PJSIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pjproject)

  # Configure and build pjproject
  include(ExternalProject)
  ExternalProject_Add(
      pjproject_build
      SOURCE_DIR ${PJSIP_ROOT}
      CONFIGURE_COMMAND ${PJSIP_ROOT}/configure
          --prefix=${CMAKE_BINARY_DIR}/pjproject
          --disable-sound
          --enable-ssl
          --with-external-srtp
          --enable-epoll
          CFLAGS=-fPIC
          CXXFLAGS=-fPIC
      BUILD_COMMAND make dep && make
      INSTALL_COMMAND make install
      BUILD_IN_SOURCE 1
  )

  # Set pjproject include and library paths
  set(PJSIP_INCLUDE_DIRS
      ${CMAKE_BINARY_DIR}/pjproject/include
      ${PJSIP_ROOT}/pjlib/include
      ${PJSIP_ROOT}/pjlib-util/include
      ${PJSIP_ROOT}/pjnath/include
      ${PJSIP_ROOT}/pjmedia/include
      ${PJSIP_ROOT}/pjsip/include
  )

  set(PJSIP_LIBRARIES
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjsua2.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjsua.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjsip.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjmedia.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjnath.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpjlib-util.a
      ${CMAKE_BINARY_DIR}/pjproject/lib/libpj.a
      pthread
      m
      ssl
      crypto
  )

  # Find other dependencies
  find_package(ONNXRuntime REQUIRED)
  find_package(spdlog REQUIRED)
  find_package(nlohmann_json 3.10.0 REQUIRED)

  # Include directories
  include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${PJSIP_INCLUDE_DIRS}
      ${ONNXRUNTIME_INCLUDE_DIRS}
  )

  # Source files
  file(GLOB_RECURSE SIP_SOURCES "src/*.cpp")

  # Create executable
  add_executable(sip_gateway ${SIP_SOURCES})
  add_dependencies(sip_gateway pjproject_build)

  # Link libraries
  target_link_libraries(sip_gateway
      ${PJSIP_LIBRARIES}
      ${ONNXRUNTIME_LIBRARIES}
      spdlog::spdlog
      nlohmann_json::nlohmann_json
      pthread
      dl
  )

  # Install
  install(TARGETS sip_gateway DESTINATION bin)
  ```

#### 1.2 Configuration System

**File**: `include/sip_gateway/config.hpp`, `src/config.cpp`

- Implement `SipConfig` class with environment variable loading
- Use `std::optional<T>` for optional configuration values
- Implement validation and default values
- Configuration is env-only for now (no JSON config file support)
- Load `.env` from the working directory with override behavior (matches Python)

```cpp
class SipConfig {
public:
    static SipConfig& instance();

    // SIP settings
    std::string sip_user;
    std::string sip_domain;
    std::string sip_password;
    std::optional<std::string> sip_caller_id;
    int sip_port = 5060;
    bool sip_use_tcp = true;

    // VAD settings
    float vad_threshold = 0.65f;
    int vad_min_speech_duration_ms = 150;
    int vad_min_silence_duration_ms = 300;

    // Audio settings
    int frame_time_usec = 60000;
    std::filesystem::path audio_dir;

    void load_from_env();
    void validate() const;
};
```

#### 1.3 Logging Infrastructure

**File**: `include/sip_gateway/logging.hpp`, `src/logging.cpp`

- Integrate spdlog
- Log format uses `msg [key=value ...]` via `with_kv()` helper
- Configure log levels from environment

```cpp
#include <spdlog/spdlog.h>

namespace sip_gateway::logging {
    void init(const Config& config);
    std::shared_ptr<spdlog::logger> get_logger();
    std::string with_kv(const std::string& message, std::initializer_list<KeyValue> items);
}
```

### Phase 2: Audio Processing Core (Weeks 3-4)

**Status**: Implemented audio port/player/recorder scaffolding with PJSIP-aware handlers; pending call integration.

#### 2.1 Audio Port Implementation

**File**: `include/sip_gateway/audio/port.hpp`, `src/audio/port.cpp`

- Inherit from `pj::AudioMediaPort`
- Implement frame callback handling
- Thread-safe audio buffer queue
- Integration with VAD processor

```cpp
class AudioMediaPort : public pj::AudioMediaPort {
public:
    AudioMediaPort(std::weak_ptr<Call> call);
    ~AudioMediaPort() override;

    void onFrameRequested(pj::MediaFrame& frame) override;
    void onFrameReceived(const pj::MediaFrame& frame) override;

private:
    std::weak_ptr<Call> call_;
    std::shared_ptr<spdlog::logger> logger_;
};
```

#### 2.2 Audio Player

**File**: `include/sip_gateway/audio/player.hpp`, `src/audio/player.cpp`

- Smart audio file queue management
- Automatic file cleanup
- Non-blocking playback with callbacks
- Interrupt handling

```cpp
class SmartPlayer {
public:
    SmartPlayer(
        pj::AudioMedia& aud_med,
        std::shared_ptr<CallRecorder> recorder,
        std::function<void()> on_stop_callback = nullptr
    );

    ~SmartPlayer();

    void enqueue(const std::filesystem::path& filename, bool discard_after = false);
    void play();
    void interrupt();
    bool is_active() const;

private:
    struct AudioFile {
        std::filesystem::path filename;
        bool discard_after;
    };

    std::deque<AudioFile> queue_;
    std::unique_ptr<pj::AudioMediaPlayer> current_player_;
    pj::AudioMedia& audio_media_;
    std::shared_ptr<CallRecorder> recorder_;
    std::function<void()> on_stop_callback_;
    std::mutex queue_mutex_;
    std::shared_ptr<spdlog::logger> logger_;

    void play_next();
    void on_eof();
};
```

#### 2.3 Call Recording

**File**: `include/sip_gateway/audio/recorder.hpp`, `src/audio/recorder.cpp`

Use PJSIP's built-in `pj::AudioMediaRecorder` class for WAV file recording. This provides automatic WAV format handling, buffering, and file I/O without needing custom implementation.

**Key Design Decision**: Do NOT implement custom WAV writing. PJSIP's `AudioMediaRecorder` already handles:
- WAV header creation and management
- Audio buffering and file I/O
- Proper format conversion
- Efficient synchronous writes (C++ file I/O is fast enough)

```cpp
// include/sip/audio/recorder.hpp
#pragma once

#include <pjsua2.hpp>
#include <filesystem>
#include <memory>
#include <spdlog/spdlog.h>

namespace sip {

class CallRecorder {
public:
    CallRecorder();
    ~CallRecorder();

    /**
     * Start recording to a WAV file.
     *
     * @param filename Path to output WAV file
     * @param sample_rate Audio sample rate (default: 16000 Hz)
     * @param channels Number of channels (default: 1 = mono)
     * @param bits_per_sample Bits per sample (default: 16)
     */
    void start_recording(
        const std::filesystem::path& filename,
        unsigned sample_rate = 16000,
        unsigned channels = 1,
        unsigned bits_per_sample = 16
    );

    /**
     * Stop recording and close the file.
     * WAV header is automatically updated with correct file size.
     */
    void stop_recording();

    /**
     * Get the recorder to connect to audio media.
     * Use: recorder->startTransmit(audio_media) to start receiving audio.
     */
    pj::AudioMediaRecorder* get_recorder() { return recorder_.get(); }

    bool is_recording() const { return recorder_ != nullptr; }

private:
    std::unique_ptr<pj::AudioMediaRecorder> recorder_;
    std::filesystem::path current_file_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace sip
```

```cpp
// src/audio/recorder.cpp
#include "sip/audio/recorder.hpp"
#include <stdexcept>

namespace sip {

CallRecorder::CallRecorder()
    : logger_(spdlog::get("sip") ? spdlog::get("sip") : spdlog::default_logger())
{
}

CallRecorder::~CallRecorder() {
    stop_recording();
}

void CallRecorder::start_recording(
    const std::filesystem::path& filename,
    unsigned sample_rate,
    unsigned channels,
    unsigned bits_per_sample
) {
    if (recorder_) {
        logger_->warn("Already recording. [current_file=%s]", current_file_.string());
        stop_recording();
    }

    try {
        recorder_ = std::make_unique<pj::AudioMediaRecorder>();
        current_file_ = filename;

        // Ensure directory exists
        auto parent_dir = filename.parent_path();
        if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
            std::filesystem::create_directories(parent_dir);
        }

        // Configure audio format
        pj::MediaFormatAudio format;
        format.type = PJMEDIA_TYPE_AUDIO;
        format.clockRate = sample_rate;
        format.channelCount = channels;
        format.bitsPerSample = bits_per_sample;
        format.frameTimeUsec = 20000;  // 20ms frames

        // Create recorder with WAV format (file_format=0 means WAV)
        // PJSIP handles all WAV header creation and updates automatically
        recorder_->createRecorder(
            filename.string(),
            0,           // file_format: 0 = WAV
            nullptr,     // encoder: nullptr = use default PCM
            0,           // max_size: 0 = unlimited
            format       // Audio format specification
        );

        logger_->info("Started recording. [filename=%s, sample_rate=%u, channels=%u, bits=%u]",
                     filename.string(), sample_rate, channels, bits_per_sample);

    } catch (const pj::Error& e) {
        recorder_.reset();
        logger_->error("Failed to start recording. [filename=%s, error=%s]",
                      filename.string(), e.info());
        throw std::runtime_error("Failed to start audio recording: " + e.info());
    }
}

void CallRecorder::stop_recording() {
    if (!recorder_) {
        return;
    }

    try {
        logger_->info("Stopping recording. [filename=%s]", current_file_.string());

        // Destroy recorder - PJSIP automatically:
        // 1. Flushes all buffered audio data
        // 2. Updates WAV header with correct file size
        // 3. Closes the file properly
        recorder_.reset();

        logger_->debug("Recording stopped. [filename=%s]", current_file_.string());

    } catch (const pj::Error& e) {
        logger_->error("Error stopping recording. [filename=%s, error=%s]",
                      current_file_.string(), e.info());
        recorder_.reset();
    }

    current_file_.clear();
}

} // namespace sip
```

**Usage Example**:

```cpp
// In Call class
class Call : public pj::Call {
public:
    void onCallMediaState(pj::OnCallMediaStateParam& prm) override {
        auto call_info = getInfo();

        if (call_info.state == PJSIP_INV_STATE_CONFIRMED) {
            // Get audio media
            auto audio_media = getAudioMedia(-1);

            // Start recording
            recorder_.start_recording(
                std::filesystem::path("recordings") / (session_id_ + ".wav"),
                16000,  // 16 kHz sample rate
                1,      // Mono
                16      // 16-bit PCM
            );

            // Connect call audio to recorder
            audio_media.startTransmit(*recorder_.get_recorder());

            logger_->info("Call recording started. [session_id=%s]", session_id_);
        }
    }

    void onCallState(pj::OnCallStateParam& prm) override {
        auto call_info = getInfo();

        if (call_info.state == PJSIP_INV_STATE_DISCONNECTED) {
            // Stop recording when call ends
            recorder_.stop_recording();
            logger_->info("Call recording stopped. [session_id=%s]", session_id_);
        }
    }

private:
    CallRecorder recorder_;
    std::string session_id_;
};
```

**Important Notes**:

1. **No Custom WAV Writing Needed**: PJSIP's `AudioMediaRecorder` handles all WAV format details automatically
2. **Automatic Header Updates**: WAV file size in header is updated correctly when recorder is destroyed
3. **RAII Pattern**: Use smart pointers (`std::unique_ptr`) for automatic cleanup
4. **Thread Safety**: PJSIP's recorder is thread-safe for audio writing
5. **File I/O Performance**: C++ synchronous file I/O is fast enough; no async needed
6. **Buffer Management**: PJSIP handles internal buffering efficiently

### Phase 3: VAD System (Weeks 5-6)

#### 3.1 VAD Model Wrapper

**File**: `include/sip/vad/model.hpp`, `src/vad/model.cpp`

- ONNX Runtime C++ API integration
- Thread-safe inference
- Model state management
- Optimized for real-time processing

```cpp
class VADModel {
public:
    VADModel(const std::filesystem::path& model_path, int sampling_rate = 16000);
    ~VADModel();

    struct InferenceResult {
        float speech_probability;
        std::vector<float> state;
    };

    InferenceResult get_speech_prob(
        const std::vector<float>& audio_chunk,
        const std::vector<float>& state = {}
    );

    std::vector<float> initialize_state();

private:
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<const char*> input_names_;
    std::vector<const char*> output_names_;
    int sampling_rate_;
    std::shared_ptr<spdlog::logger> logger_;
};
```

#### 3.2 Streaming VAD Processor

**File**: `include/sip/vad/processor.hpp`, `src/vad/processor.cpp`

- Port Python processor logic to C++
- Use `std::vector<float>` for audio buffers
- Implement pause detection callbacks
- Optimize buffer management

```cpp
class StreamingVADProcessor {
public:
    using SpeechCallback = std::function<void(
        const std::vector<float>& buffer,
        float start_sec,
        float duration_sec
    )>;

    struct Config {
        float threshold = 0.8f;
        int min_speech_duration_ms = 150;
        int min_silence_duration_ms = 300;
        int speech_pad_ms = 500;
        int short_pause_ms = 200;
        int long_pause_ms = 850;
        int user_silence_duration_ms = 60000;
    };

    StreamingVADProcessor(
        std::shared_ptr<VADModel> vad_engine,
        const Config& config
    );

    void process_audio(const std::vector<float>& audio_chunk);
    void finalize();

    void set_on_speech_start(SpeechCallback cb) { on_speech_start_ = std::move(cb); }
    void set_on_speech_end(SpeechCallback cb) { on_speech_end_ = std::move(cb); }
    void set_on_short_pause(SpeechCallback cb) { on_short_pause_ = std::move(cb); }
    void set_on_long_pause(SpeechCallback cb) { on_long_pause_ = std::move(cb); }

    void start_user_silence();
    void cancel_user_salience();

private:
    std::shared_ptr<VADModel> vad_engine_;
    Config config_;

    // Buffers
    std::vector<float> buffer_;
    std::vector<float> speech_buffer_;
    std::vector<float> silence_buffer_;
    std::deque<float> prob_history_;

    // State
    bool active_speech_ = false;
    bool active_long_speech_ = false;
    size_t current_sample_ = 0;

    // Callbacks
    SpeechCallback on_speech_start_;
    SpeechCallback on_speech_end_;
    SpeechCallback on_short_pause_;
    SpeechCallback on_long_pause_;

    std::shared_ptr<spdlog::logger> logger_;

    void process_audio_window(const std::vector<float>& window);
    float get_smoothed_prob(const std::vector<float>& chunk);
};
```

### Phase 4: Call Management (Weeks 7-9)

#### 4.1 Call State Machine

**File**: `include/sip_gateway/sip/call.hpp`, `src/sip/call.cpp`

- Implement call state management
- Audio message queue
- Integration with VAD processor
- WebSocket communication for backend

```cpp
enum class CallState {
    WAIT_FOR_USER,
    SPECULATIVE_GENERATE,
    COMMIT_GENERATE,
    FINISHED,
    HANGED_UP
};

class Call : public pj::Call, public std::enable_shared_from_this<Call> {
public:
    Call(
        std::shared_ptr<Account> account,
        std::shared_ptr<App> app,
        const std::string& user_id,
        const std::string& name,
        int call_id = PJSUA_INVALID_ID
    );

    ~Call() override;

    // PJSIP callbacks
    void onCallState(pj::OnCallStateParam& prm) override;
    void onCallMediaState(pj::OnCallMediaStateParam& prm) override;
    void onCallTransferStatus(pj::OnCallTransferStatusParam& prm) override;

    // Audio processing
    void process_audio_frame(const std::vector<uint8_t>& data);

    // Message queue
    void enqueue_message(const std::string& text);
    void play_message_queue();
    void clear_message_queue();

    // Session management
    std::string session_id() const { return session_id_; }

private:
    std::shared_ptr<App> app_;
    std::string session_id_;
    std::string user_id_;
    std::string name_;

    CallState state_ = CallState::WAIT_FOR_USER;

    // Audio components
    std::unique_ptr<AudioMediaPort> media_port_;
    std::shared_ptr<pj::AudioMedia> audio_media_;
    std::unique_ptr<SmartPlayer> smart_player_;
    std::shared_ptr<CallRecorder> recorder_;

    // VAD processing
    std::unique_ptr<StreamingVADProcessor> vad_processor_;

    // Task management
    std::map<std::string, std::future<void>> tasks_;
    std::mutex tasks_mutex_;

    // Message queue
    std::queue<std::string> message_queue_;
    std::mutex queue_mutex_;
    bool is_playing_ = false;

    std::shared_ptr<spdlog::logger> logger_;

    // Internal methods
    void open_call();
    void close_call();
    void set_state(CallState new_state);

    // VAD callbacks
    void on_user_speech_start(const std::vector<float>& buffer, float start, float duration);
    void on_user_speech_stop(const std::vector<float>& buffer, float start, float duration);
    void on_short_pause(const std::vector<float>& buffer, float start, float duration);
    void on_long_pause(const std::vector<float>& buffer, float start, float duration);

    // Async operations
    void speculative_generate(const std::vector<float>& audio_buffer);
    void commit_generate(const std::vector<float>& audio_buffer);
    std::string transcribe(const std::vector<float>& audio_buffer);

    // WebSocket communication
    void send_to_backend(const std::string& message);
    void handle_backend_message(const std::string& message);
};
```

#### 4.2 Account Management

**File**: `include/sip_gateway/sip/account.hpp`, `src/sip/account.cpp`

```cpp
class Account : public pj::Account {
public:
    Account(std::shared_ptr<App> app);
    ~Account() override;

    void onIncomingCall(pj::OnIncomingCallParam& prm) override;
    void onRegState(pj::OnRegStateParam& prm) override;

    void configure(const pj::AccountConfig& cfg);

private:
    std::shared_ptr<App> app_;
    std::shared_ptr<spdlog::logger> logger_;
};
```

### Phase 5: Application Framework (Weeks 10-11)

#### 5.1 Main Application

**File**: `include/sip_gateway/sip/app.hpp`, `src/sip/app.cpp`

- PJSIP endpoint management
- Event loop integration
- Session tracking
- HTTP/WebSocket server (using existing Python aiohttp or C++ library)

```cpp
class App {
public:
    App(const std::string& backend_url);
    ~App();

    void init();
    void run();
    void quit();
    void destroy();

    // Session management
    std::shared_ptr<Call> create_session(
        const std::string& user_id,
        const std::string& name,
        int call_id = PJSUA_INVALID_ID
    );

    void close_session(const std::string& session_id, const std::string& status = "");

    // PJSIP access
    pj::Endpoint& endpoint() { return *endpoint_; }
    std::shared_ptr<Account> account() { return account_; }
    std::shared_ptr<VADModel> vad_engine() { return vad_engine_; }

private:
    std::string backend_url_;
    std::unique_ptr<pj::Endpoint> endpoint_;
    std::shared_ptr<Account> account_;
    std::shared_ptr<VADModel> vad_engine_;

    std::map<std::string, std::shared_ptr<Call>> sessions_;
    std::mutex sessions_mutex_;

    std::atomic<bool> quitting_{false};

    std::shared_ptr<spdlog::logger> logger_;

    void handle_events();
    void init_pjsip();
    void init_vad();
};
```

#### 5.2 Async Callback System

**File**: `include/sip_gateway/utils/async.hpp`, `src/utils/async.cpp`

- Bridge PJSIP callbacks to async operations
- Thread pool for async tasks
- Event loop integration

```cpp
class AsyncCallbackJob {
public:
    template<typename Func, typename... Args>
    AsyncCallbackJob(Func&& func, Args&&... args) {
        task_ = std::async(
            std::launch::async,
            std::forward<Func>(func),
            std::forward<Args>(args)...
        );
    }

    void submit() {
        // Task already launched in constructor
    }

private:
    std::future<void> task_;
};

class AsyncExecutor {
public:
    static AsyncExecutor& instance();

    template<typename Func, typename... Args>
    auto submit(Func&& func, Args&&... args)
        -> std::future<typename std::result_of<Func(Args...)>::type>;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
};
```

### Phase 6: Backend Integration & REST Server (Weeks 10-12)

#### 6.1 Backend HTTP Client

**File**: `include/sip_gateway/backend/client.hpp`, `src/backend/client.cpp`

Replace Python `ClientBotSession` with C++ HTTP client communicating with backend FastAPI service.

```cpp
class BackendClient {
public:
    BackendClient(const std::string& base_url);
    ~BackendClient();

    // Session lifecycle
    struct SessionResponse {
        std::string session_id;
        nlohmann::json metadata;
    };

    SessionResponse create_session(
        const std::string& user_id,
        const std::string& name,
        const std::string& bot_type,
        const std::string& conversation_id = ""
    );

    void close_session(
        const std::string& session_id,
        const std::string& status = ""
    );

    // Conversation API
    nlohmann::json start_generation(
        const std::string& session_id,
        const std::string& message
    );

    nlohmann::json commit_generation(
        const std::string& session_id
    );

    nlohmann::json rollback_generation(
        const std::string& session_id
    );

    // TTS/STT
    std::vector<uint8_t> synthesize(
        const std::string& session_id,
        const std::string& text,
        const std::string& format = "wav"
    );

    std::string transcribe(
        const std::vector<uint8_t>& audio_data,
        const std::string& content_type = "audio/wav"
    );

private:
    std::string base_url_;
    std::string auth_token_;
    std::shared_ptr<httplib::Client> http_client_;
    std::shared_ptr<spdlog::logger> logger_;

    nlohmann::json request(
        const std::string& method,
        const std::string& path,
        const nlohmann::json& body = {},
        const httplib::Headers& headers = {}
    );
};
```

#### 6.2 WebSocket Client

**File**: `include/sip_gateway/backend/ws_client.hpp`, `src/backend/ws_client.cpp`

Replace Python WebSocket connection for streaming LLM responses.

```cpp
class WebSocketClient {
public:
    using MessageCallback = std::function<void(const nlohmann::json&)>;

    WebSocketClient(const std::string& ws_url);
    ~WebSocketClient();

    void connect(const std::string& session_id);
    void disconnect();

    void set_on_message(MessageCallback callback);

    bool is_connected() const;

private:
    std::string ws_url_;
    std::unique_ptr<websocketpp::client<websocketpp::config::asio_tls_client>> ws_client_;
    websocketpp::connection_hdl connection_;
    std::atomic<bool> connected_{false};

    MessageCallback on_message_;
    std::shared_ptr<spdlog::logger> logger_;

    void on_message_internal(const std::string& message);
    void on_close();
    void on_error(const std::string& error);
};
```

#### 6.3 REST Server (SIP Control Endpoints)

**File**: `include/sip_gateway/server/rest_server.hpp`, `src/server/rest_server.cpp`

Expose REST endpoints for making/controlling calls (replaces Python `SipAppMixin`).

```cpp
class RestServer {
public:
    RestServer(std::shared_ptr<App> app, int port = 8000);
    ~RestServer();

    void start();
    void stop();

private:
    std::shared_ptr<App> app_;
    int port_;
    crow::SimpleApp crow_app_;
    std::shared_ptr<spdlog::logger> logger_;

    void setup_routes();

    // Route handlers
    crow::response handle_make_call(const crow::request& req);
    crow::response handle_transfer_call(const crow::request& req, const std::string& session_id);
    crow::response handle_health(const crow::request& req);
    crow::response handle_metrics(const crow::request& req);
};
```

**Example Implementation**:

```cpp
void RestServer::setup_routes() {
    // POST /call - Make outbound SIP call
    CROW_ROUTE(crow_app_, "/call")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            return handle_make_call(req);
        });

    // POST /transfer/<session_id> - Transfer active call
    CROW_ROUTE(crow_app_, "/transfer/<string>")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& session_id) {
            return handle_transfer_call(req, session_id);
        });

    // GET /health - Health check
    CROW_ROUTE(crow_app_, "/health")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            return handle_health(req);
        });

    // GET /metrics - Prometheus metrics
    CROW_ROUTE(crow_app_, "/metrics")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            return handle_metrics(req);
        });
}

crow::response RestServer::handle_make_call(const crow::request& req) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string to_uri = body["to_uri"];
        auto env_info = body.value("env_info", nlohmann::json::object());

        logger_->info("Making outbound call. [to_uri=%s]", to_uri.c_str());

        auto session = app_->create_session(to_uri, "");
        // Make call using PJSIP
        // ...

        return crow::response(200, nlohmann::json{
            {"status", "ok"},
            {"session_id", session->session_id()}
        }.dump());

    } catch (const std::exception& e) {
        logger_->error("Failed to make call. [error=%s]", e.what());
        return crow::response(500, nlohmann::json{
            {"status", "error"},
            {"message", e.what()}
        }.dump());
    }
}
```

### Phase 7: Testing & Validation (Weeks 13-14)

**Testing scope must be staged and realistic**:
- Unit tests: pure logic (config parsing, state machine, VAD processing).
- Integration tests: real PJSIP with prerecorded audio and backend mocks.
- Add fixed fixtures in `tests/data/audio/` and mock interfaces for REST/WS.

#### 7.1 Unit Tests

**Directory**: `tests/unit/`

- Test individual components in isolation
- Use Google Test or Catch2
- Mock PJSIP calls
- Validate audio buffer processing
- Test VAD logic

```cpp
// tests/unit/test_vad_processor.cpp
#include <catch2/catch.hpp>
#include "sip/vad/processor.hpp"

TEST_CASE("StreamingVADProcessor detects speech", "[vad]") {
    auto vad_model = std::make_shared<MockVADModel>();
    StreamingVADProcessor::Config config;
    StreamingVADProcessor processor(vad_model, config);

    bool speech_detected = false;
    processor.set_on_speech_start([&](const auto& buf, float start, float dur) {
        speech_detected = true;
    });

    // Feed audio with speech
    std::vector<float> audio = generate_test_audio_with_speech();
    processor.process_audio(audio);

    REQUIRE(speech_detected);
}
```

#### 7.2 Integration Tests

**Directory**: `tests/integration/`

- End-to-end call flow testing
- Real PJSIP endpoint interaction
- VAD with real audio files
- Performance benchmarks

```cpp
// tests/integration/test_call_flow.cpp
TEST_CASE("Complete call flow", "[integration]") {
    auto app = std::make_shared<App>("http://localhost:8000");
    app->init();

    auto call = app->create_session("test_user", "Test User");

    // Simulate incoming audio
    std::vector<float> audio = load_audio_file("test_audio.wav");

    // Process audio frames
    for (size_t i = 0; i < audio.size(); i += 512) {
        std::vector<float> chunk(audio.begin() + i, audio.begin() + i + 512);
        // call->process_audio_frame(chunk);
    }

    // Verify expected behavior
    // ...
}
```

#### 7.3 Performance Benchmarks

**Directory**: `tests/benchmarks/`

- Compare Python vs C++ performance
- Measure latency improvements
- Memory usage profiling
- CPU utilization analysis

```cpp
// tests/benchmarks/bench_vad.cpp
#include <benchmark/benchmark.h>

static void BM_VADProcessing(benchmark::State& state) {
    auto vad = std::make_shared<VADModel>("silero_vad.onnx");
    StreamingVADProcessor processor(vad, {});

    std::vector<float> audio = generate_test_audio(16000); // 1 second

    for (auto _ : state) {
        processor.process_audio(audio);
    }
}
BENCHMARK(BM_VADProcessing);
```

### Phase 8: Deployment & Migration (Weeks 15-16)

## Current State (Codebase Snapshot)

- Build system, config loading, logging, and Docker setup are present.
- REST server module is implemented and wired into SipApp.
- Backend HTTP client + WebSocket client exist and are used for session creation and WS message flow.
- SIP account/call/app classes exist with multi-call support and basic call lifecycle.
- Audio port/player/recorder scaffolding exists, but not fully integrated into call media flow.
- VAD/ONNX, task manager, metrics, and tests are not implemented yet.

## Next Steps

1) Wire SipCall media handling (audio port/player/recorder integration).
2) Add VAD module (ONNX runtime + pause detection) and integrate with audio pipeline.
3) Implement transfer handling and other call controls in REST API.
4) Add metrics (prometheus-cpp or compatible approach) and basic health/diagnostics.
5) Add tests (unit tests for config/logging, integration tests for call flow).

#### 8.1 Build & Packaging

- **CMake Build**
  ```bash
  mkdir build && cd build
  cmake -DCMAKE_BUILD_TYPE=Release ..
  make -j$(nproc)
  make install
  ```

- **Docker Multi-Stage Build**
  ```dockerfile
  # Build stage - Use Ubuntu for C++ development (no Python needed)
  FROM ubuntu:24.04 AS builder

  # Install required build dependencies for C++ compilation
  RUN apt-get update && \
      apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        ca-certificates \
        libasound2-dev \
        libssl-dev \
        libsrtp2-dev \
        libsqlite3-dev \
        libswscale-dev \
        libavformat-dev \
        libavcodec-dev \
        libavutil-dev \
        wget \
        curl \
        git && \
      rm -rf /var/lib/apt/lists/*

  # Build Opus codec (static library)
  ENV OPUS_VERSION=1.5.2
  WORKDIR /tmp
  RUN wget https://github.com/xiph/opus/releases/download/v${OPUS_VERSION}/opus-${OPUS_VERSION}.tar.gz && \
      tar xzf opus-${OPUS_VERSION}.tar.gz && \
      cd opus-${OPUS_VERSION} && \
      CFLAGS="-fPIC -O2" ./configure --prefix=/usr/local --enable-static --disable-shared && \
      make -j$(nproc) && \
      make install && \
      cd .. && \
      rm -rf opus-${OPUS_VERSION} opus-${OPUS_VERSION}.tar.gz

  # Build pjproject with Opus and WebRTC AEC3 support
  ENV PJPROJECT_VERSION=2.16
  WORKDIR /tmp
  RUN wget -O pjproject-${PJPROJECT_VERSION}.tar.gz \
      https://github.com/pjsip/pjproject/archive/refs/tags/${PJPROJECT_VERSION}.tar.gz && \
      tar -xzf pjproject-${PJPROJECT_VERSION}.tar.gz && \
      mv pjproject-${PJPROJECT_VERSION} pjproject && \
      cd pjproject

  # Configure pjproject with custom config_site.h
  WORKDIR /tmp/pjproject
  RUN mkdir -p pjlib/include/pj && \
      echo "#define PJMEDIA_HAS_OPUS_CODEC 1" > pjlib/include/pj/config_site.h && \
      echo "#define PJMEDIA_EXTERNAL_LIB_OPUS 1" >> pjlib/include/pj/config_site.h && \
      echo "#define PJMEDIA_HAS_WEBRTC_AEC3 1" >> pjlib/include/pj/config_site.h

  # Build pjproject with exact flags from existing build
  ENV CFLAGS="-fPIC -O2" \
      CXXFLAGS="-fPIC -O2 -std=c++17" \
      LDFLAGS="-L/usr/local/lib" \
      LIBS="-lopus"

  RUN ./configure \
      --enable-opus \
      --enable-libwebrtc-aec3 \
      --disable-shared \
      --prefix=/usr/local && \
      make dep -j$(nproc) && \
      make -j$(nproc) && \
      make install

  # Download ONNX Runtime (for VAD)
  WORKDIR /tmp
  ENV ONNXRUNTIME_VERSION=1.23.2
  RUN wget https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
      tar -xzf onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
      cp -r onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}/include/* /usr/local/include/ && \
      cp -r onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}/lib/* /usr/local/lib/ && \
      rm -rf onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}*

  # Copy C++ SIP service source code
  COPY src/integrations/sip/cpp /app/sip
  WORKDIR /app/sip

  # Build C++ SIP service
  RUN mkdir build && cd build && \
      cmake -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_STANDARD=17 \
            -DCMAKE_PREFIX_PATH=/usr/local \
            .. && \
      make -j$(nproc)

  # Strip binaries to reduce final image size
  RUN find /usr/local -type f -exec strip --strip-unneeded {} + 2>/dev/null || true && \
      strip /app/sip/build/sip-service

  # Runtime stage
  FROM python:3.11.8-slim

  # Install runtime dependencies only
  RUN apt-get update && \
      apt-get install -y --no-install-recommends \
        libasound2 \
        ffmpeg \
        libsndfile1 \
        curl && \
      rm -rf /var/lib/apt/lists/*

  # Copy compiled libraries and binary from builder
  COPY --from=builder /usr/local /usr/local
  COPY --from=builder /app/sip/build/sip-service /usr/local/bin/

  # Download VAD model
  WORKDIR /opt/models
  RUN curl -L -o silero_vad.onnx \
      https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx

  # Create directories for audio and logs
  RUN mkdir -p /app/audio/tmp /app/audio/wav /app/logs

  # Set default environment variables
  ENV VAD_MODEL_PATH=/opt/models/silero_vad.onnx \
      SIP_AUDIO_TMP_DIR=/app/audio/tmp \
      SIP_AUDIO_WAV_DIR=/app/audio/wav \
      LOG_LEVEL=INFO \
      PJSIP_LOG_LEVEL=1

  EXPOSE 8000 5060/udp 5060/tcp

  CMD ["/usr/local/bin/sip-service"]
  ```

- **docker-compose.yml** (for local development)
  ```yaml
  version: '3.8'

  services:
    sip-gateway:
      build:
        context: .
        dockerfile: Dockerfile
      ports:
        - "8000:8000"      # REST API
        - "5060:5060/udp"  # SIP UDP
        - "5060:5060/tcp"  # SIP TCP
      environment:
        - BACKEND_URL=http://backend:8080
        - SIP_USER=${SIP_USER}
        - SIP_DOMAIN=${SIP_DOMAIN}
        - SIP_PASSWORD=${SIP_PASSWORD}
        - LOG_LEVEL=DEBUG
      volumes:
        - ./audio:/app/audio
        - ./logs:/app/logs
      depends_on:
        - backend
      networks:
        - flametree

    backend:
      image: flametree-backend:latest
      ports:
        - "8080:8080"
      networks:
        - flametree

  networks:
    flametree:
      driver: bridge
  ```

#### 8.2 Deployment Strategy

**C++ as Standalone Microservice** - No gradual Python migration needed, deploy as separate service:

1. **Week 15: Deploy to Dev Environment**
   - Deploy C++ SIP service container
   - Connect to dev backend
   - Run end-to-end integration tests
   - Monitor all metrics

2. **Week 16: Production Deployment**
   - **Option A: Blue-Green Deployment**
     - Deploy C++ service as "green" environment
     - Route 10% traffic to green (C++ service)
     - Route 90% traffic to blue (Python service)
     - Monitor metrics for 24-48 hours
     - Gradually increase: 25% → 50% → 75% → 100%
     - Switch DNS/load balancer to green
     - Keep blue running for 1 week as fallback

   - **Option B: Canary Deployment**
     - Deploy C++ service alongside Python
     - Use feature flag in orchestrator to route specific customers to C++
     - Start with internal testing calls only
     - Expand to 5% → 10% → 25% → 50% → 100% of production traffic
     - Monitor error rates, latency, resource usage
     - Rollback capability via feature flag

3. **Week 17+: Decommission Python Service**
   - After 2 weeks of 100% C++ traffic with no issues
   - Archive Python SIP code to separate repository
   - Update documentation
   - Remove Python SIP containers from infrastructure

#### 8.3 Monitoring & Metrics

- **Performance Metrics**
  - Audio processing latency (P50, P95, P99)
  - VAD inference time
  - Memory usage per call
  - CPU utilization
  - Call setup time

- **Quality Metrics**
  - Call success rate
  - VAD accuracy (false positives/negatives)
  - Audio quality (MOS scores)
  - Backend integration errors

- **Business Metrics**
  - Cost per call (compute resources)
  - Concurrent call capacity
  - Infrastructure cost reduction

## Technical Considerations

### Memory Management

- Use **smart pointers** (`std::unique_ptr`, `std::shared_ptr`) for resource management
- Implement **RAII** for all resources (files, sockets, PJSIP objects)
- Avoid raw `new`/`delete` - use `std::make_unique`/`std::make_shared`
- Pre-allocate audio buffers to avoid allocations in hot path
- Use **object pools** for frequently created/destroyed objects

### Thread Safety

- **PJSIP threads**: All PJSIP callbacks execute in PJSIP worker threads
- **Async tasks**: Use thread pool for CPU-intensive operations
- **Mutex strategy**: Fine-grained locking, avoid holding locks during callbacks
- **Lock-free queues**: Consider for audio frame passing
- **Atomic operations**: For simple state flags

### Error Handling

- Use **exceptions** for exceptional conditions (initialization failures)
- Return **std::optional** or error enums/structs for expected failures
- Log all errors with context (session_id, call_id, etc.)
- Implement **graceful degradation** (fallback to Python on critical errors)

### Performance Optimization

- **Audio buffer processing**: SIMD instructions for NumPy-like operations
- **ONNX Runtime**: Configure optimal thread count, enable graph optimizations
- **Zero-copy**: Minimize audio buffer copies
- **Cache locality**: Arrange data structures for sequential access
- **Profile-guided optimization**: Use `-fprofile-generate`/`-fprofile-use`

### Compatibility

- **C++17 features** to use:
  - `std::optional`, `std::variant`, `std::any`
  - Structured bindings: `auto [prob, state] = vad->get_speech_prob(...)`
  - `std::filesystem` for path operations
  - `if constexpr` for compile-time branching
  - Fold expressions for variadic templates

- **Avoid**:
  - C++20/23 features (portability); do not use `std::expected`
  - Platform-specific code (keep cross-platform)
  - Compiler extensions

### Platform Support

- **Production target**: Linux (Docker).
- **Supported development**: macOS (CLion/CMake) as best-effort.
- Document local setup, Homebrew deps, and platform flags in `docs/dev_setup.md`.

### Logging Policy

- Prefer stdout/stderr in containers; if file logging is required, implement rotation (size/time-based).
- Document operational guidance and log retention in `docs/runbook.md`.

## Risk Mitigation

### Technical Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| PJSIP API incompatibility | High | Extensive testing, maintain Python fallback |
| ONNX Runtime version conflicts | Medium | Pin specific version, test thoroughly |
| Memory leaks in long-running calls | High | Valgrind testing, address sanitizer in CI |
| Thread deadlocks | High | Thread sanitizer, careful lock ordering |
| Performance regression | Medium | Comprehensive benchmarks, gradual rollout |

### Business Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Extended development timeline | Medium | Phased approach, MVP first |
| Service disruption during migration | High | Gradual rollout, instant rollback capability |
| Increased maintenance burden | Low | Good documentation, training |
| Cost of additional development | Medium | ROI analysis, cost-benefit tracking |

## Success Criteria

### Performance

- [ ] Audio processing latency reduced by ≥30%
- [ ] Memory footprint reduced by ≥40%
- [ ] CPU usage reduced by ≥25%
- [ ] Support ≥2x concurrent calls on same hardware

### Quality

- [ ] Call success rate ≥99.9% (matching Python)
- [ ] VAD accuracy ≥95% (matching Python)
- [ ] Zero memory leaks in 24-hour test
- [ ] Zero crashes in 10,000 call test

### Code Quality

- [ ] Critical-path unit coverage with defined acceptance tests
- [ ] Integration tests for representative call flows
- [ ] Documentation for all public APIs
- [ ] Code review by 2+ engineers for all components

### Deployment

- [ ] Successful A/B test with 10% traffic
- [ ] Metrics showing improvement over Python
- [ ] Successful 100% migration
- [ ] Python fallback removed after 30 days

## Timeline Summary

| Phase | Duration | Key Deliverables |
|-------|----------|------------------|
| 1. Core Infrastructure | 2 weeks | Build system, config, logging |
| 2. Audio Processing | 2 weeks | Audio port, player, recorder |
| 3. VAD System | 2 weeks | VAD model, streaming processor |
| 4. Call Management | 3 weeks | Call state machine, session management |
| 5. Application Framework | 2 weeks | Main app, async system, account |
| 6. Backend Integration & REST | 3 weeks | HTTP client, WebSocket client, REST server |
| 7. Testing & Validation | 2 weeks | Unit tests, integration tests, benchmarks |
| 8. Deployment & Migration | 2 weeks | Docker packaging, deployment, monitoring |
| **Total** | **18 weeks** | Full C++ standalone microservice |

## Next Steps

1. **Review & Approval** - Get stakeholder buy-in on migration plan
2. **Team Assignment** - Assign engineers to phases
3. **Environment Setup** - Prepare development environments with dependencies
4. **Kickoff** - Begin Phase 1 (Core Infrastructure)
5. **Weekly Sync** - Review progress, adjust timeline as needed

## References

- PJSIP Documentation: https://www.pjsip.org/docs/latest/pjsip/docs/html/
- ONNX Runtime C++ API: https://onnxruntime.ai/docs/api/c/
- Modern C++ Best Practices: https://github.com/isocpp/CppCoreGuidelines
- pybind11 Documentation: https://pybind11.readthedocs.io/

---

**Document Version**: 1.0
**Last Updated**: 2026-01-03
**Author**: Migration Planning Team
