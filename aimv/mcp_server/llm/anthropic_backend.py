# [AIMV] MCP Server — Anthropic backend implementation
from anthropic import Anthropic
from .base import AbstractLLMBackend
from ..models import AnalyzeRequest, AnalyzeResponse
from ..prompt_builder import build_system_prompt, build_user_prompt
from ..suggestion_parser import parse_structured_response, SuggestionParseError


class AnthropicBackend(AbstractLLMBackend):
    """Anthropic Messages API backend.

    Key differences from OpenAI:
    - system prompt is a top-level parameter (not in messages[])
    - no response_format field; JSON constraint is enforced via prompt
    - content is a list of ContentBlock objects
    """

    def __init__(self, config: dict):
        client_kwargs = {"api_key": config.get("api_key", "mock")}
        if config.get("base_url"):
            client_kwargs["base_url"] = config["base_url"]
        self.client = Anthropic(**client_kwargs)
        self.model = config.get("model", "claude-sonnet-4-6")
        self.max_tokens = config.get("max_tokens", 4096)

    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        system_prompt = build_system_prompt(request)
        user_prompt = build_user_prompt(request)

        # Inject JSON schema constraint into system prompt
        system_prompt += (
            "\n\nRespond ONLY with a single valid JSON object. "
            "Do NOT wrap it in markdown code blocks. "
            "Do NOT include any text before or after the JSON."
        )

        message = self.client.messages.create(
            model=self.model,
            max_tokens=self.max_tokens,
            system=system_prompt,
            messages=[
                {"role": "user", "content": user_prompt},
            ],
        )

        raw_json = message.content[0].text
        return parse_structured_response(raw_json, request.request_id)

    def health_check(self) -> bool:
        try:
            self.client.messages.create(
                model=self.model,
                max_tokens=1,
                messages=[{"role": "user", "content": "ping"}],
            )
            return True
        except Exception:
            return False
