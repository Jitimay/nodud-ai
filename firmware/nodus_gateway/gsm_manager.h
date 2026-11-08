// The modem task: the ONLY code that talks to the SIM800L UART.
//
// Receives SMS (PDU mode), persists them to the flash queue, deletes them from
// the SIM only after they are safely stored, reassembles multipart SMS, sends
// queued replies, monitors signal/registration/supply voltage, and recovers
// the modem (re-init, power-cycle, ESP32 restart as last resort).
#pragma once

#include <Arduino.h>

namespace gsm {
// deviceId is mixed into message ids so two gateways never produce the same id.
void start(const String &deviceId);
}  // namespace gsm
