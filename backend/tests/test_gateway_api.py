from tests.conftest import AUTH


def sms(mid="msg-00000001", sender="+25779123456", text="Bonjour, comment conserver les haricots ?"):
    return {"messages": [{"message_id": mid, "sender": sender, "text": text}]}


def post(client, body):
    r = client.post("/v1/gateway/messages", json=body, headers=AUTH)
    assert r.status_code == 200, r.text
    return r.json()["results"][0]


def test_requires_token(client):
    assert client.post("/v1/gateway/messages", json=sms()).status_code == 401
    bad = {"Authorization": "Bearer wrong-token-0123456789abcdef"}
    assert client.post("/v1/gateway/messages", json=sms(), headers=bad).status_code == 401


def test_processes_and_replies(client, fake_ai):
    res = post(client, sms())
    assert res["status"] == "processed"
    assert res["reply"]["recipient"] == "+25779123456"
    assert res["reply"]["text"] == fake_ai.reply
    assert res["reply"]["encoding"] == "GSM7"


def test_local_number_normalised(client):
    assert post(client, sms(sender="79 12 34 56"))["reply"]["recipient"] == "+25779123456"


def test_duplicate_does_not_call_ai_twice_and_rehands_reply(client, fake_ai):
    first = post(client, sms())
    again = post(client, sms())
    assert again["status"] == "duplicate"
    assert again["reply"]["id"] == first["reply"]["id"]  # gateway may have lost the first response
    assert len(fake_ai.calls) == 1


def test_duplicate_after_sent_has_no_reply(client):
    first = post(client, sms())
    client.post(f"/v1/gateway/outbox/{first['reply']['id']}/status", json={"status": "sent"}, headers=AUTH)
    assert post(client, sms())["reply"] is None


def test_shortcode_and_alphanumeric_senders_ignored(client, fake_ai):
    assert post(client, sms(mid="msg-short001", sender="161"))["status"] == "ignored"
    assert post(client, sms(mid="msg-alpha001", sender="Lumitel"))["status"] == "ignored"
    assert fake_ai.calls == []


def test_rate_limit(client):
    for i in range(3):  # RATE_LIMIT_PER_HOUR=3 in tests
        assert post(client, sms(mid=f"msg-rate-{i:04d}"))["status"] == "processed"
    res = post(client, sms(mid="msg-rate-9999"))
    assert res["status"] == "rate_limited" and res["reply"] is None


def test_conversation_context_is_passed(client, fake_ai):
    post(client, sms(mid="msg-ctx-0001", text="Je cultive du maïs"))
    post(client, sms(mid="msg-ctx-0002", text="Et pour les ravageurs ?"))
    history, user_text = fake_ai.calls[-1]
    assert user_text == "Et pour les ravageurs ?"
    assert [h["role"] for h in history] == ["user", "assistant"]
    assert history[0]["content"] == "Je cultive du maïs"


def test_reset_clears_context(client, fake_ai):
    post(client, sms(mid="msg-rst-0001", text="Je cultive du maïs"))
    assert post(client, sms(mid="msg-rst-0002", text="reset"))["status"] == "processed"
    post(client, sms(mid="msg-rst-0003", text="Bonjour"))
    assert fake_ai.calls[-1][0] == []
    assert len(fake_ai.calls) == 2  # reset itself does not call the AI


def test_ai_failure_sends_apology(client, fake_ai):
    fake_ai.fail = True
    res = post(client, sms())
    assert res["status"] == "ai_error"
    assert "Désolé" in res["reply"]["text"] or "Desole" in res["reply"]["text"]


def test_long_reply_capped(client, fake_ai):
    fake_ai.reply = "Une phrase utile pour le paysan. " * 40
    res = post(client, sms())
    assert res["reply"]["segments"] <= 2  # SMS_MAX_SEGMENTS=2 in tests


def test_unconfirmed_reply_reoffered_by_outbox_after_lease(client):
    from datetime import UTC, datetime, timedelta

    from app.database.session import SessionLocal
    from app.models.db import OutboundMessage

    res = post(client, sms())
    # Within the lease: not offered again.
    assert client.get("/v1/gateway/outbox", headers=AUTH).json()["items"] == []
    # Expire the lease (simulates gateway rebooting before confirming).
    with SessionLocal() as db:
        out = db.get(OutboundMessage, res["reply"]["id"])
        out.lease_until = datetime.now(UTC) - timedelta(seconds=1)
        db.commit()
    items = client.get("/v1/gateway/outbox", headers=AUTH).json()["items"]
    assert [i["id"] for i in items] == [res["reply"]["id"]]


def test_failed_send_retries_then_gives_up(client):
    res = post(client, sms())
    oid = res["reply"]["id"]
    # attempt 1 failed -> back in queue
    r = client.post(f"/v1/gateway/outbox/{oid}/status", json={"status": "failed", "error": "CMS ERROR 500"}, headers=AUTH)
    assert r.json()["status"] == "queued"
    # attempts 2 and 3
    for expected in ("queued", "failed"):
        assert [i["id"] for i in client.get("/v1/gateway/outbox", headers=AUTH).json()["items"]] == [oid]
        r = client.post(f"/v1/gateway/outbox/{oid}/status", json={"status": "failed", "error": "x"}, headers=AUTH)
        assert r.json()["status"] == expected
    assert client.get("/v1/gateway/outbox", headers=AUTH).json()["items"] == []


def test_heartbeat(client):
    r = client.post("/v1/gateway/heartbeat", json={"uptime_s": 10, "gsm_csq": 18}, headers=AUTH)
    assert r.json() == {"ok": True}
