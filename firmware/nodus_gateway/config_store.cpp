#include "config_store.h"

#include <LittleFS.h>
#include <Preferences.h>

namespace config_store {
namespace {
const char *kNs = "nodus";
const char *kCaPath = "/ca.pem";

String mask(const String &s) {
  if (s.isEmpty()) return "(not set)";
  if (s.length() <= 6) return "******";
  return s.substring(0, 3) + "..." + " (" + String(s.length()) + " chars)";
}
}  // namespace

GatewayConfig load() {
  Preferences p;
  p.begin(kNs, true);
  GatewayConfig c;
  c.wifiSsid = p.getString("wifi_ssid", "");
  c.wifiPass = p.getString("wifi_pass", "");
  c.apiUrl = p.getString("api_url", "");
  c.deviceId = p.getString("device_id", "");
  c.token = p.getString("token", "");
  c.tlsInsecure = p.getBool("tls_insecure", false);
  p.end();
  return c;
}

bool set(const String &key, const String &value, String &error) {
  Preferences p;
  if (!p.begin(kNs, false)) {
    error = "NVS unavailable";
    return false;
  }
  bool ok = true;
  if (key == "wifi_ssid" || key == "wifi_pass" || key == "device_id" || key == "token") {
    if (key == "token" && value.length() < 24) {
      error = "token must be at least 24 characters (same as backend GATEWAY_TOKENS)";
      ok = false;
    } else {
      p.putString(key.c_str(), value);
    }
  } else if (key == "api_url") {
    String v = value;
    while (v.endsWith("/")) v.remove(v.length() - 1);
    if (!v.startsWith("http://") && !v.startsWith("https://")) {
      error = "api_url must start with http:// or https://";
      ok = false;
    } else {
      p.putString("api_url", v);
    }
  } else if (key == "tls_insecure") {
    p.putBool("tls_insecure", value == "1" || value == "true");
  } else {
    error = "unknown key (wifi_ssid, wifi_pass, api_url, device_id, token, tls_insecure)";
    ok = false;
  }
  p.end();
  return ok;
}

void printMasked(const GatewayConfig &c) {
  Serial.printf("  wifi_ssid    = %s\n", c.wifiSsid.isEmpty() ? "(not set)" : c.wifiSsid.c_str());
  Serial.printf("  wifi_pass    = %s\n", mask(c.wifiPass).c_str());
  Serial.printf("  api_url      = %s\n", c.apiUrl.isEmpty() ? "(not set)" : c.apiUrl.c_str());
  Serial.printf("  device_id    = %s\n", c.deviceId.isEmpty() ? "(not set)" : c.deviceId.c_str());
  Serial.printf("  token        = %s\n", mask(c.token).c_str());
  Serial.printf("  tls_insecure = %s\n", c.tlsInsecure ? "1  (WARNING: certificate not checked)" : "0");
  Serial.printf("  ca.pem       = %s\n", LittleFS.exists(kCaPath) ? "present" : "absent");
}

bool saveCaCert(const String &pem) {
  File f = LittleFS.open(kCaPath, "w");
  if (!f) return false;
  size_t n = f.print(pem);
  f.close();
  return n == pem.length();
}

String loadCaCert() {
  File f = LittleFS.open(kCaPath, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

}  // namespace config_store
