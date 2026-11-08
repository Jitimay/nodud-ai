# Nodus AI — Firmware

| Folder | What it is |
|---|---|
| `diagnostics/modem_check/` | Phase 1 tool: identifies the board, checks modem UART and power, AT terminal |
| `nodus_gateway/` | The gateway firmware |
| `test/pdu_host_test.cpp` | PC unit tests for the SMS PDU encoder/decoder |

## Toolchain (Arduino IDE)

- Arduino IDE 2.x, board package **esp32 by Espressif 3.3.x** (built and tested with 3.3.11)
- Library **ArduinoJson 7** (tested with 7.4.2): Library Manager → "ArduinoJson" by Benoit Blanchon
- Tools menu:
  - **Board:** ESP32 Dev Module
  - **Partition Scheme:** **No OTA (2MB APP/2MB SPIFFS)** ← important: the default 1.2 MB app space is 87% full
  - **Port:** `/dev/ttyUSB0` (or `/dev/ttyACM0`)
- Serial Monitor: **115200** baud, line ending **Carriage return**

Command line (the IDE's bundled CLI) from the repo root:

```bash
CLI=/opt/arduino-ide/resources/app/lib/backend/resources/arduino-cli
$CLI compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota firmware/nodus_gateway
$CLI upload  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota -p /dev/ttyUSB0 firmware/nodus_gateway
$CLI monitor -p /dev/ttyUSB0 -c baudrate=115200
```

PDU tests on the PC:

```bash
g++ -std=c++17 -Wall -Wextra -I firmware/nodus_gateway firmware/test/pdu_host_test.cpp \
    firmware/nodus_gateway/pdu.cpp -o /tmp/pdu_test && /tmp/pdu_test
```

## Order of work

1. **Run `modem_check` first.** It confirms the board revision, the modem's UART, the SIM, the
   network and, above all, that the power supply holds during transmission. Fix any `[X]` it
   reports before flashing the gateway.
2. Flash `nodus_gateway`, configure it over serial (below), send it an SMS from a phone.

## Configuring the gateway (once per board)

Settings are stored in NVS (kept across reflashing). In the Serial Monitor:

```
set wifi_ssid MyNetwork
set wifi_pass MyPassword
set api_url http://192.168.1.50:8000
set device_id tcall-01
set token <the token after "tcall-01:" in backend/.env GATEWAY_TOKENS>
reboot
```

- During development `api_url` is your laptop's LAN IP (`ip -4 addr`), with the backend started
  as `uvicorn app.main:app --host 0.0.0.0`. Laptop and gateway must be on the same Wi-Fi.
- For HTTPS in production see `docs/deployment.md` (command `ca`).
- `status` shows modem, signal, Wi-Fi, backend and queue state. `config` shows settings (masked).

## LED (GPIO 13)

| LED | Meaning |
|---|---|
| fast blink | modem not ready (no answer, SIM problem, re-initialising) |
| slow blink | modem OK, backend unreachable (Wi-Fi down, server down, wrong token) |
| solid | everything working |

## Files in `nodus_gateway/`

| File | Purpose |
|---|---|
| `nodus_gateway.ino` | Boot: watchdog, storage, starts the two tasks, console loop, LED |
| `config.h` | Pins (verified against LilyGO), timeouts, queue limits |
| `gsm_manager.*` | **Modem task**, the only code touching the SIM800L: PDU-mode receive, store-then-delete, multipart reassembly, sending, health, recovery |
| `pdu.*` | SMS PDU encode/decode: GSM-7 / UCS-2, multipart UDH. Plain C++, tested on PC |
| `message_queue.*` | Flash queues (LittleFS), atomic writes, survive power loss |
| `api_client.*` | **Network task**: upload SMS, receive replies, report delivery, poll outbox, heartbeat, backoff |
| `wifi_manager.*` | Wi-Fi connect/reconnect with backoff, NTP time (for HTTPS) |
| `config_store.*` | Settings in NVS, CA certificate |
| `console.*` | Serial commands |
| `telemetry.*` | Shared counters (atomic) and thread-safe logging |

## How a message flows

```
SIM800L  --+CMTI-->  modem task: AT+CMGL (PDU) -> decode -> /in/<id>.json -> AT+CMGD (delete from SIM)
net task: /in/<id>.json -> POST /v1/gateway/messages -> reply -> /out/<id>.json -> delete /in file
modem task: /out/<id>.json -> encode PDU(s) -> AT+CMGS -> /st/<id>.json -> delete /out file
net task: /st/<id>.json -> POST /v1/gateway/outbox/<id>/status -> delete /st file
```

Each step deletes its input only after its output is safely written, so a power cut at any
moment loses nothing. Worst case after a crash at the wrong moment: a reply is sent twice.

## Recovery behaviour

| Problem | What the firmware does |
|---|---|
| Modem stops answering (3 timeouts) | Power-cycles the modem, re-initialises |
| Modem rebooted (brownout) | Detected via lost `AT+CMEE=2` setting or `RDY`; re-initialises |
| Modem fails to init 5 times | Restarts the ESP32 |
| A task hangs > 120 s | Hardware watchdog restarts the ESP32 |
| Wi-Fi lost | Reconnects with backoff 5 s → 60 s |
| Backend down / errors | Queues locally, retries with backoff 5 s → 5 min |
| Inbound queue full (200) | Leaves SMS on the SIM until there is room |
| Long SMS part missing | Forwards what arrived after 10 minutes, marking the gap `[...]` |
| Backend rejects a message (400/422) | Drops that message (it would never succeed), counts it as failed |
