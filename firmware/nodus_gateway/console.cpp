#include "console.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "config_store.h"
#include "message_queue.h"
#include "telemetry.h"

namespace console {
namespace {

String s_line;
bool s_caMode = false;
String s_ca;

void help() {
  Serial.println(
      "\nNodus AI gateway console\n"
      "  status                 modem, network, backend, counters\n"
      "  config                 show settings (secrets masked)\n"
      "  set <key> <value>      keys: wifi_ssid wifi_pass api_url device_id token tls_insecure\n"
      "                         use quotes to keep spaces: set wifi_ssid \"My Wifi \"\n"
      "  ca                     paste a PEM CA certificate for https (ends at END CERTIFICATE)\n"
      "  reboot                 restart (needed after changing settings)\n"
      "Example:\n"
      "  set wifi_ssid MyNetwork\n"
      "  set wifi_pass MyPassword\n"
      "  set api_url http://192.168.1.50:8000\n"
      "  set device_id tcall-01\n"
      "  set token <token from backend/.env GATEWAY_TOKENS>\n"
      "  reboot");
}

void status() {
  int creg = g_tel.creg;
  Serial.printf("\n  firmware   %s, uptime %llus, free heap %u bytes\n", NODUS_FW_VERSION,
                (unsigned long long)uptimeSeconds(), (unsigned)ESP.getFreeHeap());
  Serial.printf("  modem      %s, CSQ %d (%d dBm), CREG %d (%s), operator '%s'\n",
                g_tel.modemReady ? "ready" : "NOT READY", (int)g_tel.csq,
                g_tel.csq == 99 ? 0 : -113 + 2 * (int)g_tel.csq, creg,
                creg == 1 ? "home" : creg == 5 ? "roaming" : "not registered", g_tel.getOperator().c_str());
  Serial.printf("  power      modem supply %d mV, modem resets %lu\n", (int)g_tel.supplyMv,
                (unsigned long)g_tel.modemResets);
  Serial.printf("  SIM        %d/%d SMS slots used\n", (int)g_tel.simUsed, (int)g_tel.simTotal);
  Serial.printf("  wifi       %s, RSSI %d dBm\n", g_tel.wifiUp ? WiFi.localIP().toString().c_str() : "down",
                (int)g_tel.wifiRssi);
  Serial.printf("  backend    %s, HTTP errors %lu\n", g_tel.backendOk ? "OK" : "unreachable",
                (unsigned long)g_tel.httpErrors);
  Serial.printf("  SMS        received %lu, sent %lu, failed %lu, undecodable %lu\n",
                (unsigned long)g_tel.smsReceived, (unsigned long)g_tel.smsSent, (unsigned long)g_tel.smsFailed,
                (unsigned long)g_tel.smsUndecodable);
  Serial.printf("  queues     to upload %d, to send %d, to report %d, multipart pending %d\n",
                store::count(store::Q::In), store::count(store::Q::Out), store::count(store::Q::Status),
                (int)store::partKeys().size());
}

void handle(String line) {
  if (s_caMode) {
    s_ca += line + "\n";
    if (line.indexOf("END CERTIFICATE") >= 0) {
      s_caMode = false;
      Serial.println(config_store::saveCaCert(s_ca) ? "  CA certificate saved. reboot to apply." : "  save FAILED");
      s_ca = "";
    }
    return;
  }
  line.trim();
  if (line.isEmpty()) return;
  if (line == "help" || line == "?") help();
  else if (line == "status") status();
  else if (line == "config") config_store::printMasked(config_store::load());
  else if (line == "reboot") {
    Serial.println("  rebooting...");
    delay(200);
    ESP.restart();
  } else if (line == "ca") {
    s_caMode = true;
    s_ca = "";
    Serial.println("  paste the PEM certificate now:");
  } else if (line.startsWith("set ")) {
    String rest = line.substring(4);
    rest.trim();
    int sp = rest.indexOf(' ');
    String key = sp < 0 ? rest : rest.substring(0, sp);
    String value = sp < 0 ? "" : rest.substring(sp + 1);  // value may contain spaces (SSIDs)
    // "quoted values" keep leading/trailing spaces exactly (some SSIDs end with a space).
    if (value.length() >= 2 && value.startsWith("\"") && value.endsWith("\""))
      value = value.substring(1, value.length() - 1);
    String err;
    if (config_store::set(key, value, err)) Serial.printf("  %s saved. type 'reboot' to apply.\n", key.c_str());
    else Serial.printf("  error: %s\n", err.c_str());
  } else {
    Serial.println("  unknown command, type 'help'");
  }
}

}  // namespace

void poll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_line.length() || s_caMode) handle(s_line);
      s_line = "";
    } else if (s_line.length() < 512) {
      s_line += c;
    }
  }
}

}  // namespace console
