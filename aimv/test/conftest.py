# [AIMV] AIMV test package — shared pytest fixtures
import os
import tempfile
from pathlib import Path
from unittest.mock import patch

import pytest


@pytest.fixture
def tmp_dir():
    """Create a temporary directory for test outputs."""
    with tempfile.TemporaryDirectory(prefix="aimv-test-") as d:
        yield Path(d)


@pytest.fixture
def tmp_source_file(tmp_dir):
    """Create a temporary C source file."""
    src = tmp_dir / "test.c"
    src.write_text(
        'void process_task(int *a, int *b, int n) {\n'
        '  for (int i = 0; i < n; i++) {\n'
        '    a[i] = b[i+1] + b[i];\n'
        '  }\n'
        '}\n',
        encoding="utf-8",
    )
    return src


@pytest.fixture
def clean_env():
    """Remove all AIMV_ env vars for isolated config testing."""
    aimv_keys = [k for k in os.environ if k.startswith("AIMV_")]
    saved = {k: os.environ.pop(k) for k in aimv_keys}
    yield
    os.environ.update(saved)
    for k in aimv_keys:
        if k not in saved:
            os.environ.pop(k, None)


@pytest.fixture
def mock_clang(tmp_dir):
    """Create a mock clang that always succeeds."""
    mock = tmp_dir / "clang"
    mock.write_text('#!/bin/bash\necho "mock clang OK"\nexit 0\n')
    mock.chmod(0o755)
    return mock


@pytest.fixture
def aimv_json_sample():
    """Sample AIMV diagnostic JSON matching MCP_DESIGN format."""
    return {
        "request_id": "aimv-test-abc12345-r1",
        "target": {
            "triple": "armv7-unknown-linux-gnueabi",
            "cpu": "cortex-a9",
            "features": ["neon", "vfp3"],
            "vector_width": 128,
        },
        "diagnostics": [
            {
                "pass_name": "LoopVectorize",
                "remark_id": "CantReorderMemOps",
                "remark_text": "unsafe dependent memory operations in loop",
                "severity": "missed",
                "function_name": "process_task",
                "loop_location": "test.c:2:3",
                "source_context": "for (int i = 0; i < n; i++) { a[i] = b[i+1] + b[i]; }",
                "ir_snippet": "define void @process_task(...)",
                "cost_model": {
                    "scalar_cost": 24,
                    "vector_cost": 38,
                    "vf": 4,
                    "interleave_count": 1,
                },
                "dependencies": [
                    {
                        "dep_type": "Backward",
                        "source_ptr": "ptr %b + i + 1",
                        "sink_ptr": "ptr %a + i",
                        "alias_result": "unsafe: prevents vectorization",
                    }
                ],
                "memory_info": {
                    "num_stores": 1,
                    "num_loads": 2,
                    "num_pred_stores": None,
                    "max_alignment": 4,
                    "stride": "stride=1",
                    "memory_check_count": None,
                    "memory_check_cost": None,
                },
                "loop_info": {
                    "num_blocks": 3,
                    "num_instructions": 18,
                    "trip_count": -1,
                    "num_branches": 1,
                    "num_calls": 0,
                },
            },
            {
                "pass_name": "LoopVectorize",
                "remark_id": "LoopVectorized",
                "remark_text": "loop vectorized: VF=4",
                "severity": "passed",
                "function_name": "init_buf",
                "loop_location": "test.c:10:3",
                "source_context": "",
                "ir_snippet": "",
            },
        ],
    }
