"""Inbound SMS pipeline and outbox management.

Delivery model (see docs/architecture.md):
- Inbound is idempotent on `message_id`: the gateway may retry freely.
- A reply is created as an outbound message in state "dispatched" with a lease and
  returned inline. If the gateway never confirms it (lost HTTP response, reboot),
  the lease expires and GET /outbox offers it again. So the inline reply is a fast
  path, and the outbox is the safety net.
"""

import logging
from datetime import UTC, datetime, timedelta

from sqlalchemy import delete, func, or_, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.core.config import Settings
from app.core.logging import mask_phone
from app.models.db import ConversationTurn, InboundMessage, OutboundMessage
from app.models.schemas import InboundResult, InboundSMS, OutboxItem
from app.services import language, messages, phone
from app.services.groq_service import AIUnavailableError, GroqService
from app.services.sms_text import prepare_sms

log = logging.getLogger(__name__)

_COUNTED_STATUSES = ("processing", "processed", "ai_error")


def _now() -> datetime:
    return datetime.now(UTC)


def _to_item(o: OutboundMessage) -> OutboxItem:
    return OutboxItem(id=o.id, recipient=o.recipient, text=o.text, encoding=o.encoding, segments=o.segments)  # type: ignore[arg-type]


class MessageProcessor:
    def __init__(self, db: Session, settings: Settings, ai: GroqService):
        self.db, self.s, self.ai = db, settings, ai

    # ------------------------------------------------------------------ inbound

    def handle(self, device_id: str, sms: InboundSMS) -> InboundResult:
        existing = self.db.scalar(select(InboundMessage).where(InboundMessage.message_id == sms.message_id))
        if existing is not None:
            return self._duplicate(existing, device_id)

        sender = phone.normalize(sms.sender, self.s.default_country_code)
        text = sms.text.strip()
        lang = language.detect(text)

        # Claim the message_id first, so a concurrent retry sees it as a duplicate.
        inbound = InboundMessage(
            message_id=sms.message_id, device_id=device_id, sender=sender, text=text,
            language=lang, status="processing", received_at=sms.received_at,
        )
        self.db.add(inbound)
        try:
            self.db.commit()
        except IntegrityError:
            self.db.rollback()
            existing = self.db.scalar(select(InboundMessage).where(InboundMessage.message_id == sms.message_id))
            return self._duplicate(existing, device_id)  # type: ignore[arg-type]

        tag = f"[{sms.message_id[:12]} {mask_phone(sender)} {lang}]"

        if not phone.is_replyable(sender):
            log.info("%s ignored: non-replyable sender", tag)
            return self._finish(inbound, "ignored")

        if self._over_rate_limit(sender, inbound.id):
            log.warning("%s rate limited", tag)
            return self._finish(inbound, "rate_limited")

        if not text:
            return self._finish(inbound, "processed", self._queue_reply(inbound, messages.get("empty", lang), device_id))

        if text.upper() in messages.RESET_COMMANDS:
            self.db.execute(delete(ConversationTurn).where(ConversationTurn.phone == sender))
            log.info("%s conversation reset", tag)
            return self._finish(inbound, "processed", self._queue_reply(inbound, messages.get("reset", lang), device_id))

        try:
            ai = self.ai.generate(self._history(sender), text)
        except AIUnavailableError as e:
            log.error("%s AI unavailable (%s): sending apology", tag, e)
            return self._finish(inbound, "ai_error", self._queue_reply(inbound, messages.get("ai_error", lang), device_id))

        reply = self._queue_reply(inbound, ai.text, device_id)
        # Store what the user actually received, so context matches their screen.
        self.db.add_all([
            ConversationTurn(phone=sender, role="user", content=text),
            ConversationTurn(phone=sender, role="assistant", content=reply.text),
        ])
        log.info("%s replied: %d chars, %s x%d, %.2fs, tokens %s/%s",
                 tag, len(reply.text), reply.encoding, reply.segments, ai.latency_s,
                 ai.prompt_tokens, ai.completion_tokens)
        return self._finish(inbound, "processed", reply)

    def _finish(self, inbound: InboundMessage, status: str, reply: OutboundMessage | None = None) -> InboundResult:
        inbound.status = status
        self.db.commit()
        return InboundResult(message_id=inbound.message_id, status=status,  # type: ignore[arg-type]
                             reply=_to_item(reply) if reply else None)

    def _duplicate(self, inbound: InboundMessage, device_id: str) -> InboundResult:
        """Gateway retried. Re-hand any reply it may not have received."""
        out = self.db.scalar(select(OutboundMessage).where(OutboundMessage.inbound_id == inbound.id))
        if out is not None and out.status in ("queued", "dispatched"):
            self._lease(out, device_id, count_attempt=False)
            self.db.commit()
            return InboundResult(message_id=inbound.message_id, status="duplicate", reply=_to_item(out))
        return InboundResult(message_id=inbound.message_id, status="duplicate")

    def _over_rate_limit(self, sender: str, current_id: int) -> bool:
        def count_since(delta: timedelta) -> int:
            return self.db.scalar(
                select(func.count()).select_from(InboundMessage).where(
                    InboundMessage.sender == sender,
                    InboundMessage.status.in_(_COUNTED_STATUSES),
                    InboundMessage.created_at >= _now() - delta,
                    InboundMessage.id != current_id,
                )
            ) or 0

        return (count_since(timedelta(hours=1)) >= self.s.rate_limit_per_hour
                or count_since(timedelta(days=1)) >= self.s.rate_limit_per_day)

    def _history(self, sender: str) -> list[dict[str, str]]:
        rows = self.db.scalars(
            select(ConversationTurn)
            .where(ConversationTurn.phone == sender,
                   ConversationTurn.created_at >= _now() - timedelta(hours=self.s.context_max_age_hours))
            .order_by(ConversationTurn.created_at.desc(), ConversationTurn.id.desc())
            .limit(self.s.context_turns)
        ).all()
        return [{"role": r.role, "content": r.content} for r in reversed(rows)]

    def _queue_reply(self, inbound: InboundMessage, text: str, device_id: str) -> OutboundMessage:
        sms = prepare_sms(text, self.s.sms_max_segments)
        out = OutboundMessage(inbound_id=inbound.id, recipient=inbound.sender, text=sms.text,
                              encoding=sms.encoding, segments=sms.segments)
        self._lease(out, device_id)
        self.db.add(out)
        self.db.flush()  # assigns the id
        return out

    # ------------------------------------------------------------------ outbox

    def _lease(self, out: OutboundMessage, device_id: str, count_attempt: bool = True) -> None:
        out.status = "dispatched"
        out.device_id = device_id
        out.lease_until = _now() + timedelta(seconds=self.s.outbox_lease_s)
        if count_attempt:
            out.attempts = (out.attempts or 0) + 1

    def take_outbox(self, device_id: str, limit: int = 10) -> list[OutboxItem]:
        """Queued messages plus dispatched ones whose lease expired (never confirmed)."""
        rows = self.db.scalars(
            select(OutboundMessage)
            .where(or_(OutboundMessage.status == "queued",
                       (OutboundMessage.status == "dispatched") & (OutboundMessage.lease_until < _now())))
            .order_by(OutboundMessage.created_at)
            .limit(limit)
        ).all()
        items: list[OutboxItem] = []
        for out in rows:
            if out.attempts >= self.s.outbox_max_attempts:
                out.status, out.last_error = "failed", out.last_error or "max attempts without confirmation"
                log.error("outbox %s to %s failed permanently: %s", out.id, mask_phone(out.recipient), out.last_error)
                continue
            self._lease(out, device_id)
            items.append(_to_item(out))
        self.db.commit()
        return items

    def report_status(self, outbound_id: str, status: str, error: str | None) -> OutboundMessage | None:
        out = self.db.get(OutboundMessage, outbound_id)
        if out is None:
            return None
        if out.status == "sent":
            return out  # idempotent: a repeated confirmation changes nothing
        if status == "sent":
            out.status, out.lease_until = "sent", None
        elif out.attempts >= self.s.outbox_max_attempts:
            out.status, out.last_error = "failed", error
            log.error("outbox %s to %s failed permanently: %s", out.id, mask_phone(out.recipient), error)
        else:
            out.status, out.last_error, out.lease_until = "queued", error, None  # retry via outbox
            log.warning("outbox %s send failed (attempt %d): %s", out.id, out.attempts, error)
        self.db.commit()
        return out
