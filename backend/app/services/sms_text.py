"""SMS text preparation: cleanup, GSM-7 transliteration, segment counting, truncation.

Why this matters: a single character outside the GSM-7 alphabet (e.g. French 'ê')
switches the whole SMS to UCS-2, cutting capacity from 160 to 70 characters per
SMS. Each segment costs money, so we transliterate to GSM-7 where it loses nothing
important and cap replies at a fixed number of segments.
"""

import math
import re
import unicodedata
from dataclasses import dataclass

# 3GPP TS 23.038 default alphabet (basic table), excluding ESC (0x1B).
GSM7_BASIC = set(
    "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?"
    "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà"
)
# Extension table: each costs 2 septets (ESC + char).
GSM7_EXT = set("^{}\\[~]|€\f")

GSM7_SINGLE, GSM7_MULTI = 160, 153  # septets
UCS2_SINGLE, UCS2_MULTI = 70, 67  # UTF-16 code units

# Characters common in AI output / French that have a close GSM-7 equivalent.
_TRANSLIT = {
    "’": "'", "‘": "'", "‚": "'", "´": "'", "`": "'",
    "“": '"', "”": '"', "„": '"', "«": '"', "»": '"',
    "–": "-", "—": "-", "‑": "-", "−": "-",
    "…": "...", "•": "-", "·": "-", "×": "x",
    " ": " ", " ": " ", " ": " ",
    "ç": "c", "œ": "oe", "Œ": "OE",
    "≈": "~", "°": " deg",
}

_THINK_RE = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_MD_BOLD_RE = re.compile(r"(\*\*|__)(.+?)\1")
_MD_ITAL_RE = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])")
_MD_HEADER_RE = re.compile(r"^\s{0,3}#{1,6}\s*", re.MULTILINE)
_MD_BULLET_RE = re.compile(r"^\s*[*•]\s+", re.MULTILINE)
_MD_CODE_RE = re.compile(r"`{1,3}")
_MD_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
_SPACES_RE = re.compile(r"[ \t]+")
_BLANKLINES_RE = re.compile(r"\n{3,}")


@dataclass(frozen=True)
class SmsText:
    text: str
    encoding: str  # "GSM7" | "UCS2"
    segments: int
    truncated: bool


def clean_model_output(text: str) -> str:
    """Remove reasoning blocks and markdown; SMS is plain text."""
    text = _THINK_RE.sub("", text)
    text = _MD_LINK_RE.sub(r"\1 (\2)", text)
    text = _MD_BOLD_RE.sub(r"\2", text)
    text = _MD_ITAL_RE.sub(r"\1", text)
    text = _MD_HEADER_RE.sub("", text)
    text = _MD_BULLET_RE.sub("- ", text)
    text = _MD_CODE_RE.sub("", text)
    text = _SPACES_RE.sub(" ", text)
    text = "\n".join(line.strip() for line in text.splitlines())
    text = _BLANKLINES_RE.sub("\n\n", text)
    return text.strip()


def to_gsm7(text: str) -> str:
    """Best-effort transliteration to the GSM-7 alphabet. Characters with no
    reasonable equivalent are left as-is (the message then goes out as UCS-2)."""
    out = []
    for ch in text:
        if ch in GSM7_BASIC or ch in GSM7_EXT:
            out.append(ch)
        elif ch in _TRANSLIT:
            out.append(_TRANSLIT[ch])
        else:
            # Strip accents: ê -> e, â -> a, î -> i, ô -> o, û -> u, ë -> e, ï -> i
            base = "".join(c for c in unicodedata.normalize("NFD", ch) if not unicodedata.combining(c))
            out.append(base if base and all(c in GSM7_BASIC for c in base) else ch)
    return "".join(out)


def is_gsm7(text: str) -> bool:
    return all(ch in GSM7_BASIC or ch in GSM7_EXT for ch in text)


def _gsm7_len(text: str) -> int:
    return sum(2 if ch in GSM7_EXT else 1 for ch in text)


def _ucs2_len(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def measure(text: str) -> tuple[str, int]:
    """Returns (encoding, segments)."""
    if is_gsm7(text):
        n = _gsm7_len(text)
        return "GSM7", 1 if n <= GSM7_SINGLE else math.ceil(n / GSM7_MULTI)
    n = _ucs2_len(text)
    return "UCS2", 1 if n <= UCS2_SINGLE else math.ceil(n / UCS2_MULTI)


def _capacity(encoding: str, max_segments: int) -> int:
    if encoding == "GSM7":
        return GSM7_SINGLE if max_segments == 1 else GSM7_MULTI * max_segments
    return UCS2_SINGLE if max_segments == 1 else UCS2_MULTI * max_segments


def _truncate(text: str, encoding: str, limit: int) -> str:
    length = _gsm7_len if encoding == "GSM7" else _ucs2_len
    ellipsis = "..."
    budget = limit - length(ellipsis)
    # Longest prefix that fits.
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if length(text[:mid]) <= budget:
            lo = mid
        else:
            hi = mid - 1
    cut = text[:lo]
    # Prefer ending on a sentence, then on a word, if that doesn't waste too much.
    for sep in (". ", "! ", "? ", "\n"):
        idx = cut.rfind(sep)
        if idx >= len(cut) * 0.6:
            return cut[: idx + 1].rstrip()
    idx = cut.rfind(" ")
    if idx >= len(cut) * 0.8:
        cut = cut[:idx]
    return cut.rstrip(" ,;:-") + ellipsis


def prepare_sms(text: str, max_segments: int) -> SmsText:
    """Clean, transliterate, and fit `text` into at most `max_segments` SMS."""
    text = to_gsm7(clean_model_output(text))
    encoding, segments = measure(text)
    truncated = False
    if segments > max_segments:
        text = _truncate(text, encoding, _capacity(encoding, max_segments))
        encoding, segments = measure(text)
        truncated = True
    return SmsText(text=text, encoding=encoding, segments=segments, truncated=truncated)
