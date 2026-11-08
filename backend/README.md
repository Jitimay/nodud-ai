# Nodus AI — Backend

FastAPI service that receives SMS from ESP32 gateways, answers with Groq, and hands
replies back for sending. Python 3.12+ (tested on 3.14).

## Setup

```bash
cd backend
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env      # then fill in GROQ_API_KEY, GATEWAY_TOKENS and ADMIN_TOKEN
chmod 600 .env
```

The database schema is managed by Alembic and upgraded automatically at startup.
After changing models: `.venv/bin/alembic revision --autogenerate -m "describe change"`.

## Run

```bash
.venv/bin/uvicorn app.main:app --reload            # local only (127.0.0.1:8000)
.venv/bin/uvicorn app.main:app --host 0.0.0.0      # reachable by the ESP32 on your LAN
```
Interactive API docs: http://127.0.0.1:8000/docs

## Test

```bash
.venv/bin/python -m pytest -q                       # unit + API tests, fake AI, no network
scripts/simulate_sms.sh "+25779123456" "Bonjour, comment conserver les haricots ?"   # real Groq
.venv/bin/python scripts/fake_gateway.py "+25779123456" "Bonjour"   # full gateway protocol + admin stats
```

Monitoring:
```bash
curl -s -H "Authorization: Bearer <ADMIN_TOKEN>" http://127.0.0.1:8000/v1/admin/stats | python3 -m json.tool
```

## Files

| Path | Purpose |
|---|---|
| `app/main.py` | App entry point, startup checks, runs migrations |
| `app/core/config.py` | All settings (from `.env`), validated at startup |
| `app/core/security.py` | Gateway bearer-token auth (constant-time compare) |
| `app/core/logging.py` | Log setup; phone numbers masked in logs |
| `app/database/session.py` | SQLAlchemy engine/session; SQLite in WAL mode |
| `app/models/db.py` | Tables: inbound, outbound (outbox), conversation turns, heartbeats |
| `app/models/schemas.py` | JSON contract with the ESP32 |
| `app/api/gateway.py` | `/v1/gateway/messages`, `/outbox`, `/outbox/{id}/status`, `/heartbeat` |
| `app/api/admin.py` | `/v1/admin/stats`: gateways online/offline, telemetry, 24 h counts |
| `migrations/` | Alembic schema migrations (run automatically at startup) |
| `scripts/fake_gateway.py` | Acts as the ESP32 against a running backend (same requests as the firmware) |
| `app/services/message_processor.py` | Pipeline: dedupe, filter, rate-limit, context, AI, outbox |
| `app/services/groq_service.py` | Groq call, timeouts/retries, error mapping |
| `app/services/sms_text.py` | Markdown cleanup, GSM-7 transliteration, segment count, truncation |
| `app/services/phone.py` | Number normalisation (+257), ignore short codes/alphanumeric senders |
| `app/services/language.py` | Kirundi/French/English guess for canned replies |
| `app/services/messages.py` | Canned replies (errors, reset). Kirundi needs native review |
| `app/prompts/system_prompt.txt` | The AI's instructions; edit without touching code |
| `app/prompts/examples.json` | *(optional)* native-speaker Q&A examples, `[{"user": "...", "assistant": "..."}]` |

## SMS commands

| SMS body | Effect |
|---|---|
| `RESET` / `NOUVEAU` / `NEW` / `GUTANGURA` | Clears that number's conversation memory |

## Behaviour summary

- **Duplicates:** same `message_id` is never processed twice; an unconfirmed reply is handed out again.
- **Outbox lease:** an inline reply not confirmed within `OUTBOX_LEASE_S` reappears in `GET /outbox`;
  max `OUTBOX_MAX_ATTEMPTS` attempts, then marked failed. No endless loops.
- **Ignored senders:** anything not a full phone number (operator promos, short codes) gets no reply.
- **Rate limit:** `RATE_LIMIT_PER_HOUR` / `RATE_LIMIT_PER_DAY` per sender.
- **AI failure:** sender gets a short apology instead of silence.
- **Length:** replies cleaned of markdown, transliterated to GSM-7, capped at `SMS_MAX_SEGMENTS`.
