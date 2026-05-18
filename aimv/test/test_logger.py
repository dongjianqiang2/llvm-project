# [AIMV] Tests for aimv/driver/logger.py (T0.4)
import logging

import pytest
from aimv.driver.logger import setup_logger, get_logger


class TestLoggerFormat:
    """Logger output must include [AIMV] prefix."""

    def test_logger_has_aimv_prefix(self, capsys):
        logger = setup_logger("test_aimv_logger")
        logger.info("test message")
        captured = capsys.readouterr()
        assert "[AIMV]" in captured.err

    def test_get_logger_returns_same(self):
        setup_logger("test_aimv_same")
        l1 = get_logger("test_aimv_same")
        l2 = get_logger("test_aimv_same")
        assert l1 is l2

    def test_logger_level(self):
        logger = setup_logger("test_aimv_level", level="debug")
        assert logger.level == logging.DEBUG
