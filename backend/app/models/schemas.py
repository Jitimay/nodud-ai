"""Request/response bodies for the gateway API (the contract with the ESP32)."""

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field


class InboundSMS(BaseModel):
    message_id: str = Field(min_length=8, max_length=128, description="Idempotency key computed by the gateway")
    sender: str = Field(min_length=1, max_length=32)
    text: str = Field(max_length=2000)
    received_at: datetime | None = Field(default=None, description="SMSC timestamp from the SIM800L, if known")


class InboundBatch(BaseModel):
    messages: list[InboundSMS] = Field(min_length=1, max_length=10)


class OutboxItem(BaseModel):
    id: str
    recipient: str
    text: str
    encoding: Literal["GSM7", "UCS2"]
    segments: int


class InboundResult(BaseModel):
    message_id: str
    status: Literal["processed", "duplicate", "ignored", "rate_limited", "ai_error"]
    reply: OutboxItem | None = None


class InboundResponse(BaseModel):
    results: list[InboundResult]


class OutboxResponse(BaseModel):
    items: list[OutboxItem]


class DeliveryStatus(BaseModel):
    status: Literal["sent", "failed"]
    error: str | None = Field(default=None, max_length=500)


class Heartbeat(BaseModel):
    """Gateway telemetry. All optional so firmware can grow into it."""

    firmware_version: str | None = None
    uptime_s: int | None = None
    gsm_csq: int | None = None
    gsm_registered: bool | None = None
    operator: str | None = None
    wifi_rssi: int | None = None
    free_heap: int | None = None
    sms_received: int | None = None
    sms_sent: int | None = None
    sms_failed: int | None = None
    queue_depth: int | None = None
    modem_supply_mv: int | None = None
