# Environment Compatibility

## Scope
Compatibility means identical environment variable names and precedence rules.
Defaults must match the Python service or be documented as deviations.

## Precedence Rules
- Load `.env` with override enabled (matches Python `dotenv.load_dotenv(override=True)`), so `.env` values override OS environment variables.
- Code defaults apply only when a variable is absent after env and dotenv resolution.
- Derived defaults are computed from other resolved values (for example, `SIP_AUDIO_TMP_DIR` and `SIP_AUDIO_WAV_DIR` use `SIP_AUDIO_DIR` or `os.getcwd()`).

## Identical Behavior
- List variables that match Python behavior exactly (name, precedence, default).
- Initial discovery (from Python sources in this repo):
  - SIP/account: `SIP_USER`, `SIP_LOGIN`, `SIP_DOMAIN`, `SIP_PASSWORD`, `SIP_CALLER_ID`, `SIP_PORT`, `SIP_USE_TCP`, `SIP_USE_ICE`, `SIP_STUN_SERVERS`, `SIP_PROXY_SERVERS`
  - Audio: `SIP_NULL_DEVICE`, `SIP_AUDIO_DIR`, `SIP_AUDIO_TMP_DIR`, `SIP_AUDIO_WAV_DIR`, `FRAME_TIME_USEC`, `CODECS_PRIORITY`, `RECORD_AUDIO_PARTS`
  - Timing: `EVENTS_DELAY`, `ASYNC_DELAY`, `SHORT_PAUSE_OFFSET_MS`, `LONG_PAUSE_OFFSET_MS`, `USER_SILENCE_TIMEOUT_MS`, `GREETING_DELAY_SEC`
  - PJSIP: `UA_ZERO_THREAD_CNT`, `UA_MAIN_THREAD_ONLY`, `EC_TAIL_LEN`, `EC_NO_VAD`, `PJSIP_LOG_LEVEL`
  - Logging: `LOG_LEVEL`, `LOG_FILENAME`, `LOGS_DIR`
  - VAD: `VAD_MODEL_PATH`, `VAD_MODEL_URL`, `VAD_SAMPLING_RATE`, `VAD_THRESHOLD`, `VAD_MIN_SPEECH_DURATION_MS`, `VAD_MIN_SILENCE_DURATION_MS`, `VAD_SPEECH_PAD_MS`, `VAD_SPEECH_PROB_WINDOW`, `VAD_CORRECTION_DEBUG`, `VAD_CORRECTION_ENTER_THRESHOLD`, `VAD_CORRECTION_EXIT_THRESHOLD`
  - Backend/session: `SIP_REST_API_PORT`, `CALL_CONNECTION_TIMEOUT`, `INTERRUPTIONS_ARE_ALLOWED`
  - Local STT: `USE_LOCAL_STT`, `LOCAL_STT_URL`, `LOCAL_STT_LANG`
  - Service/runtime: `FLAMETREE_CALLBACK_URL`, `FLAMETREE_CALLBACK_PORT`, `AUTHORIZATION_TOKEN`, `BACKEND_REQUEST_TIMEOUT`, `BACKEND_CONNECT_TIMEOUT`, `BACKEND_SOCK_READ_TIMEOUT`, `SESSION_TYPE`, `IS_STREAMING`, `SHOW_WAITING_MESSAGES`, `SIP_EARLY_EOC`
  - Additional discovered vars: `BACKEND_URL`, `REWRITE_ROOT`, `VAD_USE_DYNAMIC_CORRECTIONS`, `LOG_NAME`
  - Note: defaults and precedence must be verified against Python behavior before freezing.

