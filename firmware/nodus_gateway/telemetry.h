// Shared runtime state (read by the heartbeat and the "status" command) and a
// thread-safe logger. All counters are atomics, so any task can update them.
#pragma once

#include <Arduino.h>

#include <atomic>

struct Telemetry {
  std::atomic<uint32_t> smsReceived{0};  // stored to flash from the SIM
  std::atomic<uint32_t> smsSent{0};
  std::atomic<uint32_t> smsFailed{0};
  std::atomic<uint32_t> smsUndecodable{0};
  std::atomic<uint32_t> httpErrors{0};
  std::atomic<uint32_t> modemResets{0};

  std::atomic<int> csq{99};
  std::atomic<int> creg{-1};
  std::atomic<int> supplyMv{0};
  std::atomic<int> wifiRssi{0};
  std::atomic<int> simUsed{-1};
  std::atomic<int> simTotal{-1};

  std::atomic<bool> modemReady{false};
  std::atomic<bool> wifiUp{false};
  std::atomic<bool> backendOk{false};

  void setOperator(const String &op);
  String getOperator();

 private:
  char op_[32] = {0};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

extern Telemetry g_tel;

void logInit();
void logf(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
String maskPhone(const String &number);  // +25779123456 -> +257****3456
uint64_t uptimeSeconds();
