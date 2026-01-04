# Backend API Contract

## Status
- Owner: sip-gateway team
- Version: 0.1 (extracted from Python sources)
- Freeze date: 2025-01-03
- Source of truth: Python implementation in `python/` (see Source References). This document is an extraction.

## REST Endpoints (Backend)
- Session lifecycle:
  - `POST /session` (JSON: `{ "user_id", "name", "type", "conversation_id", "args", "kwargs" }`)
    - Response (expected): `{ "session": { "session_id": "..." }, "greeting"?: "..." }`
  - `POST /session_v2` (multipart: `body` JSON string + `attachments[]`)
    - Response (expected): `{ "session": { "session_id": "..." }, "greeting"?: "..." }`
  - `PUT /session/{id}` (JSON: session updates, supports `conversation_id`)
    - Response: JSON object (not directly consumed by client)
  - `DELETE /session/{id}` (query: `status` optional)
    - Response: JSON object (not directly consumed by client)
- Conversation actions:
  - `POST /session/{id}/run` (JSON: `{ "message", "kwargs" }`)
    - Response: JSON object (not directly consumed by client)
  - `POST /session/{id}/run_v2` (multipart: `message` + `attachments[]`)
    - Response: JSON object (not directly consumed by client)
  - `POST /session/{id}/logs_v2` (multipart: `message`, `role=OPERATOR`)
    - Response: JSON object (not directly consumed by client)
  - `POST /session/{id}/start` (JSON: `{ "message", "kwargs" }`)
    - Response: JSON object (not directly consumed by client)
  - `POST /session/{id}/commit` (JSON: `{}`)
    - Response (expected): `{ "response": "...", "metadata"?: { "SESSION_ENDS"?: true } }`
  - `POST /session/{id}/rollback` (JSON: `{}`)
    - Response: JSON object (not directly consumed by client)
  - `POST /session/{id}/command` (JSON: `{ "command", "args": [] }`)
    - Response (expected): `{ "metadata"?: { ... } }`
- Misc:
  - `POST /waiting_message` (JSON: `{ "session_id" }`)
    - Response: JSON object (not directly consumed by client)
  - `POST /stop_message` (JSON: `{ "session_id" }`)
    - Response: JSON object (not directly consumed by client)
  - `POST /get_message` (JSON: `{ "message_name", "session_id" }`)
    - Response: JSON object (not directly consumed by client)
  - `GET /synthesize` (query: `text`, `format`)
    - Response: audio bytes (binary)
  - `GET /session/{id}/synthesize` (query: `text`, `format`)
    - Response: audio bytes (binary)
  - `POST /transcribe` (body: audio bytes, `Content-Type: audio/*`)
    - Response (expected): JSON string or object containing transcription text
  - `GET /capabilities` (health check)
    - Response: JSON object (truthy for UP)

## REST Endpoints (SIP Service in This Repo)
- `POST /call` (Bearer auth)
  - Request JSON: `{ "to_uri": "...", "env_info": { ... } | null, "communication_id": "..."? }`
  - Response: `{"message":"ok","session_id":"..."}` or `{"message":"..."}` with 500 on failure.
- `POST /transfer/{session_id}` (Bearer auth)
  - Request JSON: `{ "to_uri": "...", "transfer_delay": 1.0? }`
  - Response: `{"status":"ok","message":"Successfully transferred","session_id":"...","to_uri":"..."}`
  - Errors: 400 if call not active, 404 if session missing, 500 otherwise.
- `GET /health`
- `GET /metrics`

## Authentication
- Backend requests use `Authorization: Bearer ${AUTHORIZATION_TOKEN}` when set.
- SIP service protects `/call` and `/transfer/*` via bearer auth middleware.

## WebSocket Contract
- URL: `/ws/{session_id}` (JSON messages).
- Client expects a JSON `type` field; `timeout` and `close` trigger session handlers, others are forwarded to `ws_message`.
- Heartbeat/ping: none in Python. The client does not send pings; it just reads messages until the socket closes.
- Reconnect: client reconnects in a loop with a fixed 5-second delay after disconnect.

## Errors and Retries
- Backend client treats non-2xx as errors; 403 raises `PermissionError`, other statuses raise `RuntimeError`.
- Error responses are expected to include `{ "message": "..." }` when possible.
- Retry semantics: fixed 5-second reconnect loop for WebSocket; REST requests do not retry.

## Versioning
- Backward compatibility policy and deprecation rules: TBD.

## Source References (Python)
- `python/client_bot_base.py`
- `python/sip/sip_mixin.py`
- `python/sip/pjapp.py`
