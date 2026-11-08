# Nodus AI — Architecture

## 1. Guiding principle

**The ESP32 is a dumb, reliable pipe. The backend is the brain.**

The gateway must never lose an SMS and never send the same reply twice, even when power, Wi-Fi or
the GSM network drops. All intelligence — language handling, AI, conversation memory, survey logic,
message splitting decisions — lives on the backend where it can be updated without reflashing devices.

```
 Basic phone ──SMS──▶ SIM card / SIM800L ──UART──▶ ESP32 (T-Call) ──HTTPS──▶ FastAPI ──▶ Groq
 Basic phone ◀─SMS─── SIM800L ◀──UART── ESP32 ◀──HTTPS── FastAPI ◀── Groq
```

## 2. Component responsibilities

### SIM800L (GSM modem, 2G only)
- Registers on the 2G GSM network, receives and stores SMS on the SIM, transmits SMS.
- Controlled exclusively via AT commands over UART.
- **Limitation:** 2G only (850/900/1800/1900 MHz). The operator must still run 2G at the install site.

### ESP32 firmware (gateway)
Owns exactly four jobs:

| Job | Detail |
|---|---|
| Inbound capture | Read every SMS from the SIM, persist it to flash, *then* delete it from the SIM |
| Forwarding | POST persisted messages to the backend; retry with backoff until acknowledged |
| Outbound delivery | Fetch replies/outbox items, send via SIM800L, report sent/failed status |
| Health | Keep Wi-Fi and modem alive, publish telemetry (signal, registration, counters, uptime) |

FreeRTOS layout (implemented in `firmware/nodus_gateway/`):

```
 ┌─────────────┐  inbound queue   ┌─────────────┐
 │ modem task  │ ───────────────▶ │  net task   │ ──HTTPS──▶ backend
 │ (owns UART) │ ◀─────────────── │ (Wi-Fi+HTTP)│
 └─────────────┘  outbound queue  └─────────────┘
   (both queues are directories of JSON files in LittleFS)
 loop(): serial console + status LED.  Task watchdog (120 s) on both tasks.
```

- **Only the modem task touches the UART.** The tasks share nothing but the flash queues and atomic
  counters. This removes the most common SIM800L bug class (interleaved AT commands from two places).
- **No blocking HTTP in the modem path.** A slow backend can never cause an incoming SMS to be missed.
- Hardware task watchdog on every task.

### FastAPI backend (central intelligence)
- Authenticates gateway devices.
- Deduplicates messages (idempotency key). (Multipart inbound SMS are reassembled on the gateway,
  which sees the UDH; the backend receives one complete message.)
- Normalizes text, detects language (Kirundi / French / other).
- Maintains per-phone-number conversation context.
- Calls Groq, enforces SMS-sized answers, handles timeouts/rate limits/fallback replies.
- Decides encoding (GSM-7 vs UCS-2) and segment count *before* handing a reply to the gateway.
- Owns the outbox — which later enables backend-initiated SMS (surveys, NGO broadcasts).
- SQLite now via SQLAlchemy + Alembic, so PostgreSQL migration is a connection-string change.

### Groq
- Inference only. API key lives **only** in the backend environment (`GROQ_API_KEY`).
- Model chosen in Phase 4 after listing models available to your account (`GET /openai/v1/models`).

## 3. Key design decisions (and why)

### 3.1 SMS reception: store-then-notify, never direct delivery
`AT+CNMI=2,1` — the modem stores each SMS on the SIM and emits `+CMTI: "SM",<index>`. We do **not**
use direct delivery (`+CMT`), because if the ESP32 is busy or rebooting at that instant the message is
gone. On boot the firmware scans the SIM (`AT+CMGL`) to recover anything that arrived while it was
down — this is the power-loss recovery path.

### 3.2 Delivery guarantee: at-least-once + idempotency
1. Read SMS from SIM → write to flash → delete from SIM.
2. Send to backend with `message_id = hash(device_id, sender, SMSC timestamp, text/part info)`.
3. Delete from flash only after the backend returns success.
4. The backend ignores repeat `message_id`s. Retries are therefore always safe.

Outbound items have backend-assigned IDs; the gateway reports `sent`/`failed` per ID so a reply is
never sent twice after a reboot.

### 3.3 PDU mode, not text mode
Text mode (`AT+CMGF=1`) cannot send concatenated (multipart) SMS and mangles non-ASCII characters.
The production firmware uses **PDU mode** for send and receive so we get:
- proper multipart send with a User Data Header (UDH),
- UDH info on inbound parts so the gateway can reassemble long messages before forwarding,
- correct UCS-2 for characters outside GSM-7.

Implemented in `firmware/nodus_gateway/pdu.cpp`, unit-tested on a PC (`firmware/test/`).

### 3.4 SMS length & encoding (matters for cost)
| Encoding | Single SMS | Per part when multipart |
|---|---|---|
| GSM-7 | 160 chars | 153 chars |
| UCS-2 | 70 chars | 67 chars |

One character outside GSM-7 (e.g. French `ê`, `â`, `î`, `ô`, `û`, lowercase `ç`) switches the
**whole** message to UCS-2, more than halving capacity. The backend will transliterate to GSM-7 by
default (`ê→e`), keeping `é è à ù` which *are* GSM-7, and cap replies at a configurable segment
count (default 3).

### 3.5 Gateway ↔ backend API shape
Designed from day one as **inbox + outbox**, not just request/response:

| Endpoint | Purpose |
|---|---|
| `POST /v1/gateway/messages` | Gateway uploads received SMS (batch). Response may include replies inline (fast path). |
| `GET  /v1/gateway/outbox` | Gateway polls for pending outbound SMS (replies that took long, surveys, broadcasts). |
| `POST /v1/gateway/outbox/{id}/status` | Gateway reports `sent` / `failed` + error. |
| `POST /v1/gateway/heartbeat` | Telemetry: signal, registration, Wi-Fi RSSI, counters, uptime, free heap. |
| `GET  /v1/admin/stats` | Operators (ADMIN_TOKEN): gateways online/offline + telemetry, 24 h counts. |

### 3.6 Security
- HTTPS in production; the ESP32 pins the root CA (ISRG Root X1 for Let's Encrypt), so
  certificate renewals never break gateways.
- Each gateway has its own device token (`Authorization: Bearer …`), stored in ESP32 NVS, not
  compiled into the firmware, revocable on the backend.
- Backend rate-limits per sender number to protect Groq quota and SMS credit from abuse loops
  (e.g. two auto-responders replying to each other).

### 3.7 Connectivity
Wi-Fi is the uplink. GPRS fallback through the SIM800L is possible later but deliberately out of
scope: SIM800L's built-in TLS is outdated and GPRS is slow. It's a Phase 7+ option.

## 4. Failure modes we design for

| Failure | Behaviour |
|---|---|
| Backend down | Messages accumulate in flash; exponential backoff (cap ~5 min); no SMS lost |
| Wi-Fi down | Same as above; reconnect loop with backoff |
| Modem brownout / reset | Detected via `RDY`/URCs; re-init sequence; SIM rescanned |
| Modem unresponsive | PWRKEY / power-cycle via GPIO, then ESP32 restart as last resort |
| ESP32 power loss | Unsent SMS still on SIM or in flash; recovered on boot |
| SIM storage full | Firmware drains SIM continuously; alarm in telemetry if it nears full |
| Poison message | Max attempts per message → marked dead, reported, no endless loop |
| Reply loop / spam | Backend per-number rate limit + daily cap |
