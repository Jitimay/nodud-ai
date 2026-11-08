#!/usr/bin/env python3
"""Plays the ESP32 gateway's role against a running backend, using exactly the
requests the firmware makes (firmware/nodus_gateway/api_client.cpp):

  1. POST /v1/gateway/messages            one received SMS, reply comes back inline
  2. POST /v1/gateway/outbox/{id}/status  "sent" (as if the SIM800L had sent it)
  3. GET  /v1/gateway/outbox               safety-net poll (should be empty)
  4. POST /v1/gateway/heartbeat            telemetry
  5. GET  /v1/admin/stats                  what an operator sees

Usage:  .venv/bin/python scripts/fake_gateway.py "+25779123456" "Bonjour, ..."
"""

import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

BASE = os.environ.get("NODUS_URL", "http://127.0.0.1:8000")
ENV = dict(l.split("=", 1) for l in (Path(__file__).parent.parent / ".env").read_text().splitlines()
           if "=" in l and not l.lstrip().startswith("#"))
DEVICE, TOKEN = ENV["GATEWAY_TOKENS"].split(",")[0].strip().split(":", 1)
ADMIN = ENV.get("ADMIN_TOKEN", "").strip()


def call(method, path, body=None, token=TOKEN):
    req = urllib.request.Request(BASE + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def main():
    sender = sys.argv[1] if len(sys.argv) > 1 else "+25779123456"
    text = sys.argv[2] if len(sys.argv) > 2 else "Bonjour, comment conserver les haricots ?"
    ts = datetime.now(timezone(timedelta(hours=2))).replace(microsecond=0).isoformat()  # SIM800L SCTS format
    msg_id = hashlib.sha256(f"{DEVICE}|{sender}|{ts}|{text}".encode()).hexdigest()[:16]

    t0 = time.monotonic()
    code, res = call("POST", "/v1/gateway/messages",
                     {"messages": [{"message_id": msg_id, "sender": sender, "text": text, "received_at": ts}]})
    print(f"1. upload -> HTTP {code} in {time.monotonic() - t0:.1f}s")
    if code != 200:
        sys.exit(f"   {res}")
    r = res["results"][0]
    print(f"   status: {r['status']}")
    reply = r["reply"]
    if reply:
        print(f"   reply ({reply['encoding']}, {reply['segments']} SMS, {len(reply['text'])} chars):\n   > "
              + reply["text"].replace("\n", "\n   > "))
        code, res = call("POST", f"/v1/gateway/outbox/{reply['id']}/status", {"status": "sent"})
        print(f"2. status report -> HTTP {code} {res}")
    code, res = call("POST", "/v1/gateway/messages",
                     {"messages": [{"message_id": msg_id, "sender": sender, "text": text, "received_at": ts}]})
    print(f"   retry of same SMS -> {res['results'][0]['status']}, reply={res['results'][0]['reply']}")
    code, res = call("GET", "/v1/gateway/outbox?limit=5")
    print(f"3. outbox poll -> HTTP {code}, {len(res['items'])} item(s)")
    code, res = call("POST", "/v1/gateway/heartbeat",
                     {"firmware_version": "fake", "uptime_s": 42, "gsm_csq": 18, "gsm_registered": True,
                      "operator": "Test", "wifi_rssi": -60, "sms_received": 1, "sms_sent": 1, "modem_supply_mv": 3990})
    print(f"4. heartbeat -> HTTP {code} {res}")
    if ADMIN:
        code, res = call("GET", "/v1/admin/stats", token=ADMIN)
        print(f"5. admin stats -> HTTP {code}\n" + json.dumps(res, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
