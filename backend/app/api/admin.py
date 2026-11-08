"""Monitoring endpoint for operators (not for gateways). Requires ADMIN_TOKEN."""

import json
from datetime import UTC, datetime, timedelta

from fastapi import APIRouter, Depends
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.core.config import Settings, get_settings
from app.core.security import require_admin
from app.database.session import get_db
from app.models.db import GatewayHeartbeat, InboundMessage, OutboundMessage

router = APIRouter(prefix="/v1/admin", tags=["admin"], dependencies=[Depends(require_admin)])


def _aware(dt: datetime) -> datetime:
    return dt if dt.tzinfo else dt.replace(tzinfo=UTC)  # SQLite returns naive UTC


@router.get("/stats")
def stats(db: Session = Depends(get_db), settings: Settings = Depends(get_settings)) -> dict:
    now = datetime.now(UTC)
    since = now - timedelta(hours=24)

    gateways = []
    for hb in db.scalars(select(GatewayHeartbeat).order_by(GatewayHeartbeat.device_id)):
        age = (now - _aware(hb.received_at)).total_seconds()
        gateways.append({
            "device_id": hb.device_id,
            "online": age <= settings.gateway_offline_after_s,
            "last_seen_s_ago": int(age),
            "telemetry": json.loads(hb.payload),
        })

    def by_status(model) -> dict[str, int]:  # noqa: ANN001
        rows = db.execute(
            select(model.status, func.count()).where(model.created_at >= since).group_by(model.status)
        ).all()
        return {status: count for status, count in rows}

    pending = db.scalar(
        select(func.count()).select_from(OutboundMessage).where(OutboundMessage.status.in_(("queued", "dispatched")))
    )
    unique_senders = db.scalar(
        select(func.count(func.distinct(InboundMessage.sender))).where(InboundMessage.created_at >= since)
    )
    return {
        "generated_at": now.isoformat(),
        "model": settings.groq_model,
        "gateways": gateways,
        "last_24h": {
            "inbound": by_status(InboundMessage),
            "outbound": by_status(OutboundMessage),
            "unique_senders": unique_senders,
        },
        "outbox_pending": pending,
    }