## Default Values (from Python)
- `SIP_USER`: `user`
- `SIP_LOGIN`: defaults to `SIP_USER`
- `SIP_DOMAIN`: `sip.linphone.org`
- `SIP_PASSWORD`: `password`
- `SIP_CALLER_ID`: unset
- `SIP_NULL_DEVICE`: `true`
- `SIP_AUDIO_DIR`: defaults to `os.getcwd()`
- `SIP_AUDIO_TMP_DIR`: `${SIP_AUDIO_DIR}/tmp`
- `SIP_AUDIO_WAV_DIR`: `${SIP_AUDIO_DIR}/wav`
- `SIP_PORT`: `5060`
- `SIP_USE_TCP`: `true`
- `SIP_USE_ICE`: `false`
- `SIP_STUN_SERVERS`: empty list
- `SIP_PROXY_SERVERS`: empty list
- `EVENTS_DELAY`: `0.010`
- `ASYNC_DELAY`: `0.005`
- `FRAME_TIME_USEC`: `60000`
- `VAD_MODEL_PATH`: `${PWD}/silero_vad.onnx`
- `VAD_MODEL_URL`: `https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx`
- `UA_ZERO_THREAD_CNT`: `true`
- `UA_MAIN_THREAD_ONLY`: `true`
- `EC_TAIL_LEN`: `200`
- `EC_NO_VAD`: `false`
- `LOG_LEVEL`: `INFO`
- `LOG_FILENAME`: unset (if set, timestamp is appended)
- `LOGS_DIR`: unset
- `PJSIP_LOG_LEVEL`: `1`
- `VAD_SAMPLING_RATE`: `16000`
- `VAD_THRESHOLD`: `0.65`
- `VAD_MIN_SPEECH_DURATION_MS`: `150`
- `VAD_MIN_SILENCE_DURATION_MS`: `300`
- `VAD_SPEECH_PAD_MS`: `700`
- `VAD_SPEECH_PROB_WINDOW`: `3`
- `VAD_CORRECTION_DEBUG`: `false`
- `VAD_CORRECTION_ENTER_THRESHOLD`: `0.6`
- `VAD_CORRECTION_EXIT_THRESHOLD`: `0.4`
- `SHORT_PAUSE_OFFSET_MS`: `200`
- `LONG_PAUSE_OFFSET_MS`: `850`
- `USER_SILENCE_TIMEOUT_MS`: `60000`
- `CALL_CONNECTION_TIMEOUT`: `10`
- `SIP_REST_API_PORT`: `8000`
- `USE_LOCAL_STT`: `false`
- `LOCAL_STT_URL`: empty string
- `LOCAL_STT_LANG`: `en`
- `GREETING_DELAY_SEC`: `0.0`
- `CODECS_PRIORITY`: `{\"opus/48000\":254,\"G722/16000\":253}`
- `INTERRUPTIONS_ARE_ALLOWED`: `true`
- `RECORD_AUDIO_PARTS`: `false`
- `FLAMETREE_CALLBACK_URL`: unset
- `FLAMETREE_CALLBACK_PORT`: `8088`
- `AUTHORIZATION_TOKEN`: unset
- `BACKEND_REQUEST_TIMEOUT`: `60`
- `BACKEND_CONNECT_TIMEOUT`: `60`
- `BACKEND_SOCK_READ_TIMEOUT`: `60`
- `SESSION_TYPE`: `inbound`
- `IS_STREAMING`: `true`
- `SHOW_WAITING_MESSAGES`: `false`
- `SIP_EARLY_EOC`: `false`
- `BACKEND_URL`: required (no default; accessed via `os.environ["BACKEND_URL"]`)
- `REWRITE_ROOT`: `true`
- `VAD_USE_DYNAMIC_CORRECTIONS`: `true`
- `LOG_NAME`: defaults to `__name__` in Python; C++ uses `sip_gateway` (intentional deviation, see below).

## Equivalent Behavior
- Variables that differ internally but yield the same external behavior.
- Include rationale and test coverage.

## Intentional Deviations
- `LOG_NAME`: C++ default is `sip_gateway` since there is no module `__name__` equivalent; behavior is otherwise identical when the env var is set.

## Validation Plan
- Source-of-truth references (Python code paths).
- Tests that assert parity (unit/integration).
- Scan for additional `os.environ` usage across Python modules before freezing compatibility.

## Source References (Python)
- `python/sip/sip_config.py`
- `python/sip/sip_mixin.py`
- `python/sip/pjapp.py`
- `python/sip/pjcall.py`
- `python/sip/vad/processor6.py`
- `python/client_bot_base.py`
