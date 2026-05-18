# [AIMV] AIMV Driver configuration loader
# Priority: env vars > ~/.aimv/config > defaults (DRIVER_DESIGN §8.1)
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml


@dataclass
class DriverConfig:
    mcp_url: str = "http://localhost:8080"
    mcp_timeout: int = 60
    mcp_api_key: str = ""
    aimv_level: str = "conservative"
    max_rounds: int = 5
    test_cmd: str = ""
    cc: str = "clang"
    cflags: list = field(default_factory=lambda: ["-O2"])
    output_dir: str = "./aimv-output"
    aimv_mode: str = "auto"  # auto | review


def load_config() -> DriverConfig:
    """Load configuration with priority: env vars > ~/.aimv/config > defaults."""
    config = DriverConfig()

    # Layer 1: ~/.aimv/config (YAML)
    config_path = Path.home() / ".aimv" / "config"
    if config_path.exists():
        with open(config_path) as f:
            file_config = yaml.safe_load(f) or {}

        mcp_cfg = file_config.get("mcp", {})
        driver_cfg = file_config.get("driver", {})

        if "url" in mcp_cfg:
            config.mcp_url = mcp_cfg["url"]
        if "api_key" in mcp_cfg:
            config.mcp_api_key = mcp_cfg["api_key"]
        if "timeout_seconds" in mcp_cfg:
            config.mcp_timeout = mcp_cfg["timeout_seconds"]
        if "max_rounds" in driver_cfg:
            config.max_rounds = driver_cfg["max_rounds"]
        if "aimv_level" in driver_cfg:
            config.aimv_level = driver_cfg["aimv_level"]
        if "test_cmd" in driver_cfg:
            config.test_cmd = driver_cfg["test_cmd"]
        if "cc" in driver_cfg:
            config.cc = driver_cfg["cc"]
        if "cflags" in driver_cfg:
            config.cflags = driver_cfg["cflags"].split() if isinstance(driver_cfg["cflags"], str) else driver_cfg["cflags"]

    # Layer 2: Environment variables override
    if env_url := os.environ.get("AIMV_MCP_URL"):
        config.mcp_url = env_url
    if env_api_key := os.environ.get("AIMV_MCP_API_KEY"):
        config.mcp_api_key = env_api_key
    if env_level := os.environ.get("AIMV_LEVEL"):
        config.aimv_level = env_level
    if env_rounds := os.environ.get("AIMV_MAX_ROUNDS"):
        config.max_rounds = int(env_rounds)
    if env_test := os.environ.get("AIMV_TEST_CMD"):
        config.test_cmd = env_test
    if env_mode := os.environ.get("AIMV_MODE"):
        config.aimv_mode = env_mode

    # Validate (raise ValueError, not assert — python -O skips assert)
    if config.aimv_level not in ("conservative", "moderate", "aggressive"):
        raise ValueError(f"invalid aimv_level: {config.aimv_level}")
    if config.max_rounds <= 0:
        raise ValueError(f"max_rounds must be > 0: {config.max_rounds}")

    return config
