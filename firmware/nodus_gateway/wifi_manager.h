// Wi-Fi station management with reconnect backoff. Driven by the network task.
#pragma once

#include "config_store.h"

namespace wifi_mgr {
void begin(const GatewayConfig &c);
// Call frequently. Returns true when connected; otherwise (re)connects with backoff.
bool ensure();
}  // namespace wifi_mgr
