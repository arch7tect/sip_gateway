# Architecture Overview

## Threading Model
- PJSIP callback threads and ownership rules.
- How state transitions are serialized.
- Interaction between PJSIP threads and async executors.
- Threading mode is configured via env: `UA_ZERO_THREAD_CNT` and `UA_MAIN_THREAD_ONLY`.

## Execution Modes
- Debug: main-thread-only deterministic mode (`UA_ZERO_THREAD_CNT=true`, `UA_MAIN_THREAD_ONLY=true`).
- Production: PJSIP worker threads + bounded executors (`UA_ZERO_THREAD_CNT=false`, `UA_MAIN_THREAD_ONLY=false`).
- Both modes are supported; behavior is selected by env at runtime.

## Core Components
- SIP endpoint layer
- Call/session state machine
- Audio pipeline (play/record/VAD)
- Backend REST/WS client

## Error Boundaries
- What can fail fast vs. requires recovery.
- Escalation paths and fallback behavior.
