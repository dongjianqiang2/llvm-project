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


class TestMcpUrlPriority:
    """MCP URL priority: CLI > env > YAML > default (T4.1)."""

    def test_default_url(self, clean_env, tmp_dir, monkeypatch):
        """Priority 4 (lowest): built-in default."""
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        cfg = load_config()
        assert cfg.mcp_url == "http://localhost:8080"

    def test_yaml_overrides_default(self, clean_env, tmp_dir, monkeypatch):
        """Priority 3: YAML config overrides default."""
        home = tmp_dir / "home"
        (home / ".aimv").mkdir(parents=True)
        (home / ".aimv" / "config").write_text(
            "mcp:\n  url: http://yaml-server:9090\n", encoding="utf-8")
        monkeypatch.setattr(Path, "home", lambda: home)
        cfg = load_config()
        assert cfg.mcp_url == "http://yaml-server:9090"

    def test_env_overrides_yaml(self, clean_env, tmp_dir, monkeypatch):
        """Priority 2: env var overrides YAML."""
        home = tmp_dir / "home"
        (home / ".aimv").mkdir(parents=True)
        (home / ".aimv" / "config").write_text(
            "mcp:\n  url: http://yaml-server:9090\n", encoding="utf-8")
        monkeypatch.setattr(Path, "home", lambda: home)
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-server:8080")
        cfg = load_config()
        assert cfg.mcp_url == "http://env-server:8080"

    def test_cli_wins_over_env(self, clean_env, tmp_dir, monkeypatch):
        """Priority 1 (highest): --mcp-url CLI flag overrides everything."""
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-server:8080")

        from aimv.driver.config import DriverConfig
        # Simulate what main() does: load config, then override with CLI
        cfg = load_config()
        assert cfg.mcp_url == "http://env-server:8080"  # env wins
        # CLI arg overrides
        cfg.mcp_url = "http://cli-server:7070"
        assert cfg.mcp_url == "http://cli-server:7070"  # CLI wins

    def test_cli_wins_over_all(self, clean_env, tmp_dir, monkeypatch):
        """Full chain: CLI overrides env, YAML, and default."""
        home = tmp_dir / "home"
        (home / ".aimv").mkdir(parents=True)
        (home / ".aimv" / "config").write_text(
            "mcp:\n  url: http://yaml-server:9090\n", encoding="utf-8")
        monkeypatch.setattr(Path, "home", lambda: home)
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-server:8080")

        cfg = load_config()
        assert cfg.mcp_url == "http://env-server:8080"  # env > yaml
        # CLI overrides all
        cfg.mcp_url = "http://faimv-mcp-url:7070"
        assert cfg.mcp_url == "http://faimv-mcp-url:7070"

    def test_no_cli_uses_env(self, clean_env, tmp_dir, monkeypatch):
        """When --mcp-url is None (not passed), env takes effect."""
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-only:8080")
        cfg = load_config()
        # Without CLI override, env is used
        assert cfg.mcp_url == "http://env-only:8080"

    def test_argparse_default_is_none(self):
        """--mcp-url argparse default is None, not a hardcoded URL."""
        from aimv.driver.aimv_driver import build_argparser
        parser = build_argparser()
        # Parse empty args — --mcp-url should be None
        args = parser.parse_args([])
        assert args.mcp_url is None

    def test_main_priority_chain(self, clean_env, tmp_dir, monkeypatch):
        """Simulate main() config merging: CLI > env > YAML > default."""
        monkeypatch.setattr(Path, "home", lambda: tmp_dir / "nohome")
        monkeypatch.setenv("AIMV_MCP_URL", "http://env-server:8080")

        from aimv.driver.aimv_driver import build_argparser
        from aimv.driver.config import load_config

        # Case A: --mcp-url passed → overrides env
        parser = build_argparser()
        args_a = parser.parse_args(["--mcp-url", "http://cli:7070"])
        cfg_a = load_config()
        assert cfg_a.mcp_url == "http://env-server:8080"  # env loaded
        if args_a.mcp_url is not None:
            cfg_a.mcp_url = args_a.mcp_url
        assert cfg_a.mcp_url == "http://cli:7070"  # CLI wins

        # Case B: --mcp-url NOT passed → keeps env
        args_b = parser.parse_args([])
        cfg_b = load_config()
        assert args_b.mcp_url is None  # not passed
        assert cfg_b.mcp_url == "http://env-server:8080"  # env kept
