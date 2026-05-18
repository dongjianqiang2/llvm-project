# [AIMV] Tests for aimv/driver/config.py (T0.2)
import os
from pathlib import Path

import pytest
from aimv.driver.config import DriverConfig, load_config


class TestConfigDefaults:
    """No env vars, no config file → all defaults."""

    def test_config_defaults(self, clean_env, tmp_dir, monkeypatch):
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        cfg = load_config()
        assert cfg.mcp_url == "http://localhost:8080"
        assert cfg.aimv_level == "conservative"
        assert cfg.max_rounds == 5
        assert cfg.test_cmd == ""
        assert cfg.cc == "clang"
        assert cfg.aimv_mode == "auto"
        assert cfg.mcp_api_key == ""


class TestConfigYaml:
    """~/.aimv/config YAML is read."""

    def test_config_yaml(self, clean_env, tmp_dir, monkeypatch):
        home = tmp_dir / "home"
        aimv_dir = home / ".aimv"
        aimv_dir.mkdir(parents=True)
        (aimv_dir / "config").write_text(
            "mcp:\n  url: http://aimv-server:8080\n  api_key: testkey\n"
            "driver:\n  max_rounds: 10\n  aimv_level: moderate\n  test_cmd: make test\n",
            encoding="utf-8",
        )
        monkeypatch.setattr(Path, "home", lambda: home)
        cfg = load_config()
        assert cfg.mcp_url == "http://aimv-server:8080"
        assert cfg.mcp_api_key == "testkey"
        assert cfg.max_rounds == 10
        assert cfg.aimv_level == "moderate"
        assert cfg.test_cmd == "make test"


class TestConfigEnvOverride:
    """Env vars override YAML config."""

    def test_config_env_override(self, clean_env, tmp_dir, monkeypatch):
        home = tmp_dir / "home"
        aimv_dir = home / ".aimv"
        aimv_dir.mkdir(parents=True)
        (aimv_dir / "config").write_text(
            "mcp:\n  url: http://yaml-url:8080\n"
            "driver:\n  aimv_level: moderate\n",
            encoding="utf-8",
        )
        monkeypatch.setattr(Path, "home", lambda: home)
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-url:9090")
        monkeypatch.setenv("AIMV_LEVEL", "aggressive")
        monkeypatch.setenv("AIMV_MAX_ROUNDS", "20")
        monkeypatch.setenv("AIMV_MCP_API_KEY", "envkey")
        monkeypatch.setenv("AIMV_MODE", "review")
        cfg = load_config()
        assert cfg.mcp_url == "http://env-url:9090"
        assert cfg.aimv_level == "aggressive"
        assert cfg.max_rounds == 20
        assert cfg.mcp_api_key == "envkey"
        assert cfg.aimv_mode == "review"


class TestConfigValidation:
    """Invalid config values raise ValueError."""

    def test_config_invalid_level(self, clean_env, tmp_dir, monkeypatch):
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_LEVEL", "invalid")
        with pytest.raises(ValueError, match="invalid aimv_level"):
            load_config()

    def test_config_invalid_max_rounds(self, clean_env, tmp_dir, monkeypatch):
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_MAX_ROUNDS", "0")
        with pytest.raises(ValueError, match="max_rounds must be > 0"):
            load_config()

    def test_config_negative_max_rounds(self, clean_env, tmp_dir, monkeypatch):
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_MAX_ROUNDS", "-1")
        with pytest.raises(ValueError, match="max_rounds must be > 0"):
            load_config()
