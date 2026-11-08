"""Endpoints used by the ESP32 gateway. All require a device bearer token."""

import json

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from app.api.deps import get_processor
from app.core.security import require_gateway
from app.database.session import get_db
from app.models.db import GatewayHeartbeat
from app.models.schemas import DeliveryStatus, Heartbeat, InboundBatch, InboundResponse, OutboxResponse
from app.services.message_processor import MessageProcessor

router = APIRouter(prefix="/v1/gateway", tags=["gateway"])


@router.post("/messages", response_model=InboundResponse)
def post_messages(
    batch: InboundBatch,
    device_id: str = Depends(require_gateway),
    proc: MessageProcessor = Depends(get_processor),
) -> InboundResponse:
    """Upload received SMS. Safe to retry: duplicates are recognised by message_id.
    Replies are returned inline; any reply not confirmed later reappears in /outbox."""
    return InboundResponse(results=[proc.handle(device_id, sms) for sms in batch.messages])


@router.get("/outbox", response_model=OutboxResponse)
def get_outbox(
    limit: int = 10,
    device_id: str = Depends(require_gateway),
    proc: MessageProcessor = Depends(get_processor),
) -> OutboxResponse:
    """SMS waiting to be sent: new ones plus dispatched ones never confirmed."""
    return OutboxResponse(items=proc.take_outbox(device_id, limit=max(1, min(limit, 20))))


@router.post("/outbox/{outbound_id}/status")
def post_status(
    outbound_id: str,
    body: DeliveryStatus,
    device_id: str = Depends(require_gateway),
    proc: MessageProcessor = Depends(get_processor),
) -> dict:
    out = proc.report_status(outbound_id, body.status, body.error)
    if out is None:
        raise HTTPException(404, "Unknown outbound id")
    return {"id": out.id, "status": out.status}


@router.post("/heartbeat")
def post_heartbeat(
    hb: Heartbeat,
    device_id: str = Depends(require_gateway),
    db: Session = Depends(get_db),
) -> dict:
    row = db.get(GatewayHeartbeat, device_id)
    payload = json.dumps(hb.model_dump(exclude_none=True))
    if row is None:
        db.add(GatewayHeartbeat(device_id=device_id, payload=payload))
    else:
        row.payload = payload
    db.commit()
    return {"ok": True}
