from tests.conftest import ADMIN_AUTH, AUTH


def test_admin_requires_admin_token(client):
    assert client.get("/v1/admin/stats").status_code == 401
    assert client.get("/v1/admin/stats", headers=AUTH).status_code == 401  # gateway token is not enough


def test_stats_reports_gateway_and_counts(client):
    client.post("/v1/gateway/heartbeat", json={"uptime_s": 5, "gsm_csq": 17, "modem_supply_mv": 3950}, headers=AUTH)
    client.post("/v1/gateway/messages", headers=AUTH,
                json={"messages": [{"message_id": "msg-adm-0001", "sender": "+25779123456", "text": "Bonjour"}]})
    s = client.get("/v1/admin/stats", headers=ADMIN_AUTH).json()
    gw = s["gateways"][0]
    assert gw["device_id"] == "test-gw" and gw["online"] is True
    assert gw["telemetry"]["modem_supply_mv"] == 3950
    assert s["last_24h"]["inbound"] == {"processed": 1}
    assert s["last_24h"]["unique_senders"] == 1
    assert s["outbox_pending"] == 1  # reply dispatched, not yet confirmed
