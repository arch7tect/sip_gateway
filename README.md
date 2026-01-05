## Project Overview

This is a C++17 SIP gateway that bridges phone calls to AI backend services via WebSocket. It handles real-time voice conversations with low-latency requirements (<500ms end-to-end), voice activity detection, text-to-speech synthesis, and speculative generation for responsive AI interactions.

## Build & Test Commands

**Initial setup:**
```bash
cmake -S . -B build
```

**Build the project:**
```bash
cmake --build build
```

**Run the executable:**
```bash
./build/sip_gateway
```

**Run tests:**
```bash
./build/sip_gateway_tests
```

**Run specific test:**
```bash
./build/sip_gateway_tests "[test_name]"
```

**Docker build:**
```bash
docker build -t sip-gateway .
```

**Health check:**
```bash
curl http://127.0.0.1:8000/health
```

## Architecture Overview

### System Components

The application orchestrates several subsystems in a multi-threaded architecture:

- **SipApp**: Main orchestrator that runs the PJSIP event loop
- **SipAccount**: Handles SIP registration
- **SipCall**: Per-call handler managing call lifecycle and state machine
- **BackendClient**: HTTP REST client for session management and TTS synthesis
- **BackendWsClient**: WebSocket client for real-time AI response streaming
- **RestServer**: Control API for initiating calls and transfers
- **VadModel**: Shared Silero VAD model for voice activity detection
- **Metrics**: Prometheus metrics collection

### Data Flow

**Inbound call flow:**
1. PJSIP receives call → `SipAccount::onIncomingCall()`
2. Backend session created via HTTP POST to `/session_v2`
3. `SipCall` object created, WebSocket connection established
4. Audio pipeline initialized in `open_media()`
5. Bi-directional audio streaming begins

**Audio processing pipeline (receive):**
```
PJSIP Audio → AudioMediaPort::onFrameReceived()
           → Worker Thread (queued)
           → SipCall::handle_audio_frame()
           → StreamingVadProcessor::process_samples()
           → VAD Events → Transcription + Backend
```

**Audio playback pipeline (send):**
```
Backend WS Message → SipCall::handle_ws_message()
                  → TtsPipeline::enqueue()
                  → Async TTS Synthesis
                  → SmartPlayer::enqueue() + play()
                  → AudioMediaPlayer → Remote Party
```

### Speculative Generation Pattern

This is the key architectural pattern for low-latency AI responses:

- **Short Pause (200ms)**: VAD fires `on_vad_short_pause` → transcribe + send to `/start` → begin speculative generation
- **Long Pause (850ms)**: VAD fires `on_vad_long_pause` → send `/commit` → finalize response
- **User Interrupts**: VAD detects speech → cancel TTS queue → send `/rollback` → return to waiting

**State Machine:**
```
WaitForUser → SpeculativeGenerate → CommitGenerate → Finished
     ↑                ↓ (rollback on interrupt)
     └────────────────┘
```

### TTS Pipeline Architecture

Sophisticated pipeline that parallelizes synthesis while maintaining order:

1. **Enqueue**: Text added to pending queue
2. **Start Synthesis**: Launch async task if under `max_inflight` limit (default 3)
3. **Wait for Ready**: `try_play()` polls for completion
4. **Playback**: Enqueue to `SmartPlayer` when ready

**Cancellation**: Each task has `std::shared_ptr<std::atomic<bool>>` cancel flag checked during synthesis. Completed-but-canceled results are discarded.

### VAD with Dynamic Correction

The `StreamingVadProcessor` includes advanced features:

- **Dynamic threshold adjustment** based on energy profile, SNR, and variance
- **Early detection mode** when AI is speaking (prevents late interrupt detection)
- **State machine** with hysteresis for speech/silence transitions
- **Padding** to avoid clipping speech boundaries

This solves the problem that static thresholds fail across different microphone qualities, noise levels, and network conditions.

### PJSIP Integration

**Endpoint configuration** (`init_pjsip`):
- Default: 0 worker threads, main thread only (`ua_zero_thread_cnt`, `ua_main_thread_only`)
- WebRTC AEC3 echo cancellation with gain control and noise suppression
- Media threading: 1 separate thread (`sip_media_thread_cnt`)

**Custom components extend PJSIP classes:**
- `AudioMediaPort` extends `pj::AudioMediaPort` for frame-based audio bridging
- `SipCall` extends `pj::Call` for call lifecycle management
- `SipAccount` extends `pj::Account` for registration and routing

**Media routing pattern:**
```
Remote Party ↔ AudioMedia
              ↓ startTransmit()
           AudioMediaPort (custom)
              ↓ onFrameReceived()
           Application Processing
```

## Configuration

Environment-driven configuration loaded in `Config::load()`:
- ~75 configurable parameters
- Covers SIP, audio, VAD, backend, logging
- See `src/config.cpp` for all options

Required environment variables:
- `BACKEND_URL`: Backend service endpoint

## Key Dependencies

- **PJSIP 2.16**: SIP protocol stack and audio handling
- **ONNX Runtime 1.23.2**: VAD model inference
- **websocketpp 0.8.2**: WebSocket client
- **cpp-httplib 0.16.0**: HTTP client and REST server
- **nlohmann/json 3.11.3**: JSON parsing
- **spdlog 1.13.0**: Structured logging
- **Catch2 3.5.4**: Testing framework

## Operational Notes

**Metrics endpoint:**
```bash
curl http://127.0.0.1:8000/metrics
```

**Initiate outbound call:**
```bash
curl -X POST http://127.0.0.1:8000/call \
  -H 'Content-Type: application/json' \
  -d '{"to_uri":"sip:destination@example.com"}'
```

**Transfer call:**
```bash
curl -X POST http://127.0.0.1:8000/transfer/<session_id> \
  -H 'Content-Type: application/json' \
  -d '{"to_uri":"dtmf:*1w5555","transfer_delay":1.0}'
```

**Health check returns:**
- `200 OK` with `{"status":"ok"}` if healthy
- Check logs at startup for configuration errors

## Platform Support

- **Production**: Linux (Docker)
- **Development**: macOS (best-effort)

Platform-specific behavior is minimal. Most differences are in dependency building (PJSIP, ONNX Runtime).
