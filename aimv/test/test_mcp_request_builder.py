# [AIMV] T3.6 — MCP request builder tests
import pytest
from aimv.driver.aimv_driver import build_mcp_request, build_history
from aimv.driver.config import DriverConfig
from aimv.driver.models import (
    PerFunctionResult, RoundRecord, TerminationReason,
    IterationStatus, BuildResult,
)


@pytest.fixture
def source_file(tmp_path):
    f = tmp_path / "task.c"
    f.write_text("void foo(int *a, int *b) { for(int i=0;i<100;i++) a[i]=b[i]; }\n")
    return str(f)


@pytest.fixture
def config():
    return DriverConfig()


DIAGNOSTICS = [
    {
        "function_name": "foo",
        "severity": "missed",
        "remark_id": "CantReorderMemOps",
        "remark_text": "cant reorder",
        "loop_location": "task.c:1:42",
    },
    {
        "function_name": "foo",
        "severity": "passed",
        "remark_id": "LoopVectorized",
        "remark_text": "vectorized",
        "loop_location": "task.c:2:1",
    },
    {
        "function_name": "bar",
        "severity": "missed",
        "remark_id": "UnsafeDep",
        "remark_text": "unsafe",
        "loop_location": "task.c:10:5",
    },
]


class TestBuildMcpRequest:
    def test_request_id_unique(self, source_file, config):
        """Same function, different rounds → different request_id."""
        r1 = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative",
            config, session_id="sess1", round_num=1)
        r2 = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative",
            config, session_id="sess1", round_num=2)
        assert r1["request_id"] != r2["request_id"]
        assert "r1" in r1["request_id"]
        assert "r2" in r2["request_id"]

    def test_loop_line_parse(self, source_file, config):
        """loop_location="task.c:42:5" → loop_line=42."""
        diags = [{
            "function_name": "foo",
            "severity": "missed",
            "remark_id": "test",
            "remark_text": "test",
            "loop_location": "task.c:42:5",
        }]
        req = build_mcp_request(
            "foo", source_file, diags, {}, [], "conservative", config)
        assert req["function"]["loop_line"] == 42

    def test_loop_line_windows_path(self, source_file, config):
        """Windows path "C:\\src\\file.c:42:5" → loop_line=42 (no crash)."""
        diags = [{
            "function_name": "foo",
            "severity": "missed",
            "remark_id": "test",
            "remark_text": "test",
            "loop_location": "C:\\src\\file.c:42:5",
        }]
        req = build_mcp_request(
            "foo", source_file, diags, {}, [], "conservative", config)
        assert req["function"]["loop_line"] == 42

    def test_diagnostics_filtered_by_function(self, source_file, config):
        """Only missed diagnostics for target function are included."""
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative", config)
        assert len(req["diagnostics"]) == 1
        assert req["diagnostics"][0]["remark_id"] == "CantReorderMemOps"

    def test_source_code_current(self, source_file, config):
        """source_code is the current file content."""
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative", config)
        with open(source_file) as f:
            assert req["function"]["source_code"] == f.read()

    def test_aimv_level_set(self, source_file, config):
        """aimv_level matches the passed level."""
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "aggressive", config)
        assert req["aimv_level"] == "aggressive"

    def test_target_from_param(self, source_file, config):
        """Target comes from parameter when provided."""
        target = {"triple": "armv7-unknown-linux-gnueabi", "cpu": "cortex-a9",
                  "features": ["neon"], "vector_width": 128}
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, target, [], "conservative", config)
        assert req["target"]["triple"] == "armv7-unknown-linux-gnueabi"

    def test_target_default_when_empty(self, source_file, config):
        """Empty target → default with empty triple."""
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative", config)
        assert req["target"]["triple"] == ""

    def test_request_id_contains_func_hash(self, source_file, config):
        """request_id contains function name hash prefix."""
        req = build_mcp_request(
            "foo", source_file, DIAGNOSTICS, {}, [], "conservative",
            config, session_id="test-session", round_num=1)
        assert "aimv-test-session-" in req["request_id"]
        assert "-r1" in req["request_id"]


class TestBuildHistory:
    def test_history_last_3(self):
        """5 rounds → only last 3 sent."""
        pfr = PerFunctionResult(function_name="foo")
        for i in range(1, 6):
            rr = RoundRecord(round_number=i)
            rr.finished_at = 1.0
            rr.diagnostics_json = {"diagnostics": [{"remark_id": f"r{i}"}]}
            rr.applied_diff_summary = f"change {i}"
            pfr.rounds.append(rr)

        history = build_history(pfr, max_entries=3)
        assert len(history) == 3
        assert history[0]["round"] == 3
        assert history[2]["round"] == 5

    def test_history_empty(self):
        """No finished rounds → empty history."""
        pfr = PerFunctionResult(function_name="foo")
        history = build_history(pfr)
        assert history == []

    def test_history_outcome_compile_failed(self):
        """Round with failed verify_build → "compile_failed" outcome."""
        pfr = PerFunctionResult(function_name="foo")
        rr = RoundRecord(round_number=1)
        rr.finished_at = 1.0
        rr.verify_build = BuildResult(
            returncode=1, stdout="", stderr="error",
            opt_record_path="", aimv_json_path="", elapsed_ms=100)
        pfr.rounds.append(rr)
        history = build_history(pfr)
        assert history[0]["outcome"] == "compile_failed"
