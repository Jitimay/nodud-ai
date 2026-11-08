"""Phone number normalisation and sender filtering."""

import re

_DIGITS_RE = re.compile(r"\d+")
_E164_RE = re.compile(r"^\+\d{8,15}$")


def normalize(sender: str, default_country_code: str) -> str:
    """Normalise to E.164 where possible. Alphanumeric senders are returned unchanged.

    '+257 79 12 34 56' -> '+25779123456'
    '0025779123456'    -> '+25779123456'
    '79123456'         -> '+25779123456'   (8-digit Burundi local number)
    'Lumitel'          -> 'Lumitel'
    """
    s = sender.strip()
    compact = re.sub(r"[\s\-().]", "", s)
    if compact.startswith("00") and compact[2:].isdigit():
        return "+" + compact[2:]
    if compact.startswith("+") and compact[1:].isdigit():
        return compact
    if compact.isdigit():
        if compact.startswith(default_country_code) and len(compact) > 9:
            return "+" + compact
        if len(compact) == 8:
            return "+" + default_country_code + compact
        return compact  # short code or unknown format: kept as-is
    return s


def is_replyable(number: str) -> bool:
    """Only reply to real phone numbers.

    Operator promos, banks and short codes ('Lumitel', '161', ...) must never get
    an AI reply: it wastes credit and two auto-responders can loop forever.
    """
    return bool(_E164_RE.match(number))
