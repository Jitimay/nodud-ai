#!/usr/bin/env bash
# Simulate the ESP32 gateway posting one received SMS to the backend.
# Usage: scripts/simulate_sms.sh "+25779123456" "Bonjour, comment conserver les haricots ?"
# Reads the first gateway token from backend/.env. Backend must be running (uvicorn).
set -euo pipefail
cd "$(dirname "$0")/.."
SENDER="${1:?sender phone number}"
TEXT="${2:?message text}"
URL="${NODUS_URL:-http://127.0.0.1:8000}"
TOKEN=$(grep -E '^GATEWAY_TOKENS=' .env | cut -d= -f2- | cut -d, -f1 | cut -d: -f2-)
[ -n "$TOKEN" ] || { echo "GATEWAY_TOKENS missing in .env" >&2; exit 1; }
MSG_ID="sim-$(date +%s%N)"
BODY=$(python3 -c 'import json,sys; print(json.dumps({"messages":[{"message_id":sys.argv[1],"sender":sys.argv[2],"text":sys.argv[3]}]}))' "$MSG_ID" "$SENDER" "$TEXT")
curl -sS -X POST "$URL/v1/gateway/messages" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d "$BODY" | python3 -m json.tool --no-ensure-ascii
