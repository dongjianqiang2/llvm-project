# [AIMV] MCP Server — LLM response → structured Suggestion parser
import json
import re
from .models import AnalyzeResponse


class SuggestionParseError(Exception):
    """LLM response could not be parsed as valid suggestion."""


RESPONSE_SCHEMA_JSON = """\
{
  "suggestions": [
    {
      "description": "<one-line summary>",
      "reasoning": "<detailed analysis>",
      "source_file": "<path>",
      "line_start": <int>,
      "line_end": <int>,
      "original": "<original source text>",
      "modified": "<modified source text>",
      "diff": "<unified diff>",
      "estimated_impact": "high|medium|low",
      "safety_concern": "<null or string>"
    }
  ],
  "overall_analysis": "<summary paragraph>",
  "confidence": <float 0.0-1.0>,
  "no_action_possible": false
}
"""


def parse_structured_response(raw_text: str, request_id: str) -> AnalyzeResponse:
    """Parse LLM raw output text into AnalyzeResponse.

    Handles common LLM output format issues:
    - JSON wrapped in ```json ... ``` code blocks
    - Trailing commas
    - Prefix noise ("Here is the response: ...")
    """

    json_text = _extract_json(raw_text)

    try:
        data = json.loads(json_text)
    except json.JSONDecodeError as e:
        raise SuggestionParseError(f"JSON parse failed: {e}")

    data["request_id"] = request_id
    try:
        response = AnalyzeResponse.model_validate(data)
    except Exception as e:
        raise SuggestionParseError(f"Response validation failed: {e}")

    return response


def _extract_json(text: str) -> str:
    """Extract JSON from LLM output, handling markdown wraps and noise."""
    text = text.strip()

    # Remove markdown code block
    m = re.search(r"```(?:json)?\s*\n?(.*?)\n?```", text, re.DOTALL)
    if m:
        return m.group(1)

    # Find first { and last }
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1:
        return text[start:end + 1]

    return text
