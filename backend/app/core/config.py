"""Application settings, loaded from environment variables / backend/.env."""

from functools import lru_cache

from pydantic import Field, SecretStr, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    # --- Groq -------------------------------------------------------------
    groq_api_key: SecretStr
    groq_model: str = "qwen/qwen3.8-27b"
    groq_timeout_s: float = 20.0
    groq_max_retries: int = 2
    groq_temperature: float = 0.4
    groq_max_completion_tokens: int = 400
    # Only for reasoning models (e.g. "low" for openai/gpt-oss-*). Empty = don't send.
    groq_reasoning_effort: str = ""

    # --- Gateway authentication ------------------------------------------
    # Comma-separated "device_id:token" pairs, e.g. "tcall-01:3f9a...".
    # Generate a token with:  python -c "import secrets; print(secrets.token_urlsafe(32))"
    gateway_tokens: str = ""
    # Bearer token for /v1/admin/* (monitoring). Empty = admin API disabled.
    admin_token: SecretStr = SecretStr("")
    # A gateway with no heartbeat for this long is reported offline.
    gateway_offline_after_s: int = 300

    # --- Storage -----------------------------------------------------------
    database_url: str = "sqlite:///./nodus.db"

    # --- SMS policy ----------------------------------------------------------
    sms_max_segments: int = Field(default=3, ge=1, le=6)
    sms_target_chars: int = 300  # length we ask the model for
    default_country_code: str = "257"  # Burundi; used to normalise local numbers

    # --- Conversation & abuse protection -------------------------------------
    context_turns: int = 6  # previous user+assistant messages sent to the model
    context_max_age_hours: int = 24  # older history is ignored
    rate_limit_per_hour: int = 15  # inbound SMS per sender per hour
    rate_limit_per_day: int = 60

    # --- Outbox ----------------------------------------------------------------
    outbox_lease_s: int = 180  # a dispatched SMS not confirmed within this is re-offered
    outbox_max_attempts: int = 3

    log_level: str = "INFO"

    @field_validator("groq_api_key")
    @classmethod
    def _key_present(cls, v: SecretStr) -> SecretStr:
        if not v.get_secret_value().strip():
            raise ValueError("GROQ_API_KEY is empty: set it in backend/.env")
        return v

    @field_validator("groq_model")
    @classmethod
    def _model_present(cls, v: str) -> str:
        if not v.strip():
            raise ValueError("GROQ_MODEL is empty: set it in backend/.env (e.g. qwen/qwen3.8-27b)")
        return v.strip()

    def gateway_token_map(self) -> dict[str, str]:
        """token -> device_id."""
        out: dict[str, str] = {}
        for pair in filter(None, (p.strip() for p in self.gateway_tokens.split(","))):
            device_id, sep, token = pair.partition(":")
            if not sep or not device_id or len(token) < 24:
                raise ValueError(f"Bad GATEWAY_TOKENS entry for '{device_id}': need device_id:token (token >= 24 chars)")
            out[token] = device_id
        return out


@lru_cache
def get_settings() -> Settings:
    return Settings()  # type: ignore[call-arg]
