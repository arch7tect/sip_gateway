# Architecture Overview

## Threading Model
- Threading mode is configured via env: `UA_ZERO_THREAD_CNT` and `UA_MAIN_THREAD_ONLY`.
- PJSIP callbacks must be treated as real-time; keep them short and dispatch work to executors.
- Call state transitions are serialized via a single call-manager executor (one worker thread) to avoid races.
- Audio frame callbacks copy buffers and enqueue processing; heavy work is never done in the PJSIP callback thread.
- Backend REST/WS I/O runs on a separate async executor; callbacks only enqueue requests.

## Execution Modes
- Debug: main-thread-only deterministic mode (`UA_ZERO_THREAD_CNT=true`, `UA_MAIN_THREAD_ONLY=true`).
- Production: PJSIP worker threads + bounded executors (`UA_ZERO_THREAD_CNT=false`, `UA_MAIN_THREAD_ONLY=false`).
- Both modes are supported; behavior is selected by env at runtime.
- Recommended defaults: debug uses main-thread-only; production enables worker threads.

## Core Components
- SIP endpoint layer
- Call/session state machine
- Audio pipeline (play/record/VAD)
- Backend REST/WS client

## Error Boundaries
- What can fail fast vs. requires recovery.
- Escalation paths and fallback behavior.

## Thread Ownership Rules
- PJSIP threads may call `onFrameRequested`/`onFrameReceived` and SIP event callbacks.
- Call state mutations must happen on the call-manager executor only.
- Shared data between callbacks and executors must be protected (mutex or message passing).
