"""T0.5 — Config loading and validation tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.config import load_config, _validate


class TestConfigLoading:
    def test_load_default_config(self):
        cfg = load_config()
        assert cfg["max_rounds"] == 5
        assert cfg["aimv_level"] == "moderate"
        assert cfg["mcp_url"] == "http://localhost:8080"
        assert cfg["cc"] == "clang"

    def test_all_required_keys_present(self):
        cfg = load_config()
        required = ["max_rounds", "aimv_level", "mcp_url", "mcp_timeout_seconds",
                     "mcp_retry_count", "cache_enabled", "cc", "cflags",
                     "check_vectorization", "measure_perf", "output_dir"]
        for key in required:
            assert key in cfg, f"Missing key: {key}"


class TestConfigValidation:
    def test_valid_config_passes(self):
        _validate({"max_rounds": 5, "aimv_level": "moderate",
                    "mcp_timeout_seconds": 60, "mcp_retry_count": 2})

    def test_invalid_max_rounds_raises(self):
        with pytest.raises(ValueError, match="max_rounds"):
            _validate({"max_rounds": -1, "aimv_level": "moderate",
                        "mcp_timeout_seconds": 60, "mcp_retry_count": 2})

    def test_invalid_aimv_level_raises(self):
        with pytest.raises(ValueError, match="aimv_level"):
            _validate({"max_rounds": 5, "aimv_level": "extreme",
                        "mcp_timeout_seconds": 60, "mcp_retry_count": 2})

    def test_invalid_timeout_raises(self):
        with pytest.raises(ValueError, match="mcp_timeout_seconds"):
            _validate({"max_rounds": 5, "aimv_level": "moderate",
                        "mcp_timeout_seconds": 0, "mcp_retry_count": 2})

    def test_negative_retry_count_raises(self):
        with pytest.raises(ValueError, match="mcp_retry_count"):
            _validate({"max_rounds": 5, "aimv_level": "moderate",
                        "mcp_timeout_seconds": 60, "mcp_retry_count": -1})
