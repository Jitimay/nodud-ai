"""Lightweight language guess (Kirundi / French / English) for fixed system messages.

The AI itself is told to answer in the user's language; this guess is only used to
pick the language of canned replies (errors, help) and for statistics.
"""

import re

_WORD_RE = re.compile(r"[a-zàâçéèêëîïôûùüÿœ']+")

_MARKERS = {
    "rn": {
        "amahoro", "ni", "gute", "nshobora", "ndashaka", "canke", "kandi", "ukuntu", "iki",
        "ivyo", "vyose", "mwaramutse", "mwiriwe", "murakoze", "ndagukunda", "ego", "oya",
        "ubu", "none", "kubera", "ngo", "nde", "hehe", "ryari", "vyanje", "ibigori",
        "umuntu", "abantu", "amazi", "indwara", "kurinda", "uburundi", "bite",
    },
    "fr": {
        "le", "la", "les", "un", "une", "des", "est", "et", "je", "vous", "tu", "nous",
        "bonjour", "bonsoir", "merci", "comment", "pourquoi", "quand", "pour", "avec",
        "dans", "sur", "que", "qui", "quoi", "mon", "ma", "mes", "pas", "oui", "non",
        "salut", "aide", "s'il", "plait", "après",
    },
    "en": {
        "the", "is", "are", "what", "how", "why", "when", "hello", "hi", "thanks",
        "please", "my", "you", "and", "can", "help",
    },
}


def detect(text: str) -> str:
    words = _WORD_RE.findall(text.lower())
    if not words:
        return "unknown"
    scores = {lang: sum(w in marks for w in words) for lang, marks in _MARKERS.items()}
    best = max(scores, key=scores.get)  # type: ignore[arg-type]
    return best if scores[best] > 0 else "unknown"
