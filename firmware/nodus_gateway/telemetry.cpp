#include "telemetry.h"

#include <esp_timer.h>

#include <cstdarg>

Telemetry g_tel;

static SemaphoreHandle_t s_logMutex = nullptr;

void Telemetry::setOperator(const String &op) {
  portENTER_CRITICAL(&mux_);
  strlcpy(op_, op.c_str(), sizeof op_);
  portEXIT_CRITICAL(&mux_);
}

String Telemetry::getOperator() {
  char copy[sizeof op_];
  portENTER_CRITICAL(&mux_);
  memcpy(copy, op_, sizeof op_);
  portEXIT_CRITICAL(&mux_);
  return String(copy);
}

void logInit() { s_logMutex = xSemaphoreCreateMutex(); }

void logf(const char *tag, const char *fmt, ...) {
  char buf[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (s_logMutex) xSemaphoreTake(s_logMutex, portMAX_DELAY);
  Serial.printf("[%8lu] %-5s %s\n", (unsigned long)uptimeSeconds(), tag, buf);
  if (s_logMutex) xSemaphoreGive(s_logMutex);
}

String maskPhone(const String &n) {
  if (n.length() <= 6) return "***";
  return n.substring(0, 4) + "****" + n.substring(n.length() - 4);
}

uint64_t uptimeSeconds() { return (uint64_t)(esp_timer_get_time() / 1000000ULL); }
