#include "api_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

#include <memory>

#include "config.h"
#include "message_queue.h"
#include "telemetry.h"
#include "wifi_manager.h"

namespace net {
namespace {

GatewayConfig s_cfg;
String s_ca;

struct HttpResult {
  int code;
  String body;
};

HttpResult request(const char *method, const String &path, const String *body) {
  const bool https = s_cfg.apiUrl.startsWith("https://");
  std::unique_ptr<WiFiClient> client;
  if (https) {
    auto *sec = new WiFiClientSecure();
    if (s_ca.length()) sec->setCACert(s_ca.c_str());
    else if (s_cfg.tlsInsecure) sec->setInsecure();
    else {
      delete sec;
      return {-100, "https needs a CA certificate (console: ca) or tls_insecure=1 for testing"};
    }
    client.reset(sec);
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  http.setConnectTimeout(cfg::HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout((uint16_t)cfg::HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(*client, s_cfg.apiUrl + path)) return {-101, "bad URL"};
  http.addHeader("Authorization", "Bearer " + s_cfg.token);
  http.addHeader("User-Agent", "nodus-gateway/" NODUS_FW_VERSION);
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, *body);
  } else {
    code = http.sendRequest(method);
  }
  String resp = code > 0 ? http.getString() : HTTPClient::errorToString(code);
  http.end();
  if (code <= 0 || code >= 500) g_tel.httpErrors++;
  return {code, resp};
}

void logHttpFailure(const char *what, const HttpResult &r) {
  if (r.code == 401 || r.code == 403)
    logf("NET", "[X] %s: token REJECTED (%d). Check 'token' and 'device_id' match backend GATEWAY_TOKENS", what, r.code);
  else
    logf("NET", "[X] %s failed: %d %s", what, r.code, r.body.substring(0, 160).c_str());
}

// Queues a reply for sending unless we already have it (queued or sent-but-unreported).
void queueOutbound(JsonVariantConst item) {
  String id = item["id"].as<String>();
  if (id.isEmpty() || store::exists(store::Q::Out, id) || store::exists(store::Q::Status, id)) return;
  if (store::count(store::Q::Out) >= cfg::MAX_OUTBOUND_FILES) {
    logf("NET", "outbound queue full, not accepting %s (backend will re-offer it)", id.c_str());
    return;
  }
  JsonDocument o;
  o["id"] = id;
  o["recipient"] = item["recipient"];
  o["text"] = item["text"];
  String json;
  serializeJson(o, json);
  if (store::put(store::Q::Out, id, json))
    logf("NET", "reply %s queued for %s", id.c_str(), maskPhone(item["recipient"].as<String>()).c_str());
}

enum class Step { Idle, Progress, Fail };

Step uploadOne() {
  String id, json;
  if (!store::peek(store::Q::In, id, json)) return Step::Idle;
  JsonDocument msg;
  if (deserializeJson(msg, json)) {
    logf("NET", "corrupt inbound file %s: dropped", id.c_str());
    store::remove(store::Q::In, id);
    return Step::Progress;
  }
  JsonDocument req;
  req["messages"].add(msg);
  String body;
  serializeJson(req, body);

  HttpResult r = request("POST", "/v1/gateway/messages", &body);
  if (r.code == 200) {
    JsonDocument resp;
    if (deserializeJson(resp, r.body)) {
      logf("NET", "unparseable response for %s: will retry", id.c_str());
      return Step::Fail;
    }
    for (JsonVariantConst res : resp["results"].as<JsonArrayConst>()) {
      logf("NET", "[OK] SMS forwarded to backend: %s%s", res["status"].as<const char *>(),
           res["reply"].isNull() ? " (no reply)" : ", AI reply received");
      if (!res["reply"].isNull()) queueOutbound(res["reply"]);
    }
    store::remove(store::Q::In, id);
    return Step::Progress;
  }
  if (r.code == 400 || r.code == 422) {  // our payload is invalid: retrying will never work
    logf("NET", "backend refused %s (%d): dropped. %s", id.c_str(), r.code, r.body.substring(0, 200).c_str());
    store::remove(store::Q::In, id);
    g_tel.smsFailed++;
    return Step::Progress;
  }
  logHttpFailure("upload", r);
  return Step::Fail;
}

Step reportOne() {
  String id, json;
  if (!store::peek(store::Q::Status, id, json)) return Step::Idle;
  HttpResult r = request("POST", "/v1/gateway/outbox/" + id + "/status", &json);
  if (r.code == 200 || r.code == 404 || r.code == 422) {  // 404: backend doesn't know it, nothing to report
    store::remove(store::Q::Status, id);
    return Step::Progress;
  }
  logHttpFailure("status report", r);
  return Step::Fail;
}

bool pollOutbox() {
  HttpResult r = request("GET", "/v1/gateway/outbox?limit=5", nullptr);
  if (r.code != 200) {
    logHttpFailure("outbox poll", r);
    return false;
  }
  JsonDocument resp;
  if (deserializeJson(resp, r.body)) return false;
  for (JsonVariantConst item : resp["items"].as<JsonArrayConst>()) queueOutbound(item);
  return true;
}

bool heartbeat() {
  JsonDocument hb;
  hb["firmware_version"] = NODUS_FW_VERSION;
  hb["uptime_s"] = (uint32_t)uptimeSeconds();
  hb["gsm_csq"] = (int)g_tel.csq;
  hb["gsm_registered"] = g_tel.creg == 1 || g_tel.creg == 5;
  hb["operator"] = g_tel.getOperator();
  hb["wifi_rssi"] = (int)g_tel.wifiRssi;
  hb["free_heap"] = ESP.getFreeHeap();
  hb["sms_received"] = (uint32_t)g_tel.smsReceived;
  hb["sms_sent"] = (uint32_t)g_tel.smsSent;
  hb["sms_failed"] = (uint32_t)g_tel.smsFailed;
  hb["queue_depth"] = store::count(store::Q::In) + store::count(store::Q::Out);
  hb["modem_supply_mv"] = (int)g_tel.supplyMv;
  String body;
  serializeJson(hb, body);
  HttpResult r = request("POST", "/v1/gateway/heartbeat", &body);
  if (r.code != 200) logHttpFailure("heartbeat", r);
  return r.code == 200;
}

void netTask(void *) {
  esp_task_wdt_add(nullptr);
  wifi_mgr::begin(s_cfg);
  uint32_t nextTry = 0, backoff = cfg::BACKOFF_MIN_MS;
  uint32_t lastOutbox = 0, lastHeartbeat = 0;
  bool first = true;

  for (;;) {
    esp_task_wdt_reset();
    if (!wifi_mgr::ensure()) {
      g_tel.backendOk = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if ((int32_t)(millis() - nextTry) < 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    bool ok = true;
    // Uploads first: they produce replies. Bounded per round so other work still happens.
    for (int i = 0; i < 10 && ok; i++) {
      Step s = uploadOne();
      esp_task_wdt_reset();
      if (s == Step::Idle) break;
      ok = s != Step::Fail;
    }
    for (int i = 0; i < 10 && ok; i++) {
      Step s = reportOne();
      esp_task_wdt_reset();
      if (s == Step::Idle) break;
      ok = s != Step::Fail;
    }
    const uint32_t now = millis();
    if (ok && (first || now - lastOutbox >= cfg::OUTBOX_POLL_MS)) {
      ok = pollOutbox();
      lastOutbox = millis();
    }
    if (ok && (first || now - lastHeartbeat >= cfg::HEARTBEAT_MS)) {
      ok = heartbeat();
      lastHeartbeat = millis();
    }

    if (ok) {
      if (!g_tel.backendOk) logf("NET", "[OK] backend reachable at %s (authenticated as '%s')", s_cfg.apiUrl.c_str(),
                                 s_cfg.deviceId.c_str());
      g_tel.backendOk = true;
      first = false;
      backoff = cfg::BACKOFF_MIN_MS;
      nextTry = millis() + 1000;
    } else {
      g_tel.backendOk = false;
      logf("NET", "[X] backend problem: retrying in %lus (is uvicorn running with --host 0.0.0.0?)",
           (unsigned long)(backoff / 1000));
      nextTry = millis() + backoff;
      backoff = min(backoff * 2, cfg::BACKOFF_MAX_MS);
    }
  }
}

}  // namespace

void start(const GatewayConfig &c) {
  s_cfg = c;
  if (s_cfg.apiUrl.startsWith("https://")) {
    s_ca = config_store::loadCaCert();
    if (s_ca.isEmpty() && !s_cfg.tlsInsecure) logf("NET", "WARNING: https URL but no CA certificate stored");
  }
  // HTTPS (mbedTLS) needs a big stack.
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 2, nullptr, 0);
}

}  // namespace net
