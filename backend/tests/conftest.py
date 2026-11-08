"""Test fixtures: isolated SQLite DB per test, fake Groq, test gateway token."""

import os
import tempfile

# Must be set before app modules are imported (settings and engine are created at import).
_tmpdir = tempfile.mkdtemp(prefix="nodus-test-")
os.environ.update({
    "GROQ_API_KEY": "test-key-not-real",
    "GROQ_MODEL": "test-model",
    "GATEWAY_TOKENS": "test-gw:test-token-0123456789abcdefghij",
    "DATABASE_URL": f"sqlite:///{_tmpdir}/test.db",
    "RATE_LIMIT_PER_HOUR": "3",
    "SMS_MAX_SEGMENTS": "2",
    "ADMIN_TOKEN": "admin-token-0123456789abcdefghij",
})

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from app.api.deps import get_ai  # noqa: E402
from app.database.session import Base, engine  # noqa: E402
from app.main import app  # noqa: E402
from app.services.groq_service import AIReply, AIUnavailableError  # noqa: E402

TOKEN = "test-token-0123456789abcdefghij"
AUTH = {"Authorization": f"Bearer {TOKEN}"}
ADMIN_AUTH = {"Authorization": "Bearer admin-token-0123456789abcdefghij"}


class FakeAI:
    def __init__(self):
        self.calls: list[tuple[list, str]] = []
        self.reply = "Bonjour ! Séchez les haricots au soleil."
        self.fail = False

    def generate(self, history, user_text):
        self.calls.append((list(history), user_text))
        if self.fail:
            raise AIUnavailableError("timeout")
        return AIReply(text=self.reply, model="test-model", latency_s=0.01, prompt_tokens=1, completion_tokens=1)


@pytest.fixture
def fake_ai():
    return FakeAI()


@pytest.fixture
def client(fake_ai):
    # Start from an empty database; the app's startup runs the Alembic migrations.
    Base.metadata.drop_all(engine)
    with engine.begin() as conn:
        conn.exec_driver_sql("DROP TABLE IF EXISTS alembic_version")
    app.dependency_overrides[get_ai] = lambda: fake_ai
    with TestClient(app) as c:
        yield c
    app.dependency_overrides.clear()
