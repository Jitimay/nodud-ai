// Nodus AI — Phase 1 hardware diagnostic for LilyGO T-Call (SIM800L variants)
//
// What it does, in order:
//   1. Prints ESP32 identity (chip rev, flash, PSRAM, MAC, last reset reason).
//   2. Scans I2C and identifies the power-management chip (IP5306 or AXP192),
//      which tells us which T-Call revision this is.
//   3. Powers the SIM800L using LilyGO's documented sequence.
//   4. Finds the modem's baud rate and verifies AT communication.
//   5. Reads modem identity, SIM status, signal, registration and supply voltage,
//      and watches for brownout symptoms while the modem registers (TX bursts).
//   6. Becomes an interactive AT terminal (type AT commands, or !help).
//
// Pin numbers: LilyGO utilities.h, identical on all three SIM800L T-Call
// variants (20190610 IP5306, 20200327 AXP192, 20200811 IP5306).
// NOT valid for the SIM800C variant (POWER_ON is 25 there).

#include <Arduino.h>
#include <Wire.h>
#include <climits>
#include <esp_system.h>

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

constexpr uint8_t IP5306_ADDR = 0x75;
constexpr uint8_t AXP192_ADDR = 0x34;
constexpr uint8_t IP5306_REG_SYS_CTL0 = 0x00;
constexpr uint8_t IP5306_BOOST_KEEP_ON = 0x37;  // value LilyGO writes

static HardwareSerial &Modem = Serial1;

static bool g_modemSynced = false;
static uint32_t g_modemBaud = 0;
static uint32_t g_modemResets = 0;
static uint32_t g_underVoltage = 0;
static int g_minMv = INT_MAX;
static int g_maxMv = INT_MIN;

// ---------------------------------------------------------------- helpers

static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (normal)";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "OTHER WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP WAKE";
    case ESP_RST_BROWNOUT:  return "BROWNOUT  <-- power supply too weak!";
    case ESP_RST_EXT:       return "EXTERNAL PIN";
    default:                return "UNKNOWN";
  }
}

// Watches modem output for signs of power problems. Only meaningful after the
// first successful sync (before that, boot banners are expected).
// Note: SIM800L prints "RDY" on boot only when its baud rate is fixed (AT+IPR=n).
// In the default autobaud mode a reboot is silent, so the reliable reset
// detector is checkResetMarker() below; "RDY" here is just an early warning.
static void inspectModemText(const String &s) {
  if (!g_modemSynced || s.isEmpty()) return;
  if (s.indexOf("RDY") >= 0) {
    Serial.println("\n[!!] Modem printed RDY -> it just RESET. Almost always a power brownout.");
  }
  if (s.indexOf("UNDER-VOLTAGE") >= 0) {
    g_underVoltage++;
    Serial.println("\n[!!] Modem reports UNDER-VOLTAGE. Supply sagging under TX load.");
  }
  if (s.indexOf("OVER-VOLTAGE") >= 0) {
    Serial.println("\n[!!] Modem reports OVER-VOLTAGE (> 4.4 V). Check supply!");
  }
  if (s.indexOf("NORMAL POWER DOWN") >= 0) {
    Serial.println("\n[!!] Modem powered down.");
  }
}

static void printIndented(const String &resp) {
  String r = resp;
  r.trim();
  if (r.isEmpty()) {
    Serial.println("   (no response)");
    return;
  }
  int start = 0;
  while (start < (int)r.length()) {
    int nl = r.indexOf('\n', start);
    if (nl < 0) nl = r.length();
    String line = r.substring(start, nl);
    line.trim();
    if (!line.isEmpty()) Serial.printf("   %s\n", line.c_str());
    start = nl + 1;
  }
}

// Sends one AT command, waits for OK / ERROR or timeout. Returns the raw response.
static String sendAT(const String &cmd, uint32_t timeoutMs = 2000, bool show = true) {
  String stale;
  while (Modem.available()) stale += (char)Modem.read();
  inspectModemText(stale);

  Modem.print(cmd);
  Modem.print("\r");

  String resp;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (Modem.available()) resp += (char)Modem.read();
    if (resp.endsWith("OK\r\n")) break;
    if (resp.indexOf("ERROR") >= 0 && resp.endsWith("\r\n")) break;
    delay(5);
  }
  inspectModemText(resp);
  if (show) {
    Serial.printf(">> %s\n", cmd.c_str());
    printIndented(resp);
  }
  return resp;
}

