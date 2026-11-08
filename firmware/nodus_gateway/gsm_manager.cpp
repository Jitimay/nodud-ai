#include "gsm_manager.h"

#include <ArduinoJson.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include <map>
#include <vector>

#include "config.h"
#include "message_queue.h"
#include "pdu.h"
#include "telemetry.h"

namespace gsm {
namespace {

HardwareSerial &ser = Serial1;
String g_deviceId;

enum class AtStatus { Ok, Error, Timeout };
struct AtResult {
  AtStatus status = AtStatus::Timeout;
  std::vector<String> lines;
  String error;
  bool ok() const { return status == AtStatus::Ok; }
};

bool g_smsIndicated = true;  // scan the SIM right after init (recovers SMS received while off)
bool g_rebootSuspected = false;
int g_consecutiveTimeouts = 0;
String g_lineBuf;
std::map<String, uint32_t> g_partFirstSeen;  // multipart key -> millis() first seen this boot
uint8_t g_concatRef = (uint8_t)esp_random();

// ------------------------------------------------------------------ helpers

String hashHex(const String &s) {  // FNV-1a 64-bit
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 1099511628211ULL;
  }
  char buf[17];
  snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return String(buf);
}

// Integer field #idx (comma separated) after "prefix" in the first matching line.
int fieldInt(const std::vector<String> &lines, const char *prefix, int idx, int dflt = INT32_MIN) {
  for (const String &l : lines) {
    if (!l.startsWith(prefix)) continue;
    String rest = l.substring(strlen(prefix));
    rest.trim();
    int field = 0, start = 0;
    for (int i = 0; i <= (int)rest.length(); i++) {
      if (i == (int)rest.length() || rest[i] == ',') {
        if (field == idx) return rest.substring(start, i).toInt();
        field++;
        start = i + 1;
      }
    }
  }
  return dflt;
}

bool isUrc(const String &l) {
  return l.startsWith("+CMTI:") || l == "RDY" || l.startsWith("UNDER-VOLTAGE") || l.startsWith("OVER-VOLTAGE") ||
         l.indexOf("POWER DOWN") >= 0 || l == "Call Ready" || l == "SMS Ready";
}

void handleUrc(const String &l) {
  if (l.startsWith("+CMTI:")) {
    g_smsIndicated = true;
    return;
  }
  if (l == "Call Ready" || l == "SMS Ready") return;
  logf("MODEM", "URC: %s", l.c_str());
  // RDY is only printed when the modem (re)boots with a fixed baud rate; the
  // CMEE marker in health() catches silent reboots in autobaud mode.
  if (l == "RDY" || l.indexOf("POWER DOWN") >= 0) g_rebootSuspected = true;
}

// Feeds one char into the line buffer. Returns true when `out` holds a complete non-empty line.
bool feed(char c, String &out) {
  if (c == '\n') {
    out = g_lineBuf;
    g_lineBuf = "";
    out.trim();
    return out.length() > 0;
  }
  if (c != '\r' && g_lineBuf.length() < 700) g_lineBuf += c;
  return false;
}

bool readLine(String &out, uint32_t deadline) {
  while ((int32_t)(deadline - millis()) > 0) {
    while (ser.available())
      if (feed((char)ser.read(), out)) return true;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

void pollUrcs() {
  String line;
  while (ser.available()) {
    if (!feed((char)ser.read(), line)) continue;
    if (isUrc(line)) handleUrc(line);
    else logf("MODEM", "unsolicited: %s", line.c_str());
  }
}

AtResult at(const String &cmd, uint32_t timeoutMs = 2000) {
  pollUrcs();
  AtResult r;
  ser.print(cmd);
  ser.print("\r");
  const uint32_t deadline = millis() + timeoutMs;
  String line;
  while (readLine(line, deadline)) {
    if (line == "OK") {
      r.status = AtStatus::Ok;
      break;
    }
    if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) {
      r.status = AtStatus::Error;
      r.error = line;
      break;
    }
    if (line == cmd) continue;  // command echo (comes back after a modem reboot)
    if (isUrc(line)) {
      handleUrc(line);
      continue;
    }
    r.lines.push_back(line);
  }
  if (r.status == AtStatus::Timeout) {
    g_consecutiveTimeouts++;
    logf("MODEM", "timeout: %s", cmd.c_str());
  } else {
    g_consecutiveTimeouts = 0;
  }
  return r;
}

AtResult atRetry(const String &cmd, int attempts, uint32_t timeoutMs, uint32_t pauseMs) {
  AtResult r;
  for (int i = 0; i < attempts; i++) {
    r = at(cmd, timeoutMs);
    if (r.ok()) break;
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(pauseMs));
  }
  return r;
}

// ------------------------------------------------------------------ power

void setupPmu() {
  Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
  Wire.beginTransmission(0x75);
  if (Wire.endTransmission() == 0) {
    // IP5306: keep the boost converter on even under light load (LilyGO value).
    Wire.beginTransmission(0x75);
    Wire.write(0x00);
    Wire.write(0x37);
    bool ok = Wire.endTransmission() == 0;
    logf("POWER", "IP5306 found, boost keep-on %s", ok ? "set" : "FAILED");
    return;
  }
  Wire.beginTransmission(0x34);
  if (Wire.endTransmission() == 0) {
    logf("POWER", "AXP192 found (no configuration needed for the modem)");
    return;
  }
  logf("POWER", "no known PMU on I2C (expected IP5306 0x75 or AXP192 0x34)");
}

void powerPinsInit() {
  pinMode(pins::MODEM_RST, OUTPUT);
  digitalWrite(pins::MODEM_RST, HIGH);
  pinMode(pins::MODEM_PWRKEY, OUTPUT);
  digitalWrite(pins::MODEM_PWRKEY, HIGH);
  pinMode(pins::MODEM_POWER_ON, OUTPUT);
  digitalWrite(pins::MODEM_POWER_ON, HIGH);
}

bool probeAt(int tries) {
  for (int i = 0; i < tries; i++) {
    if (at("AT", 300).ok()) return true;
    esp_task_wdt_reset();
  }
  return false;
}

// PWRKEY is a toggle: only pulse it when the modem is known to be off.
void pulsePwrkey() {
  digitalWrite(pins::MODEM_PWRKEY, LOW);
  vTaskDelay(pdMS_TO_TICKS(1100));
  digitalWrite(pins::MODEM_PWRKEY, HIGH);
}

void powerCycle() {
  logf("MODEM", "power-cycling modem");
  digitalWrite(pins::MODEM_POWER_ON, LOW);
  vTaskDelay(pdMS_TO_TICKS(3000));
  esp_task_wdt_reset();
  digitalWrite(pins::MODEM_POWER_ON, HIGH);
  vTaskDelay(pdMS_TO_TICKS(300));
  pulsePwrkey();
  vTaskDelay(pdMS_TO_TICKS(3000));
  g_tel.modemResets++;
}

bool initModem() {
  g_tel.modemReady = false;
  g_consecutiveTimeouts = 0;
  if (!probeAt(5)) {
    logf("MODEM", "no answer: pulsing PWRKEY");
    pulsePwrkey();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_task_wdt_reset();
    if (!probeAt(20)) {
      logf("MODEM", "[X] modem does not answer AT commands (power? battery connected?)");
      return false;
    }
  }
  logf("MODEM", "[OK] modem answers AT commands");
  at("ATE0");
  at("AT+CMEE=2");  // verbose errors; also the reboot marker checked in health()

  bool simReady = false;
  for (int i = 0; i < 10 && !simReady; i++) {
    AtResult r = at("AT+CPIN?", 5000);
    for (const String &l : r.lines) simReady |= l.indexOf("READY") >= 0;
    if (!simReady) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
  if (!simReady) {
    logf("MODEM", "[X] SIM card not ready: is it inserted? is the PIN disabled?");
    return false;
  }
  for (const String &l : at("AT+CCID").lines)
    if (l.length() >= 18) logf("MODEM", "[OK] SIM card ready (ICCID %s)", l.c_str());
  // The SMS subsystem may still be busy right after boot ("SMS Ready" not yet): retry.
  if (!atRetry("AT+CMGF=0", 5, 2000, 2000).ok()) {                                 // PDU mode
    logf("MODEM", "[X] could not set SMS PDU mode");
    return false;
  }
  atRetry("AT+CPMS=\"SM\",\"SM\",\"SM\"", 5, 5000, 2000);                           // SIM storage
  if (!atRetry("AT+CNMI=2,1,0,0,0", 5, 2000, 2000).ok()) {                         // store + notify (+CMTI)
    logf("MODEM", "[X] could not enable SMS notifications");
    return false;
  }
  logf("MODEM", "[OK] SMS receiving enabled (PDU mode, stored on SIM, notify on arrival)");
  at("AT+CSCLK=0");                                                                  // never sleep
  at("AT+COPS=3,0");                                                                 // long operator names
  at("AT+CIURC=0");                                                                  // no "Call Ready" chatter

  g_rebootSuspected = false;
  g_smsIndicated = true;
  g_tel.modemReady = true;
  logf("MODEM", "[OK] modem ready, waiting for network registration...");
  return true;
}

// ------------------------------------------------------------------ health

void health() {
  AtResult m = at("AT+CMEE?");
  int cmee = fieldInt(m.lines, "+CMEE:", 0);
  if (m.ok() && cmee != INT32_MIN && cmee != 2) {
    logf("MODEM", "settings lost: modem REBOOTED (power brownout?) -> re-initialising");
    g_tel.modemResets++;
    g_tel.modemReady = false;
    return;
  }
  g_tel.csq = fieldInt(at("AT+CSQ").lines, "+CSQ:", 0, 99);
  g_tel.creg = fieldInt(at("AT+CREG?").lines, "+CREG:", 1, -1);
  int mv = fieldInt(at("AT+CBC").lines, "+CBC:", 2, 0);
  if (mv > 0) g_tel.supplyMv = mv;
  if (mv > 0 && mv < 3500) logf("POWER", "[!] modem supply LOW: %d mV (connect/charge the battery)", mv);

  for (const String &l : at("AT+COPS?", 5000).lines) {
    int q1 = l.indexOf('"'), q2 = l.lastIndexOf('"');
    if (l.startsWith("+COPS:") && q2 > q1) g_tel.setOperator(l.substring(q1 + 1, q2));
  }
  AtResult s = at("AT+CPMS?");
  for (const String &l : s.lines) {
    if (!l.startsWith("+CPMS:")) continue;
    // +CPMS: "SM",3,30,"SM",3,30,"SM",3,30
    int c1 = l.indexOf(','), c2 = l.indexOf(',', c1 + 1), c3 = l.indexOf(',', c2 + 1);
    if (c1 > 0 && c2 > c1 && c3 > c2) {
      g_tel.simUsed = l.substring(c1 + 1, c2).toInt();
      g_tel.simTotal = l.substring(c2 + 1, c3).toInt();
    }
  }
  // Announce registration changes (and keep reminding while unregistered).
  static int lastCreg = -2;
  int creg = g_tel.creg, csq = g_tel.csq;
  bool registered = creg == 1 || creg == 5;
  bool wasRegistered = lastCreg == 1 || lastCreg == 5;
  if (registered && !wasRegistered) {
    const char *quality = csq == 99 ? "unknown" : csq < 10 ? "POOR" : csq < 15 ? "ok" : csq < 20 ? "good" : "excellent";
    logf("GSM", "[OK] registered on '%s' (%s), signal CSQ %d = %d dBm (%s), supply %d mV",
         g_tel.getOperator().c_str(), creg == 1 ? "home network" : "roaming", csq, csq == 99 ? 0 : -113 + 2 * csq,
         quality, (int)g_tel.supplyMv);
  } else if (!registered) {
    const char *why = creg == 2 ? "searching" : creg == 3 ? "DENIED by the network (SIM blocked? no 2G?)" : "not searching";
    logf("GSM", "%s not registered: %s (CREG %d, CSQ %d)", wasRegistered ? "[X] LOST network," : "[..]", why, creg, csq);
  }
  lastCreg = creg;
}

// ------------------------------------------------------------------ receive

bool queueInbound(const String &sender, const String &text, const String &ts, const String &idExtra) {
  if (store::count(store::Q::In) >= cfg::MAX_INBOUND_FILES) {
    logf("SMS", "inbound queue FULL: leaving SMS on the SIM until the backend is reachable");
    return false;
  }
  String id = hashHex(g_deviceId + "|" + sender + "|" + ts + "|" + text + idExtra);
  JsonDocument doc;
  doc["message_id"] = id;
  doc["sender"] = sender;
  doc["text"] = text;
  if (ts.length() == 25) doc["received_at"] = ts;
  String json;
  serializeJson(doc, json);
  bool already = store::exists(store::Q::In, id);
  if (!store::put(store::Q::In, id, json)) return false;
  if (!already) {
    g_tel.smsReceived++;
    logf("SMS", "<<< received from %s (%u chars): saved, will forward to backend [%s]", maskPhone(sender).c_str(),
         (unsigned)text.length(), id.c_str());
  }
  return true;
}

void assembleParts(const String &key, bool timedOut) {
  std::vector<String> parts = store::loadParts(key);
  if (parts.empty()) return;
  String sender, ts, text;
  int total = 0, expected = 1;
  for (const String &p : parts) {
    JsonDocument d;
    if (deserializeJson(d, p)) continue;
    int seq = d["seq"] | 0;
    total = d["total"] | total;
    if (sender.isEmpty()) {
      sender = d["sender"].as<String>();
      ts = d["ts"].as<String>();
    }
    for (; expected < seq; expected++) text += " [...] ";  // missing part
    text += d["text"].as<String>();
    expected = seq + 1;
  }
  if (timedOut && expected <= total) text += " [...]";
  if (queueInbound(sender, text, ts, "|" + key)) {
    store::removeParts(key);
    g_partFirstSeen.erase(key);
    if (timedOut) logf("SMS", "multipart %s incomplete after timeout: forwarded what arrived", key.c_str());
  }
}

void checkMultipartTimeouts() {
  for (const String &key : store::partKeys()) {
    auto it = g_partFirstSeen.find(key);
    if (it == g_partFirstSeen.end()) g_partFirstSeen[key] = millis();  // e.g. found after a reboot
    else if (millis() - it->second >= cfg::MULTIPART_TIMEOUT_MS) assembleParts(key, true);
  }
}

// Returns true when the SMS is safely handled and may be deleted from the SIM.
bool handleStoredSms(const String &hex) {
  pdu::Deliver d;
  std::string err;
  if (!pdu::decodeDeliver(hex.c_str(), d, &err)) {
    if (err == "not SMS-DELIVER") return true;  // a stored outgoing/status PDU: not for us
    g_tel.smsUndecodable++;
    logf("SMS", "UNDECODABLE PDU (%s), deleting: %s", err.c_str(), hex.c_str());
    return true;
  }
  String sender = d.sender.c_str(), text = d.text.c_str(), ts = d.timestamp.c_str();
  if (!d.concatenated || d.concatTotal <= 1) return queueInbound(sender, text, ts, "");

  String key = hashHex(sender + "|" + String(d.concatRef) + "|" + String(d.concatTotal)).substring(0, 12);
  JsonDocument doc;
  doc["sender"] = sender;
  doc["text"] = text;
  doc["ts"] = ts;
  doc["seq"] = d.concatSeq;
  doc["total"] = d.concatTotal;
  String json;
  serializeJson(doc, json);
  if (!store::putPart(key, d.concatSeq, json)) return false;
  if (!g_partFirstSeen.count(key)) g_partFirstSeen[key] = millis();
  logf("SMS", "part %d/%d of %s from %s", d.concatSeq, d.concatTotal, key.c_str(), maskPhone(sender).c_str());
  if (store::partCount(key) >= d.concatTotal) assembleParts(key, false);
  return true;
}

void scanSim() {
  g_smsIndicated = false;
  AtResult r = at("AT+CMGL=4", 20000);  // 4 = all messages, PDU mode
  if (!r.ok()) return;
  for (size_t i = 0; i + 1 < r.lines.size(); i++) {
    const String &hdr = r.lines[i];
    if (!hdr.startsWith("+CMGL:")) continue;
    int index = hdr.substring(6).toInt();
    const String &hex = r.lines[++i];
    if (!handleStoredSms(hex)) {
      logf("SMS", "could not store SMS at index %d: kept on SIM, will retry", index);
      break;
    }
    if (!at("AT+CMGD=" + String(index), 5000).ok()) logf("SMS", "delete of index %d failed", index);
    esp_task_wdt_reset();
  }
}

// ------------------------------------------------------------------ send

// Sends one PDU part. Returns false with `error` set on failure.
bool sendPdu(const pdu::Submit &part, String &error) {
  pollUrcs();
  ser.printf("AT+CMGS=%d\r", part.tpduLength);
  // Wait for the "> " prompt (not newline-terminated, so watch raw chars).
  uint32_t deadline = millis() + 10000;
  bool prompt = false;
  String line;
  while (!prompt && (int32_t)(deadline - millis()) > 0) {
    while (ser.available()) {
      char c = (char)ser.read();
      if (c == '>') {
        prompt = true;
        break;
      }
      if (feed(c, line)) {
        if (line.indexOf("ERROR") >= 0) {
          error = line;
          return false;
        }
        if (isUrc(line)) handleUrc(line);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!prompt) {
    ser.write(0x1B);  // ESC: abort the pending CMGS
    error = "no > prompt";
    g_consecutiveTimeouts++;
    return false;
  }
  g_lineBuf = "";
  ser.print(part.hex.c_str());
  ser.write(0x1A);  // Ctrl+Z: send

  deadline = millis() + cfg::SMS_SEND_TIMEOUT_MS;
  bool gotRef = false;
  while (readLine(line, deadline)) {
    if (line.startsWith("+CMGS:")) gotRef = true;
    else if (line == "OK" && gotRef) {
      g_consecutiveTimeouts = 0;
      return true;
    } else if (line.indexOf("ERROR") >= 0) {
      error = line;
      return false;
    } else if (isUrc(line)) {
      handleUrc(line);
    }
    esp_task_wdt_reset();
  }
  error = "send timeout";
  g_consecutiveTimeouts++;
  return false;
}

enum class SendResult { Idle, Sent, Wait };

SendResult sendNextOutbound() {
  if (!store::outboundPending) return SendResult::Idle;
  String id, json;
  if (!store::peek(store::Q::Out, id, json)) {
    store::outboundPending = false;
    return SendResult::Idle;
  }
  int creg = g_tel.creg;
  if (creg != 1 && creg != 5) return SendResult::Wait;  // no network: keep it queued

  JsonDocument doc;
  String recipient, text;
  if (!deserializeJson(doc, json)) {
    recipient = doc["recipient"].as<String>();
    text = doc["text"].as<String>();
  }
  String error;
  bool ok = false;
  int nParts = 0;
  if (recipient.isEmpty() || text.isEmpty()) {
    error = "malformed outbound item";
  } else {
    bool ucs2 = false;
    std::vector<pdu::Submit> parts = pdu::encodeSubmit(recipient.c_str(), text.c_str(), g_concatRef++, &ucs2);
    nParts = (int)parts.size();
    ok = true;
    for (const pdu::Submit &p : parts) {
      if (!sendPdu(p, error)) {
        ok = false;
        break;
      }
      esp_task_wdt_reset();
    }
  }

  JsonDocument st;
  st["status"] = ok ? "sent" : "failed";
  if (!ok) st["error"] = error;
  String stJson;
  serializeJson(st, stJson);
  store::put(store::Q::Status, id, stJson);
  store::remove(store::Q::Out, id);
  if (ok) {
    g_tel.smsSent++;
    logf("SMS", ">>> [OK] reply SENT to %s (%d SMS part%s) [%s]", maskPhone(recipient).c_str(), nParts,
         nParts == 1 ? "" : "s", id.c_str());
  } else {
    g_tel.smsFailed++;
    logf("SMS", ">>> [X] reply FAILED to %s: %s (no credit? weak signal?) [%s]", maskPhone(recipient).c_str(),
         error.c_str(), id.c_str());
  }
  return SendResult::Sent;
}

// ------------------------------------------------------------------ task

void modemTask(void *) {
  esp_task_wdt_add(nullptr);
  powerPinsInit();
  setupPmu();
  ser.setRxBufferSize(cfg::MODEM_RX_BUFFER);
  ser.begin(cfg::MODEM_BAUD, SERIAL_8N1, pins::MODEM_RX, pins::MODEM_TX);

  int failedInits = 0;
  uint32_t lastScan = 0, lastHealth = 0, nextSendAllowed = 0;

  for (;;) {
    esp_task_wdt_reset();

    if (!g_tel.modemReady) {
      if (failedInits > 0) powerCycle();
      if (initModem()) {
        failedInits = 0;
        health();
        lastHealth = lastScan = millis();
      } else if (++failedInits >= cfg::MODEM_MAX_FAILED_INITS) {
        logf("MODEM", "modem failed to initialise %d times: restarting ESP32", failedInits);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
      } else {
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
      continue;
    }

    pollUrcs();
    if (g_rebootSuspected) {
      logf("MODEM", "modem reboot detected: re-initialising");
      g_tel.modemResets++;
      g_tel.modemReady = false;
      continue;
    }
    if (g_consecutiveTimeouts >= cfg::MODEM_MAX_CONSECUTIVE_TIMEOUTS) {
      logf("MODEM", "%d consecutive timeouts: modem hung", g_consecutiveTimeouts);
      g_tel.modemReady = false;
      failedInits = 1;  // forces a power-cycle before re-init
      continue;
    }

    const uint32_t now = millis();
    if (g_smsIndicated || now - lastScan >= cfg::SIM_SCAN_INTERVAL_MS) {
      scanSim();
      lastScan = millis();
    }
    if (now - lastHealth >= cfg::MODEM_HEALTH_INTERVAL_MS) {
      health();
      checkMultipartTimeouts();
      lastHealth = millis();
    }
    if ((int32_t)(now - nextSendAllowed) >= 0) {
      SendResult s = sendNextOutbound();
      if (s == SendResult::Wait) nextSendAllowed = millis() + 5000;
      if (s == SendResult::Sent) continue;  // send the next one straight away
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void start(const String &deviceId) {
  g_deviceId = deviceId;
  xTaskCreatePinnedToCore(modemTask, "modem", 10240, nullptr, 3, nullptr, 1);
}

}  // namespace gsm
