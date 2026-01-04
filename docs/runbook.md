# Runbook

## Logging Policy
- Prefer stdout/stderr in containers.
- If file logging is required, configure rotation (size/time-based).

## Startup
- Environment variables required: `BACKEND_URL`.
- Health endpoint: `GET /health`.

## Operations
- Metrics to watch: call setup time, VAD latency, CPU, memory.
- Common failure modes and remediation steps: document after REST server lands.

## Log Retention
- Define rotation and retention defaults here.

## Smoke test

```bash
curl -s http://127.0.0.1:8088/health
curl -s -X POST http://127.0.0.1:8088/call \
  -H 'Content-Type: application/json' \
  -d '{"to_uri":"sip:destination@example.com"}'
```

If you receive a `session_id`, you can test transfer (DTMF or SIP URI):

```bash
curl -s -X POST http://127.0.0.1:8088/transfer/<session_id> \
  -H 'Content-Type: application/json' \
  -d '{"to_uri":"dtmf:*1w5555","transfer_delay":1.0}'
```
