"""Fixed (non-AI) replies, per language.

TODO(native speaker): the Kirundi strings below were not written by a native
speaker and must be reviewed before real users see them.
"""

_TEXTS = {
    "ai_error": {
        "rn": "Mutubabarire, ntidushoboye kwishura ubu. Mwongere mugerageze haciye akanya.",
        "fr": "Désolé, nous ne pouvons pas répondre pour le moment. Réessayez dans quelques minutes.",
        "en": "Sorry, we cannot answer right now. Please try again in a few minutes.",
    },
    "reset": {
        "rn": "Ikiganiro gishasha catanguye. Mubaze ico mushaka.",
        "fr": "Nouvelle conversation commencée. Posez votre question.",
        "en": "New conversation started. Ask your question.",
    },
    "empty": {
        "rn": "Mwandike ikibazo canyu muri SMS.",
        "fr": "Écrivez votre question dans le SMS.",
        "en": "Please write your question in the SMS.",
    },
}

# Bilingual fallback when the language is unknown: Burundi users read French or Kirundi.
_FALLBACK_ORDER = ("fr", "rn")


def get(key: str, language: str) -> str:
    texts = _TEXTS[key]
    if language in texts:
        return texts[language]
    return " / ".join(texts[lang] for lang in _FALLBACK_ORDER)


# Commands are matched on the whole (trimmed, upper-cased) SMS body.
RESET_COMMANDS = {"RESET", "NOUVEAU", "NEW", "GUTANGURA"}
