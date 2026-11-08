// Compile-time configuration: pins and timing. Secrets and site settings
// (Wi-Fi, backend URL, token) are NOT here: they live in NVS and are set over
// the serial console (type "help").
#pragma once

#include <cstdint>

#define NODUS_FW_VERSION "0.1.0"

// LilyGO T-Call SIM800L pins (LilyGO utilities.h; same on all three SIM800L
// revisions: 20190610 IP5306, 20200327 AXP192, 20200811 IP5306).
namespace pins {
constexpr int MODEM_RST = 5;
constexpr int MODEM_PWRKEY = 4;
constexpr int MODEM_POWER_ON = 23;
constexpr int MODEM_TX = 27;  // ESP32 TX -> SIM800L RX
constexpr int MODEM_RX = 26;  // ESP32 RX <- SIM800L TX
constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;
constexpr int LED = 13;
}  // namespace pins

namespace cfg {
constexpr uint32_t MODEM_BAUD = 115200;
constexpr uint32_t MODEM_RX_BUFFER = 4096;

constexpr uint32_t SIM_SCAN_INTERVAL_MS = 60 * 1000;     // safety net if a +CMTI is missed
constexpr uint32_t MODEM_HEALTH_INTERVAL_MS = 30 * 1000;
constexpr uint32_t SMS_SEND_TIMEOUT_MS = 60 * 1000;
constexpr uint32_t MULTIPART_TIMEOUT_MS = 10 * 60 * 1000;  // forward incomplete long SMS after this
constexpr int MODEM_MAX_CONSECUTIVE_TIMEOUTS = 3;          // then power-cycle the modem
constexpr int MODEM_MAX_FAILED_INITS = 5;                  // then restart the ESP32

constexpr uint32_t HTTP_TIMEOUT_MS = 30 * 1000;  // backend calls Groq (backend timeout 20 s)
constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 8 * 1000;
constexpr uint32_t OUTBOX_POLL_MS = 30 * 1000;
constexpr uint32_t HEARTBEAT_MS = 60 * 1000;
constexpr uint32_t BACKOFF_MIN_MS = 5 * 1000;
constexpr uint32_t BACKOFF_MAX_MS = 5 * 60 * 1000;
constexpr uint32_t WIFI_RETRY_MIN_MS = 5 * 1000;
constexpr uint32_t WIFI_RETRY_MAX_MS = 60 * 1000;

constexpr int MAX_INBOUND_FILES = 200;  // when full, SMS stay on the SIM (back-pressure)
constexpr int MAX_OUTBOUND_FILES = 100;

constexpr uint32_t TASK_WDT_TIMEOUT_MS = 120 * 1000;  // > SMS send timeout and HTTP timeout
}  // namespace cfg