// Returns integer field #idx (0-based, comma separated) following `prefix`, or INT_MIN.
static int fieldInt(const String &resp, const char *prefix, int idx) {
  int p = resp.indexOf(prefix);
  if (p < 0) return INT_MIN;
  p += strlen(prefix);
  int end = resp.indexOf('\r', p);
  String line = resp.substring(p, end < 0 ? resp.length() : end);
  line.trim();
  int field = 0, fstart = 0;
  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == ',') {
      if (field == idx) return line.substring(fstart, i).toInt();
      field++;
      fstart = i + 1;
    }
  }
  return INT_MIN;
}

static int readSupplyMv(bool show) {
  int mv = fieldInt(sendAT("AT+CBC", 2000, show), "+CBC:", 2);
  if (mv > 0) {
    g_minMv = min(g_minMv, mv);
    g_maxMv = max(g_maxMv, mv);
  }
  return mv;
}

// Reset detection that works in autobaud mode: AT+CMEE=2 lives only in RAM
// (not saved without AT&W). If AT+CMEE? later reports anything else, the modem
// rebooted in between - on this board that almost always means a brownout.
static void armResetMarker() { sendAT("AT+CMEE=2", 1000, false); }

static bool checkResetMarker() {
  int v = fieldInt(sendAT("AT+CMEE?", 1000, false), "+CMEE:", 0);
  if (v == INT_MIN) return false;  // no answer: can't tell
  if (v != 2) {
    g_modemResets++;
    Serial.println("\n[!!] Modem lost its settings -> it REBOOTED. Almost always a power brownout.");
    sendAT("ATE0", 1000, false);
    armResetMarker();
    return true;
  }
  return false;
}

static const char *cregStr(int stat) {
  switch (stat) {
    case 0: return "not registered, not searching";
    case 1: return "REGISTERED (home)";
    case 2: return "searching...";
    case 3: return "registration DENIED";
    case 4: return "unknown";
    case 5: return "REGISTERED (roaming)";
    default: return "?";
  }
}

static void printSignal(int rssi) {
  if (rssi == INT_MIN) {
    Serial.println("   signal: could not parse");
  } else if (rssi == 99) {
    Serial.println("   signal: unknown / no network (99)");
  } else {
    int dbm = -113 + 2 * rssi;
    const char *q = rssi < 10 ? "POOR" : rssi < 15 ? "OK" : rssi < 20 ? "GOOD" : "EXCELLENT";
    Serial.printf("   signal: CSQ %d = %d dBm (%s)\n", rssi, dbm, q);
  }
}

// ---------------------------------------------------------------- stages

