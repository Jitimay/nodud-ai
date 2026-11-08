# Nodus AI

AI over plain SMS for basic phones. A LilyGO T-Call (ESP32 + SIM800L) receives SMS, forwards them
to a FastAPI backend that answers with Groq, and sends the reply back by SMS. Built for Kirundi and
French, NGOs, surveys, and communities without smartphones or data.

```
Phone ─SMS─▶ SIM800L ─UART─▶ ESP32 ─HTTPS─▶ FastAPI ─▶ Groq
Phone ◀─SMS─ SIM800L ◀─UART─ ESP32 ◀─HTTPS─ FastAPI ◀─ Groq
```

## Repository layout

| Path | Purpose |
|---|---|
| `firmware/nodus_gateway/` | ESP32 gateway firmware (Arduino IDE, esp32 core 3.3.x) |
| `firmware/diagnostics/modem_check/` | Hardware check: board revision, modem UART, SIM, network, power |
| `firmware/test/` | PC unit tests for the SMS PDU codec |
| `backend/` | FastAPI + Groq service, SQLite (Alembic migrations), tests |
| `deploy/` | systemd unit, Caddy (HTTPS) config, backup script |
| `docs/architecture.md` | System design, delivery guarantees, API, failure modes |
| `docs/hardware.md` | Board revisions, pin map, power requirements, toolchain |
| `docs/deployment.md` | Running the backend on a server with HTTPS, monitoring, backups |
| `docs/installation.md` | Installing a gateway on site, troubleshooting |

## Quick start

1. **Backend** (`backend/README.md`): `cp .env.example .env`, fill it in, `uvicorn app.main:app --host 0.0.0.0`.
2. **Hardware check**: flash `firmware/diagnostics/modem_check`, fix anything marked `[X]`.
3. **Gateway**: flash `firmware/nodus_gateway` (partition **No OTA 2MB/2MB**), configure over serial
   (`firmware/README.md`), text the SIM's number from a phone.

## Status

| Phase | State |
|---|---|
| 1 Hardware verification | **passed** 2026-11-08: UART, SIM, 2G registration, CSQ 20+, power stable (4054+ mV, 0 resets) |
| 2 GSM | implemented (PDU mode, multipart, store-then-delete); PDU codec unit-tested; **awaiting hardware test** |
| 3 Wi-Fi | implemented (reconnect backoff, NTP, HTTPS with CA pinning); **awaiting hardware test** |
| 4 Backend | done: Groq `qwen/qwen3.8-27b`, 25 tests, live-tested |
| 5 Integration | implemented; backend side verified with `scripts/fake_gateway.py`; **awaiting hardware test** |
| 6 Reliability | implemented: flash queues, retries/backoff, watchdog, modem recovery; **awaiting soak test** |
| 7 Production | migrations, admin stats, deploy files and guides done; Kirundi content needs native review |

## Security

The Groq API key never goes into firmware or git. Backend configuration lives in `backend/.env`
(ignored by git); `backend/.env.example` documents the variables. Gateways authenticate with
per-device tokens; production traffic is HTTPS with the root CA pinned on the gateway.
