"""FastAPI entry point.  Run:  uvicorn app.main:app --reload"""

import logging
from contextlib import asynccontextmanager
from pathlib import Path

from alembic import command
from alembic.config import Config
from fastapi import FastAPI
from sqlalchemy import inspect

from app.api import admin, gateway
from app.core.config import get_settings
from app.core.logging import setup_logging
from app.database.session import engine

log = logging.getLogger("nodus")

_BACKEND_DIR = Path(__file__).resolve().parent.parent


def run_migrations() -> None:
    """Bring the database schema up to date (safe to run on every start)."""
    cfg = Config(str(_BACKEND_DIR / "alembic.ini"))
    cfg.set_main_option("script_location", str(_BACKEND_DIR / "migrations"))
    # Databases created by v0.1 (before Alembic) already have the initial tables:
    # mark them as revision 0001 instead of trying to create the tables again.
    # (An empty alembic_version table, left by an interrupted first migration, counts as unversioned.)
    with engine.connect() as conn:
        tables = inspect(conn).get_table_names()
        versioned = "alembic_version" in tables and bool(
            conn.exec_driver_sql("SELECT COUNT(*) FROM alembic_version").scalar())
    if "inbound_messages" in tables and not versioned:
        log.info("existing pre-migration database found: stamping it as revision 0001")
        command.stamp(cfg, "0001")
    command.upgrade(cfg, "head")


@asynccontextmanager
async def lifespan(_: FastAPI):
    settings = get_settings()  # fails fast if .env is incomplete
    setup_logging(settings.log_level)
    devices = sorted(set(settings.gateway_token_map().values()))
    if not devices:
        log.warning("GATEWAY_TOKENS is empty: no gateway can authenticate")
    run_migrations()
    log.info("Nodus AI backend up: model=%s devices=%s db=%s admin_api=%s",
             settings.groq_model, devices, settings.database_url.split("://")[0],
             "on" if len(settings.admin_token.get_secret_value()) >= 24 else "off")
    yield


app = FastAPI(title="Nodus AI", version="0.2.0", lifespan=lifespan)
app.include_router(gateway.router)
app.include_router(admin.router)


@app.get("/health", tags=["ops"])
def health() -> dict:
    return {"status": "ok"}
