"""T3.10 — OptInfoParser (YAML mode) tests."""
import sys
import tempfile
import yaml
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.opt_info_parser import parse_to_analyze_request


@pytest.fixture
def yaml_file():
    records = [
        {"Function": "foo", "Pass": "loop-vectorize",
         "Name": "CantReorderMemOps", "type": "Missed",
         "DebugLoc": "test.c:5:3"},
        {"Function": "bar", "Pass": "loop-vectorize",
         "Name": "Vectorized", "type": "Passed",
         "DebugLoc": "test.c:10:3"},
    ]
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(records, f)
        path = f.name
    yield path
    Path(path).unlink(missing_ok=True)


class TestOptInfoParser:
    def test_parse_missed_diagnostic(self, yaml_file):
        result = parse_to_analyze_request(yaml_file, "foo", "test.c")
        assert len(result["diagnostics"]) == 1
        assert result["diagnostics"][0]["severity"] == "missed"
        assert result["diagnostics"][0]["remark_id"] == "CantReorderMemOps"
        assert result["diagnostics"][0]["loop_location"] == "test.c:5:3"

    def test_parse_passed_diagnostic(self, yaml_file):
        result = parse_to_analyze_request(yaml_file, "bar", "test.c")
        assert len(result["diagnostics"]) == 1
        assert result["diagnostics"][0]["severity"] == "passed"

    def test_no_matching_function(self, yaml_file):
        result = parse_to_analyze_request(yaml_file, "nonexistent", "test.c")
        assert len(result["diagnostics"]) == 0

    def test_output_has_required_fields(self, yaml_file):
        result = parse_to_analyze_request(yaml_file, "foo", "test.c")
        assert "request_id" in result
        assert "target" in result
        assert "diagnostics" in result
        d = result["diagnostics"][0]
        for key in ["pass_name", "remark_text", "severity", "function_name",
                     "loop_location", "source_context", "ir_snippet",
                     "cost_model", "dependencies", "memory_info", "loop_info"]:
            assert key in d, f"Missing key: {key}"
        # YAML mode has no cost model or dependencies
        assert d["cost_model"] is None
        assert d["dependencies"] == []
