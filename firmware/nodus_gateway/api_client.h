// Backend communication: the network task.
//
// Talks to the FastAPI backend (see backend/app/api/gateway.py):
//   POST /v1/gateway/messages             upload one received SMS, get the reply inline
//   GET  /v1/gateway/outbox               fetch replies not yet confirmed (safety net)
//   POST /v1/gateway/outbox/{id}/status   report sent / failed
//   POST /v1/gateway/heartbeat            telemetry
// Never blocks the modem task: they share only the flash queues.
#pragma once

#include "config_store.h"

namespace net {
void start(const GatewayConfig &c);
}  // namespace net
