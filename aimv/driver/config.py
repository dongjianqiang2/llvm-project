# [AIMV] AIMV Driver configuration loader
import yaml
import os
from pathlib import Path
from typing import Optional

_DEFAULT_CONFIG_PATHS = [
    Path("aimv/config/aimv_config.yaml"),
    Path(os.path.expanduser("~/.config/aimv/aimv_config.yaml")),
]


def _find_config(config_path: Optional[str] = None) -> Path:
    if config_path:
        p = Path(config_path)
        if p.exists():
            return p
        raise FileNotFoundError(f"config not found: {config_path}")
    for p in _DEFAULT_CONFIG_PATHS:
        if p.exists():
            return p
    raise FileNotFoundError("no aimv_config.yaml found in default paths")


def load_config(config_path: Optional[str] = None) -> dict:
    """Load and validate AIMV configuration from YAML file.

    Returns a dict with defaults filled in for any missing keys.
    """
    path = _find_config(config_path)
    with open(path) as f:
        raw = yaml.safe_load(f) or {}

    aimv = raw.get("aimv", {})

    # Apply defaults
    cfg = {
        "max_rounds": aimv.get("max_rounds", 5),
        "aimv_level": aimv.get("aimv_level", "moderate"),
        "mcp_url": aimv.get("mcp", {}).get("url", "http://localhost:8080"),
        "mcp_timeout_seconds": aimv.get("mcp", {}).get("timeout_seconds", 60),
        "mcp_retry_count": aimv.get("mcp", {}).get("retry_count", 2),
        "cache_enabled": aimv.get("mcp", {}).get("cache_enabled", True),
        "cache_ttl_hours": aimv.get("mcp", {}).get("cache_ttl_hours", 24),
        "cc": aimv.get("build", {}).get("cc", "clang"),
        "cflags": aimv.get("build", {}).get(
            "cflags",
            "-O2 -fsave-optimization-record -g "
            "-Rpass=loop-vectorize -Rpass-missed=loop-vectorize "
            "-Rpass-analysis=loop-vectorize",
        ),
        "opt_record_format": aimv.get("build", {}).get("opt_record_format", "yaml"),
        "test_cmd": aimv.get("verify", {}).get("test_cmd", "make test"),
        "check_vectorization": aimv.get("verify", {}).get("check_vectorization", True),
        "measure_perf": aimv.get("verify", {}).get("measure_perf", False),
        "output_dir": aimv.get("output", {}).get("dir", "./aimv-output"),
        "keep_sessions": aimv.get("output", {}).get("keep_sessions", True),
        "log_level": aimv.get("output", {}).get("log_level", "info"),
    }

    _validate(cfg)
    return cfg


def _validate(cfg: dict):
    """Validate configuration values."""
    if not isinstance(cfg["max_rounds"], int) or cfg["max_rounds"] < 1:
        raise ValueError(f"max_rounds must be >= 1, got {cfg['max_rounds']}")
    if cfg["aimv_level"] not in ("conservative", "moderate", "aggressive"):
        raise ValueError(
            f"aimv_level must be conservative|moderate|aggressive, "
            f"got {cfg['aimv_level']}"
        )
    if cfg["mcp_timeout_seconds"] < 1:
        raise ValueError(
            f"mcp_timeout_seconds must be >= 1, got {cfg['mcp_timeout_seconds']}"
        )
    if cfg["mcp_retry_count"] < 0:
        raise ValueError(
            f"mcp_retry_count must be >= 0, got {cfg['mcp_retry_count']}"
        )
