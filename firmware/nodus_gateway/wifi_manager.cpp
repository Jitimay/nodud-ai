#include "wifi_manager.h"

#include <WiFi.h>

#include "config.h"
#include "telemetry.h"

namespace wifi_mgr {
namespace {
String s_ssid, s_pass;
uint32_t s_nextAttempt = 0;
uint32_t s_retryDelay = cfg::WIFI_RETRY_MIN_MS;
bool s_wasUp = false;
bool s_timeConfigured = false;
}  // namespace

void begin(const GatewayConfig &c) {
  s_ssid = c.wifiSsid;
  s_pass = c.wifiPass;
  WiFi.persistent(false);  // credentials live in our own NVS namespace
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(("nodus-" + c.deviceId).c_str());
  WiFi.setAutoReconnect(false);  // we reconnect ourselves, with backoff
  WiFi.setSleep(false);          // lower latency; the gateway is mains/battery powered
}

bool ensure() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_wasUp) {
      s_wasUp = true;
      s_retryDelay = cfg::WIFI_RETRY_MIN_MS;
      logf("WIFI", "[OK] connected to '%s', IP %s, signal %d dBm", s_ssid.c_str(), WiFi.localIP().toString().c_str(),
           (int)WiFi.RSSI());
      if (!s_timeConfigured) {  // real time is needed to validate HTTPS certificates
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        s_timeConfigured = true;
      }
    }
    g_tel.wifiUp = true;
    g_tel.wifiRssi = WiFi.RSSI();
    return true;
  }
  if (s_wasUp) {
    logf("WIFI", "[X] connection lost");
    s_wasUp = false;
  }
  g_tel.wifiUp = false;
  if ((int32_t)(millis() - s_nextAttempt) >= 0) {
    logf("WIFI", "connecting to '%s' (next retry in %lus)", s_ssid.c_str(), (unsigned long)(s_retryDelay / 1000));
    WiFi.disconnect();
    WiFi.begin(s_ssid.c_str(), s_pass.isEmpty() ? nullptr : s_pass.c_str());
    s_nextAttempt = millis() + s_retryDelay;
    s_retryDelay = min(s_retryDelay * 2, cfg::WIFI_RETRY_MAX_MS);
  }
  return false;
}

}  // namespace wifi_mgr
