# [AIMV] Tests for aimv/mcp_server/suggestion_parser.py (T2.5)
import json
import pytest
from aimv.mcp_server.suggestion_parser import (
    parse_structured_response, SuggestionParseError, _extract_json,
)


VALID_RESPONSE = json.dumps({
    "suggestions": [{
        "description": "Add restrict",
        "reasoning": "Alias analysis failed",
        "source_file": "test.c",
        "line_start": 1,
        "line_end": 1,
        "original": "void f(int *a, int n)",
        "modified": "void f(int * restrict a, int n)",
        "diff": "--- a/test.c\n+++ b/test.c\n@@ -1 +1 @@\n-void f(int *a, int n)\n+void f(int * restrict a, int n)",
        "estimated_impact": "high",
    }],
    "overall_analysis": "Works",
    "confidence": 0.92,
    "no_action_possible": False,
})


class TestExtractJson:
    """T2.5: _extract_json uses brace-depth counting, not regex."""

    def test_extract_json_plain(self):
        text = '{"key": "value"}'
        assert _extract_json(text) == text

    def test_extract_json_markdown_wrapped(self):
        text = '```json\n{"key": "value"}\n```'
        result = _extract_json(text)
        assert '"key"' in result

    def test_extract_json_with_diff_backticks(self):
        """diff field containing triple backticks should not truncate."""
        data = {
            "suggestions": [{
                "description": "d", "reasoning": "r",
                "source_file": "f.c", "line_start": 1, "line_end": 1,
                "original": "x", "modified": "y",
                "diff": "```diff\n-sold\n+new\n```",
                "estimated_impact": "high",
            }],
            "overall_analysis": "a", "confidence": 0.5,
            "no_action_possible": False,
        }
        text = json.dumps(data)
        result = _extract_json(text)
        parsed = json.loads(result)
        assert "```diff" in parsed["suggestions"][0]["diff"]

    def test_extract_json_with_apology_prefix(self):
        text = "I'm sorry, here is the analysis:\n" + VALID_RESPONSE
        result = _extract_json(text)
        parsed = json.loads(result)
        assert parsed["confidence"] == 0.92

    def test_extract_json_no_json(self):
        result = _extract_json("no json here")
        assert result == "no json here"


class TestParseStructuredResponse:
    def test_valid_json(self):
        resp = parse_structured_response(VALID_RESPONSE, "req-1")
        assert resp.request_id == "req-1"
        assert len(resp.suggestions) == 1
        assert resp.confidence == 0.92

    def test_json_in_markdown_block(self):
        text = "```json\n" + VALID_RESPONSE + "\n```"
        resp = parse_structured_response(text, "req-1")
        assert resp.request_id == "req-1"

    def test_invalid_json_raises(self):
        with pytest.raises(SuggestionParseError):
            parse_structured_response("not json at all", "req-1")

    def test_no_action_possible(self):
        data = json.dumps({
            "suggestions": [],
            "overall_analysis": "Cannot fix",
            "confidence": 1.0,
            "no_action_possible": True,
        })
        resp = parse_structured_response(data, "req-1")
        assert resp.no_action_possible is True
        assert resp.suggestions == []

    def test_invalid_estimated_impact_raises(self):
        data = json.dumps({
            "suggestions": [{
                "description": "d", "reasoning": "r",
                "source_file": "f.c", "line_start": 1, "line_end": 1,
                "original": "x", "modified": "y", "diff": "---",
                "estimated_impact": "critical",
            }],
            "overall_analysis": "", "confidence": 0.5,
            "no_action_possible": False,
        })
        with pytest.raises(SuggestionParseError):
            parse_structured_response(data, "req-1")

    def test_parse_with_prefix_noise(self):
        text = "Here is the analysis:\n" + VALID_RESPONSE
        resp = parse_structured_response(text, "req-1")
        assert resp.request_id == "req-1"
