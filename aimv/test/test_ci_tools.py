"""T6.3-T6.4 — CI report and gate tests."""
import sys
import json
import tempfile
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.ci.aimv_report import generate_markdown
from aimv.ci.aimv_gate import evaluate_gate


@pytest.fixture
def sessions_dir():
    d = tempfile.mkdtemp(prefix="aimv-test-sessions-")
    sessions_dir = Path(d) / "sessions"
    sessions_dir.mkdir()
    # Create 3 session JSON files
    s1 = {"session_id": "s1", "function_name": "func_a",
          "termination_reason": "vectorized", "rounds": [{"round_number": 1}]}
    s2 = {"session_id": "s2", "function_name": "func_b",
          "termination_reason": "no_suggestion", "rounds": []}
    s3 = {"session_id": "s3", "function_name": "func_c",
          "termination_reason": "round_limit", "rounds": [{}, {}, {}]}
    for s in [s1, s2, s3]:
        (sessions_dir / f"{s['session_id']}.json").write_text(json.dumps(s))
    yield str(Path(d))
    import shutil
    shutil.rmtree(d, ignore_errors=True)


class TestReportGeneration:
    def test_generates_markdown(self, sessions_dir):
        md = generate_markdown(sessions_dir)
        assert "AIMV Vectorization Analysis" in md
        assert "func_a" in md
        assert "vectorized" in md.lower()

    def test_includes_success_rate(self, sessions_dir):
        md = generate_markdown(sessions_dir)
        assert "Success rate" in md

    def test_no_sessions_does_not_crash(self):
        d = tempfile.mkdtemp(prefix="empty-")
        try:
            md = generate_markdown(d)
            assert "AIMV" in md
        finally:
            import shutil
            shutil.rmtree(d, ignore_errors=True)


class TestGateDecision:
    def test_report_mode_always_allows(self):
        result = evaluate_gate({}, {"mode": "report"})
        assert result["allow"] is True

    def test_regression_no_baseline_allows(self):
        summary = {"details": [{"function": "f", "perf_improvement_pct": 5.0}]}
        result = evaluate_gate(summary, {"mode": "regression",
                                          "regression": {"perf_degradation_threshold_pct": 5.0}})
        assert result["allow"] is True

    def test_regression_with_degradation_blocks(self):
        summary = {"details": [{"function": "f", "perf_improvement_pct": -10.0}]}
        baseline = {"details": [{"function": "f", "perf_improvement_pct": 5.0}]}
        result = evaluate_gate(summary, {"mode": "regression",
                                          "regression": {"perf_degradation_threshold_pct": 5.0}},
                                baseline)
        assert result["allow"] is False

    def test_enforce_below_threshold_blocks(self):
        summary = {"details": [
            {"file": "src/dsp/kernel.c", "function": "f1", "status": "vectorized"},
            {"file": "src/dsp/kernel.c", "function": "f2", "status": "no_suggestion"},
        ]}
        config = {"mode": "enforce", "enforce": {"rules": [
            {"path_pattern": "src/dsp/**/*.c", "min_vectorization_rate": 0.6}
        ]}}
        result = evaluate_gate(summary, config)
        # 1/2 = 50% < 60%
        assert result["allow"] is False

    def test_enforce_above_threshold_allows(self):
        summary = {"details": [
            {"file": "src/dsp/kernel.c", "function": "f1", "status": "vectorized"},
            {"file": "src/dsp/kernel.c", "function": "f2", "status": "vectorized"},
            {"file": "src/dsp/kernel.c", "function": "f3", "status": "no_suggestion"},
        ]}
        config = {"mode": "enforce", "enforce": {"rules": [
            {"path_pattern": "src/dsp/**/*.c", "min_vectorization_rate": 0.6}
        ]}}
        result = evaluate_gate(summary, config)
        # 2/3 = 67% >= 60%
        assert result["allow"] is True
