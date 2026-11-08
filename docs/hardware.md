# Nodus AI — Hardware

Board: **LilyGO TTGO T-Call ESP32 + SIM800L**

## 1. Board revisions

LilyGO made several T-Call variants with different power-management chips and some different pins.
Source of truth: LilyGO's official `utilities.h`
(https://github.com/Xinyuan-LilyGO/LilyGo-T-Call-SIM800/blob/master/examples/Arduino_TinyGSM/utilities.h).

| Variant (LilyGO macro) | Modem | PMU (I2C addr) | RST | PWRKEY | POWER_ON | TX | RX | DTR | RI | LED |
|---|---|---|---|---|---|---|---|---|---|---|
| `SIM800L_IP5306_VERSION_20190610` | SIM800L | IP5306 (0x75) | 5 | 4 | 23 | 27 | 26 | — | — | 13 |
| `SIM800L_AXP192_VERSION_20200327` | SIM800L | AXP192 (0x34) | 5 | 4 | 23 | 27 | 26 | 32 | 33 | 13 |
| `SIM800L_IP5306_VERSION_20200811` | SIM800L | IP5306 (0x75) | 5 | 4 | 23 | 27 | 26 | 32 | 33 | 13 |
| `SIM800C_AXP192_VERSION_20200609` | SIM800C | AXP192 (0x34) | — | 4 | **25** | 27 | 26 | 32 | 33 | **12** |

TX/RX are from the **ESP32's** side: GPIO27 = ESP32 TX → modem RX; GPIO26 = ESP32 RX ← modem TX.
I2C to the PMU: SDA 21, SCL 22.

**What's the same on every SIM800L variant:** RST 5, PWRKEY 4, POWER_ON 23, TX 27, RX 26, LED 13.
**What differs:** the PMU chip (IP5306 vs AXP192) and whether DTR/RI (32/33) are wired.
RI (GPIO33) matters later: it lets the modem wake the ESP32 on incoming SMS.

### How we identify yours
1. **Silkscreen:** look on the PCB (front and back) for a version/date string such as `V1.3`, `V1.4`,
   `20190610`, `20200811`, or `SIM800L IP5306` / `AXP192`.
2. **Chip markings:** the PMU is a small square chip near the battery connector, marked `IP5306` or `AXP192`.
3. **Automatic:** the Phase 1 diagnostic (`firmware/diagnostics/modem_check`) scans I2C and
   reports which PMU answers (0x75 → IP5306, 0x34 → AXP192).

**Status: VERIFIED on 2026-11-08** with `firmware/diagnostics/modem_check`:

| Item | Value |
|---|---|
| Silkscreen revision/date | _not recorded yet_ |
| PMU detected | _not recorded yet (see section [2] of the diagnostic output)_ |
| Modem UART | OK at 115200 baud (autobaud) on GPIO 26/27 |
| SIM | ready, PIN disabled |
| Network | registered (home) in ~9 s, operator 642-08 (Burundi), 2G OK |
| Signal | CSQ 20–23 (about -73 dBm, excellent) |
| Modem supply under load (`AT+CBC`) | 4054–4076 mV during registration, battery 84% |
| Modem resets / under-voltage warnings | 0 / 0 |

## 2. Power — the most common SIM800L failure

- SIM800L supply range: **3.4 V – 4.4 V**. During a 2G transmit burst it draws **up to ~2 A peaks**
  (577 µs bursts). Registration and SMS sending both trigger bursts.
- When the supply sags, the modem browns out and **silently resets**. Typical symptoms:
  - modem silently reboots (loses RAM settings; prints `RDY` only if its baud rate is fixed —
    in the default autobaud mode a reboot is silent, so the diagnostic uses a settings marker),
  - `UNDER-VOLTAGE WARNNING` / `UNDER-VOLTAGE POWER DOWN` URCs (the typo is SIMCom's),
  - network LED blinks fast forever and never settles on slow (3 s) blinks,
  - SMS send hangs or the ESP32 itself resets (reset reason `BROWNOUT`).

### Rules for this board
1. **Connect a lithium battery** (single cell 3.7 V LiPo / 18650, ≥ 1500 mAh recommended) to the
   board's battery connector. The battery absorbs the current peaks; USB alone is often insufficient.
   ⚠️ **Check polarity against the `+`/`-` marks on the PCB before plugging in** — JST connectors on
   batteries from other vendors are frequently wired the opposite way, and reversing it destroys the board.
2. Use a short, thick USB cable and a real 5 V / 2 A supply (not a laptop hub port) for field installs.
3. **Always attach the GSM antenna** before powering the modem. Transmitting without an antenna can
   damage the RF stage and causes registration failure.
4. On IP5306 boards, firmware must set "boost keep-on" (register 0x00 = 0x37, per LilyGO), otherwise
   the IP5306 can shut its output off when load is light while running on battery.
5. We verify power empirically: the diagnostic logs `AT+CBC` (modem-side voltage) during
   registration and watches for brownout URCs. Target: stays **above ~3.6 V** under load.

## 3. SIM card

- Disable the SIM PIN first (put the SIM in any phone → security settings → disable PIN).
- Confirm the SIM can send/receive SMS and has credit/bundle.
- Insert with the power **off**. Check the holder size against your SIM (use an adapter if needed).
- Confirm the operator still runs **2G GSM** at the install site — SIM800L cannot use 3G/4G.

## 4. Linux / serial notes (Arch)

- The board shows up as `/dev/ttyUSB0` (CP210x / CH340 bridge) or `/dev/ttyACM0` (CH9102 on some
  newer units). Run `ls /dev/ttyUSB* /dev/ttyACM*` before and after plugging in.
- Serial devices belong to group `uucp` on Arch — your user is already in it.
- ModemManager would hijack the port by probing AT commands; it is inactive on this machine. If it
  is ever enabled, mask it: `sudo systemctl mask --now ModemManager`.
- The product listing name "LilyGO TTGO T-Call ESP32 SIM800L COM44, R28" contains seller/stock
  codes, not a LilyGO revision. The revision is identified by PCB silkscreen and the PMU I2C scan.

## 5. Libraries / toolchain

| Need | Choice | Why |
|---|---|---|
| Build | **Arduino IDE 2.x**, esp32 core **3.3.11** | what this project is built and tested with |
| Partition scheme | **No OTA (2MB APP/2MB SPIFFS)** | gateway firmware is 1.15 MB: 87% of the default app slot, 54% of this one; also ~1.9 MB for the queue |
| Modem driver | **Own AT layer + PDU codec** (no TinyGSM) | TinyGSM's SMS support is text-mode send only: no PDU, no multipart, no reading. We need all of those |
| JSON | ArduinoJson 7 (tested 7.4.2) | standard, bounded memory |
| Wi-Fi / HTTPS | built-in `WiFi`, `HTTPClient`, `WiFiClientSecure` | no extra deps |
| Persistence | LittleFS (queues) + NVS/Preferences (settings, token) | survives power loss |
| PMU | raw `Wire` writes (IP5306) | one register; AXP192 needs no setup for the modem |

The Phase 1 diagnostic uses **no external libraries**, so a failure there can only be hardware/wiring.
