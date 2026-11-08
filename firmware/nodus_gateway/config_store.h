// Site configuration persisted in NVS (survives reflashing the sketch).
// Set over the serial console: "set wifi_ssid MyNetwork", etc.
#pragma once

#include <Arduino.h>

struct GatewayConfig {
  String wifiSsid;
  String wifiPass;
  String apiUrl;    // e.g. http://192.168.1.50:8000  or  https://api.example.org
  String deviceId;  // must match the device_id in the backend's GATEWAY_TOKENS
  String token;     // must match the token in the backend's GATEWAY_TOKENS
  bool tlsInsecure = false;  // https without certificate check: testing ONLY

  bool complete() const { return wifiSsid.length() && apiUrl.length() && deviceId.length() && token.length(); }
};

namespace config_store {
GatewayConfig load();
// Returns false (and fills `error`) for unknown keys or invalid values.
bool set(const String &key, const String &value, String &error);
void printMasked(const GatewayConfig &c);
// CA certificate (PEM) for https, stored in LittleFS at /ca.pem.
bool saveCaCert(const String &pem);
String loadCaCert();
}  // namespace config_store
