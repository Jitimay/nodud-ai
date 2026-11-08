"""Database tables."""

import uuid
from datetime import UTC, datetime

from sqlalchemy import DateTime, ForeignKey, Index, Integer, String, Text
from sqlalchemy.orm import Mapped, mapped_column

from app.database.session import Base


def utcnow() -> datetime:
    return datetime.now(UTC)


class InboundMessage(Base):
    """An SMS received by a gateway. `message_id` is the gateway's idempotency key."""

    __tablename__ = "inbound_messages"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    message_id: Mapped[str] = mapped_column(String(128), unique=True, index=True)
    device_id: Mapped[str] = mapped_column(String(64))
    sender: Mapped[str] = mapped_column(String(32), index=True)
    text: Mapped[str] = mapped_column(Text)
    language: Mapped[str] = mapped_column(String(8), default="unknown")
    # processed | ignored | rate_limited | ai_error
    status: Mapped[str] = mapped_column(String(16))
    received_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)


class OutboundMessage(Base):
    """An SMS the backend wants a gateway to send."""

    __tablename__ = "outbound_messages"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    inbound_id: Mapped[int | None] = mapped_column(ForeignKey("inbound_messages.id"))
    recipient: Mapped[str] = mapped_column(String(32))
    text: Mapped[str] = mapped_column(Text)
    encoding: Mapped[str] = mapped_column(String(8))  # GSM7 | UCS2
    segments: Mapped[int] = mapped_column(Integer)
    # queued -> dispatched -> sent | failed   (dispatched expires back to re-offer)
    status: Mapped[str] = mapped_column(String(16), default="queued")
    device_id: Mapped[str | None] = mapped_column(String(64))
    attempts: Mapped[int] = mapped_column(Integer, default=0)
    lease_until: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    last_error: Mapped[str | None] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)

    __table_args__ = (Index("ix_outbound_status_created", "status", "created_at"),)


class ConversationTurn(Base):
    """One message in a per-phone-number conversation (role: user | assistant)."""

    __tablename__ = "conversation_turns"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    phone: Mapped[str] = mapped_column(String(32))
    role: Mapped[str] = mapped_column(String(16))
    content: Mapped[str] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)

    __table_args__ = (Index("ix_turn_phone_created", "phone", "created_at"),)


class GatewayHeartbeat(Base):
    """Latest telemetry from each gateway (one row per device, overwritten)."""

    __tablename__ = "gateway_heartbeats"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    payload: Mapped[str] = mapped_column(Text)  # JSON
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)
