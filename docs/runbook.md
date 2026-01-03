# Runbook

## Logging Policy
- Prefer stdout/stderr in containers.
- If file logging is required, configure rotation (size/time-based).

## Startup
- Environment variables required: `BACKEND_URL`.
- Health endpoint: not implemented yet (planned: `GET /health`).

## Operations
- Metrics to watch: call setup time, VAD latency, CPU, memory.
- Common failure modes and remediation steps: document after REST server lands.

## Log Retention
- Define rotation and retention defaults here.
