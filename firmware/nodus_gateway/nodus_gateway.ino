// Nodus AI — SMS AI gateway firmware for LilyGO T-Call ESP32 + SIM800L.
//
// Board: "ESP32 Dev Module" (Arduino ESP32 core 3.x), library: ArduinoJson 7.
// Open this folder in the Arduino IDE; every .cpp/.h here is a tab.
//
// Tasks:
//   modem (core 1)  gsm_manager.cpp  SIM800L only: receive/store/send SMS, health, recovery
//   net   (core 0)  api_client.cpp   Wi-Fi + backend: upload SMS, fetch replies, report, heartbeat
//   loop  (core 1)  console.cpp      serial setup console and status LED
// The tasks share nothing but the flash queues (message_queue.cpp) and atomic counters,
// so a slow backend can never make the gateway miss an SMS.
//
// First boot: open Serial Monitor (115200, line ending "Carriage return") and type "help".

#include <esp_task_wdt.h>

#include "api_client.h"
#include "config.h"
#include "config_store.h"
#include "console.h"
#include "gsm_manager.h"
#include "message_queue.h"
#include "telemetry.h"

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power supply too weak)";
    case ESP_RST_EXT: return "reset button";
    default: return "other";
  }
}

// Prints [OK]/[X] for each setting so a missing one is obvious at boot.
static void printSetupChecklist(const GatewayConfig &c) {
  auto line = [](bool ok, const char *what, const String &value) {
    logf("SETUP", "%s %-10s %s", ok ? "[OK]" : "[X] ", what, ok ? value.c_str() : "NOT SET  -> type: help");
  };
  line(c.wifiSsid.length(), "wifi_ssid", "'" + c.wifiSsid + "'");  // quotes reveal trailing spaces
  line(true, "wifi_pass", c.wifiPass.length() ? "(set)" : "(empty: open network)");
  line(c.apiUrl.length(), "api_url", c.apiUrl);
  line(c.deviceId.length(), "device_id", c.deviceId);
  line(c.token.length() >= 24, "token", "(set, " + String(c.token.length()) + " chars)");
}

// State-change messages, an "all OK" banner, and a one-line summary every minute.
static void reportStatus() {
  static bool allOkShown = false;
  static uint32_t lastSummary = 0;
  const int creg = g_tel.creg;
  const bool registered = creg == 1 || creg == 5;
  const bool allOk = g_tel.modemReady && registered && g_tel.wifiUp && g_tel.backendOk;

  if (allOk && !allOkShown) {
    allOkShown = true;
    Serial.println();
    Serial.println("==================================================================");
    Serial.println("  [OK] ALL SYSTEMS GO: modem, SIM, GSM network, Wi-Fi and backend");
    Serial.printf("       operator '%s', signal CSQ %d, supply %d mV\n", g_tel.getOperator().c_str(), (int)g_tel.csq,
                  (int)g_tel.supplyMv);
    Serial.println("  Send an SMS to this SIM's number: the AI reply comes back from it.");
    Serial.println("==================================================================");
    Serial.println();
  } else if (!allOk && allOkShown) {
    allOkShown = false;
    logf("STATE", "[!] gateway degraded: modem %s, network %s, wifi %s, backend %s (SMS are kept and retried)",
         g_tel.modemReady ? "ok" : "DOWN", registered ? "ok" : "DOWN", g_tel.wifiUp ? "ok" : "DOWN",
         g_tel.backendOk ? "ok" : "DOWN");
  }

  if (millis() - lastSummary >= 60000) {
    lastSummary = millis();
    logf("STATE", "modem %s | GSM %s '%s' CSQ %d | %d mV | wifi %s | backend %s | SMS in %lu, replied %lu, failed %lu | queued %d",
         g_tel.modemReady ? "ok" : "DOWN", registered ? "ok" : "--", g_tel.getOperator().c_str(), (int)g_tel.csq,
         (int)g_tel.supplyMv, g_tel.wifiUp ? "ok" : "DOWN", g_tel.backendOk ? "ok" : "DOWN",
         (unsigned long)g_tel.smsReceived, (unsigned long)g_tel.smsSent, (unsigned long)g_tel.smsFailed,
         store::count(store::Q::In) + store::count(store::Q::Out));
  }
}

static void configureWatchdog() {
  esp_task_wdt_config_t wdt = {
      .timeout_ms = cfg::TASK_WDT_TIMEOUT_MS,
      .idle_core_mask = 0,  // watch our tasks, not the idle tasks
      .trigger_panic = true,  // a hung task reboots the gateway
  };
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  logInit();
  pinMode(pins::LED, OUTPUT);
  digitalWrite(pins::LED, LOW);

  logf("BOOT", "Nodus AI gateway %s, last reset: %s", NODUS_FW_VERSION, resetReason());
  configureWatchdog();
  if (!store::begin()) logf("BOOT", "flash storage unavailable: messages will NOT survive power loss");

  GatewayConfig cfg = config_store::load();
  printSetupChecklist(cfg);
  String deviceId = cfg.deviceId.length() ? cfg.deviceId : String("unset");
  gsm::start(deviceId);  // SMS are received and stored even before Wi-Fi is configured

  if (cfg.complete()) {
    net::start(cfg);
  } else {
    logf("BOOT", "[X] setup incomplete: SMS will be stored but not forwarded. Type 'help'.");
  }
}

void loop() {
  console::poll();
  reportStatus();

  // LED: solid = all good; slow blink = modem ok, backend not; fast blink = modem not ready.
  static uint32_t last = 0;
  static bool on = false;
  uint32_t period = !g_tel.modemReady ? 150 : (!g_tel.backendOk ? 1000 : 0);
  if (period == 0) {
    digitalWrite(pins::LED, HIGH);
  } else if (millis() - last >= period) {
    last = millis();
    on = !on;
    digitalWrite(pins::LED, on);
  }
  delay(20);
}
