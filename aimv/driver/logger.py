# [AIMV] AIMV Driver — Unified logging with [AIMV] prefix
import logging
import sys


def setup_logger(name: str = "aimv", level: str = "info") -> logging.Logger:
    """Create a logger with [AIMV] prefix formatting."""
    logger = logging.getLogger(name)
    if logger.handlers:
        return logger

    logger.setLevel(getattr(logging, level.upper(), logging.INFO))

    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(logging.Formatter("[AIMV] %(message)s"))
    logger.addHandler(handler)
    return logger


def get_logger(name: str = "aimv") -> logging.Logger:
    return logging.getLogger(name)