static void printEspInfo() {
  Serial.println("\n=== [1] ESP32 identity ===");
  Serial.printf("   chip: %s rev %d, %d cores, %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("   flash: %lu bytes\n", (unsigned long)ESP.getFlashChipSize());
  Serial.printf("   PSRAM: %lu bytes%s\n", (unsigned long)ESP.getPsramSize(),
                ESP.getPsramSize() == 0 ? " (none detected)" : "");
  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("   MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", (uint8_t)(mac), (uint8_t)(mac >> 8),
                (uint8_t)(mac >> 16), (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  Serial.printf("   last reset: %s\n", resetReasonStr(esp_reset_reason()));
}

static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static int i2cReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

static void detectPmu() {
  Serial.println("\n=== [2] Power-management chip (identifies board revision) ===");
  Wire.begin(pins::I2C_SDA, pins::I2C_SCL);

  Serial.print("   I2C devices found:");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cPresent(a)) {
      Serial.printf(" 0x%02X", a);
      found++;
    }
  }
  Serial.println(found ? "" : " none");

  if (i2cPresent(IP5306_ADDR)) {
    int before = i2cReadReg(IP5306_ADDR, IP5306_REG_SYS_CTL0);
    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(IP5306_REG_SYS_CTL0);
    Wire.write(IP5306_BOOST_KEEP_ON);
    bool ok = Wire.endTransmission() == 0;
    int after = i2cReadReg(IP5306_ADDR, IP5306_REG_SYS_CTL0);
    Serial.println("   -> IP5306 at 0x75: board is SIM800L_IP5306 (20190610 or 20200811)");
    if (before < 0 || after < 0)
      Serial.printf("   IP5306 SYS_CTL0: register read failed (write %s)\n", ok ? "ok" : "FAILED");
    else
      Serial.printf("   IP5306 SYS_CTL0: 0x%02X -> 0x%02X (boost keep-on %s)\n", before, after,
                    ok ? "set" : "WRITE FAILED");
  } else if (i2cPresent(AXP192_ADDR)) {
    Serial.println("   -> AXP192 at 0x34: board is SIM800L_AXP192_VERSION_20200327");
    Serial.println("      (or SIM800C variant: if the modem never answers below, tell me)");
  } else {
    Serial.println("   -> No known PMU found. Report this; do not guess the revision.");
  }
}

static void powerOnModem() {
  Serial.println("\n=== [3] Powering SIM800L (LilyGO sequence) ===");
  pinMode(pins::MODEM_RST, OUTPUT);
  digitalWrite(pins::MODEM_RST, HIGH);  // keep out of reset
  pinMode(pins::MODEM_PWRKEY, OUTPUT);
  pinMode(pins::MODEM_POWER_ON, OUTPUT);

  digitalWrite(pins::MODEM_POWER_ON, HIGH);  // enable modem supply
  digitalWrite(pins::MODEM_PWRKEY, HIGH);    // PWRKEY idle

  // PWRKEY is a TOGGLE: a >1 s low pulse turns an OFF modem on, but turns an
  // already-running modem OFF. After an ESP32-only reset (RST button, crash,
  // reflash) the modem may still be running, so probe it before pulsing.
  delay(200);
  Modem.begin(115200, SERIAL_8N1, pins::MODEM_RX, pins::MODEM_TX);
  bool alreadyOn = false;
  for (int i = 0; i < 5 && !alreadyOn; i++) alreadyOn = sendAT("AT", 200, false).indexOf("OK") >= 0;
  Modem.end();
  if (alreadyOn) {
    Serial.println("   modem was already running (ESP32-only reset): skipping PWRKEY pulse");
    return;
  }

  // PWRKEY low for >1 s turns the SIM800L on (SIM800 hardware design guide).
  delay(100);
  digitalWrite(pins::MODEM_PWRKEY, LOW);
  delay(1000);
  digitalWrite(pins::MODEM_PWRKEY, HIGH);

  Serial.println("   power applied, PWRKEY pulsed. Waiting 3 s for modem boot...");
  Serial.println("   (watch the modem's red LED: fast blink = searching, slow ~3 s blink = registered)");
  delay(3000);
}

static bool syncModem() {
  Serial.println("\n=== [4] UART / AT communication ===");
  static const uint32_t bauds[] = {115200, 57600, 38400, 19200, 9600};
  for (uint32_t baud : bauds) {
    Modem.begin(baud, SERIAL_8N1, pins::MODEM_RX, pins::MODEM_TX);
    Serial.printf("   trying %lu baud", (unsigned long)baud);
    int tries = (baud == 115200) ? 20 : 8;  // autobaud default: 115200 should work
    for (int i = 0; i < tries; i++) {
      Serial.print(".");
      if (sendAT("AT", 300, false).indexOf("OK") >= 0) {
        Serial.println(" OK");
        g_modemBaud = baud;
        g_modemSynced = true;
        return true;
      }
    }
    Serial.println(" no answer");
    Modem.end();
  }
  return false;
}

static void runStatus() {
  checkResetMarker();
  printSignal(fieldInt(sendAT("AT+CSQ"), "+CSQ:", 0));
  int stat = fieldInt(sendAT("AT+CREG?"), "+CREG:", 1);
  Serial.printf("   registration: %s\n", cregStr(stat));
  sendAT("AT+COPS?", 5000);
  int mv = readSupplyMv(true);
  if (mv > 0) Serial.printf("   modem supply: %d mV\n", mv);
  Serial.printf("   brownout counters: resets=%lu under-voltage=%lu\n", (unsigned long)g_modemResets,
                (unsigned long)g_underVoltage);
}

static void runModemChecks() {
  Serial.println("\n=== [5] Modem identity, SIM, network, power ===");
  sendAT("ATE0");       // no command echo: cleaner output
  sendAT("AT+CMEE=2");  // verbose errors + reset marker (see checkResetMarker)
  sendAT("ATI");
  sendAT("AT+CGMR");  // firmware revision
  sendAT("AT+GSN");   // IMEI
  sendAT("AT+CFUN?");

  String cpin = sendAT("AT+CPIN?", 5000);
  bool simReady = cpin.indexOf("READY") >= 0;
  if (!simReady) {
    if (cpin.indexOf("SIM PIN") >= 0)
      Serial.println("   [X] SIM is PIN-locked. Disable the PIN in a phone first.");
    else
      Serial.println("   [X] SIM not ready / not inserted (CME 10 = not inserted). Power off, reseat SIM.");
  } else {
    sendAT("AT+CCID");
  }

  int mv = readSupplyMv(true);
  if (mv > 0) Serial.printf("   idle supply: %d mV\n", mv);
  else Serial.println("   idle supply: could not read AT+CBC");

  if (!simReady) return;

  // Registration triggers TX bursts: the real power test.
  Serial.println("\n   Waiting up to 90 s for network registration while monitoring supply...");
  const uint32_t start = millis();
  int stat = -1;
  while (millis() - start < 90000) {
    checkResetMarker();
    stat = fieldInt(sendAT("AT+CREG?", 2000, false), "+CREG:", 1);
    int rssi = fieldInt(sendAT("AT+CSQ", 2000, false), "+CSQ:", 0);
    int v = readSupplyMv(false);
    Serial.printf("   t=%2lus  creg=%d (%s)  csq=%d  supply=%d mV\n",
                  (unsigned long)((millis() - start) / 1000), stat, cregStr(stat), rssi, v);
    if (stat == 1 || stat == 5 || stat == 3) break;
    delay(3000);
  }
  if (stat == 1 || stat == 5) {
    sendAT("AT+COPS?", 5000);
    // Give the modem time to do a few more TX bursts so brownouts can show up.
    Serial.println("   registered; sampling supply for 15 s more...");
    for (int i = 0; i < 5; i++) {
      checkResetMarker();
      readSupplyMv(false);
      delay(3000);
    }
  }
}

static void printSummary() {
  Serial.println("\n=== SUMMARY (copy everything above and below this line back to me) ===");
  Serial.printf("   modem UART: %s", g_modemSynced ? "OK" : "FAILED");
  if (g_modemSynced) Serial.printf(" at %lu baud", (unsigned long)g_modemBaud);
  Serial.println();
  if (g_minMv != INT_MAX) {
    Serial.printf("   modem supply range: %d .. %d mV  ", g_minMv, g_maxMv);
    if (g_minMv < 3400) Serial.println("[X] below SIM800L minimum (3400 mV)!");
    else if (g_minMv < 3600) Serial.println("[!] marginal - attach/charge a battery");
    else Serial.println("[OK]");
  }
  Serial.printf("   modem resets seen: %lu   under-voltage warnings: %lu  %s\n",
                (unsigned long)g_modemResets, (unsigned long)g_underVoltage,
                (g_modemResets || g_underVoltage) ? "[X] POWER PROBLEM" : "[OK]");
  Serial.println("\n=== Interactive AT terminal. Type AT commands + Enter. !help for shortcuts. ===");
}

static void handleUsbLine(String line) {
  line.trim();
  if (line.isEmpty()) return;
  if (line == "!help") {
    Serial.println("   !status  signal, registration, operator, supply voltage, counters");
    Serial.println("   !help    this help");
    Serial.println("   anything else is sent to the modem as an AT command");
  } else if (line == "!status") {
    runStatus();
  } else {
    Modem.print(line);
    Modem.print("\r");
  }
}

// ---------------------------------------------------------------- Arduino

void setup() {
  Serial.begin(115200);
  delay(1500);  // let the USB serial monitor attach
  pinMode(pins::LED, OUTPUT);
  digitalWrite(pins::LED, LOW);

  Serial.println("\n\n##### Nodus AI - Phase 1 T-Call diagnostic #####");
  printEspInfo();
  detectPmu();
  powerOnModem();

  if (!syncModem()) {
    Serial.println("\n   [X] Modem did not answer at any baud rate. Check, in order:");
    Serial.println("       - battery connected / power adequate (modem needs up to 2 A peaks)");
    Serial.println("       - is the modem's red LED blinking at all? (no LED = modem not powered)");
    Serial.println("       - board revision: is this really a SIM800L (not SIM800C) T-Call?");
    // Re-open the UART so the interactive terminal still works for manual probing.
    Modem.begin(115200, SERIAL_8N1, pins::MODEM_RX, pins::MODEM_TX);
    printSummary();
    return;
  }
  digitalWrite(pins::LED, HIGH);
  runModemChecks();
  printSummary();
}

void loop() {
  static String usbLine;
  static String modemLine;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      handleUsbLine(usbLine);
      usbLine = "";
    } else if (usbLine.length() < 512) {
      usbLine += c;
    }
  }

  while (Modem.available()) {
    char c = Modem.read();
    Serial.write(c);
    if (c == '\n') {
      inspectModemText(modemLine);
      modemLine = "";
    } else if (c != '\r' && modemLine.length() < 512) {
      modemLine += c;
    }
  }
  delay(1);
}
