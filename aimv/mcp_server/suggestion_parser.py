# [AIMV] MCP Server — LLM response → structured Suggestion parser
import json
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
    - Prefix noise ("I'm sorry, here is...")
    - diff field containing triple backticks
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
    """Extract JSON from LLM output using brace-depth counting.

    Does NOT use regex for ```json extraction because diff fields
    may contain triple backticks (unified diff format), causing
    premature truncation. Brace matching is more reliable.
    """
    text = text.strip()

    start = text.find("{")
    if start == -1:
        return text

    depth = 0
    in_string = False
    escape = False
    for i in range(start, len(text)):
        c = text[i]
        if escape:
            escape = False
            continue
        if c == '\\' and in_string:
            escape = True
            continue
        if c == '"' and not escape:
            in_string = not in_string
            continue
        if in_string:
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]

    # Fallback: no matching } found
    return text[start:]
