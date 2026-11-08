"""Groq chat-completion wrapper: prompt assembly, timeouts, retries, error mapping."""

import json
import logging
import time
from dataclasses import dataclass
from pathlib import Path

import groq

from app.core.config import Settings

log = logging.getLogger(__name__)

_PROMPTS = Path(__file__).resolve().parent.parent / "prompts"


class AIUnavailableError(Exception):
    """Groq could not produce an answer (timeout, rate limit, outage, bad config)."""


@dataclass(frozen=True)
class AIReply:
    text: str
    model: str
    latency_s: float
    prompt_tokens: int | None
    completion_tokens: int | None


def _load_examples() -> list[dict[str, str]]:
    """Optional few-shot examples (e.g. native-speaker Kirundi Q&A) from prompts/examples.json:
    [{"user": "...", "assistant": "..."}, ...]"""
    path = _PROMPTS / "examples.json"
    if not path.exists():
        return []
    msgs: list[dict[str, str]] = []
    for ex in json.loads(path.read_text(encoding="utf-8")):
        msgs += [{"role": "user", "content": ex["user"]}, {"role": "assistant", "content": ex["assistant"]}]
    return msgs


class GroqService:
    def __init__(self, settings: Settings):
        self._s = settings
        self._client = groq.Groq(
            api_key=settings.groq_api_key.get_secret_value(),
            timeout=settings.groq_timeout_s,
            max_retries=settings.groq_max_retries,  # SDK retries 429/5xx/connection errors with backoff
        )
        self._system = (_PROMPTS / "system_prompt.txt").read_text(encoding="utf-8").format(
            target_chars=settings.sms_target_chars
        )
        self._examples = _load_examples()

    def generate(self, history: list[dict[str, str]], user_text: str) -> AIReply:
        messages = [{"role": "system", "content": self._system}, *self._examples, *history,
                    {"role": "user", "content": user_text}]
        kwargs: dict = {
            "model": self._s.groq_model,
            "messages": messages,
            "temperature": self._s.groq_temperature,
            "max_completion_tokens": self._s.groq_max_completion_tokens,
        }
        if self._s.groq_reasoning_effort:
            kwargs["reasoning_effort"] = self._s.groq_reasoning_effort

        t0 = time.monotonic()
        try:
            resp = self._client.chat.completions.create(**kwargs)
        except groq.AuthenticationError as e:
            log.critical("Groq rejected the API key: check GROQ_API_KEY")
            raise AIUnavailableError("auth") from e
        except groq.NotFoundError as e:
            log.critical("Groq model %r not found: check GROQ_MODEL", self._s.groq_model)
            raise AIUnavailableError("model") from e
        except groq.RateLimitError as e:
            log.warning("Groq rate limit hit (after SDK retries)")
            raise AIUnavailableError("rate_limit") from e
        except groq.APITimeoutError as e:
            log.warning("Groq timeout after %.0fs", self._s.groq_timeout_s)
            raise AIUnavailableError("timeout") from e
        except groq.APIError as e:
            log.error("Groq API error: %s", e)
            raise AIUnavailableError("api") from e

        latency = time.monotonic() - t0
        text = (resp.choices[0].message.content or "").strip()
        if not text:
            raise AIUnavailableError("empty")
        usage = resp.usage
        return AIReply(
            text=text,
            model=resp.model,
            latency_s=latency,
            prompt_tokens=usage.prompt_tokens if usage else None,
            completion_tokens=usage.completion_tokens if usage else None,
        )
