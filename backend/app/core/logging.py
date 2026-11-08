"""Logging setup. Phone numbers are masked in logs to protect users' privacy."""

import logging


def setup_logging(level: str) -> None:
    logging.basicConfig(
        level=level.upper(),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    logging.getLogger("httpx").setLevel(logging.WARNING)


def mask_phone(number: str) -> str:
    """+25779123456 -> +257****3456"""
    if len(number) <= 6:
        return "***"
    return f"{number[:4]}****{number[-4:]}"
