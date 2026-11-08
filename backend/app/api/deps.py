"""Shared FastAPI dependencies."""

from functools import lru_cache

from fastapi import Depends
from sqlalchemy.orm import Session

from app.core.config import Settings, get_settings
from app.database.session import get_db
from app.services.groq_service import GroqService
from app.services.message_processor import MessageProcessor


@lru_cache
def get_ai() -> GroqService:
    return GroqService(get_settings())


def get_processor(
    db: Session = Depends(get_db),
    settings: Settings = Depends(get_settings),
    ai: GroqService = Depends(get_ai),
) -> MessageProcessor:
    return MessageProcessor(db, settings, ai)
