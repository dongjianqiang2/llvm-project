# [AIMV] MCP Server — DeepSeek backend (OpenAI-compatible API)
from openai import OpenAI
from .base import AbstractLLMBackend
from ..models import AnalyzeRequest, AnalyzeResponse
from ..prompt_builder import build_system_prompt, build_user_prompt
from ..suggestion_parser import parse_structured_response


class DeepSeekBackend(AbstractLLMBackend):
    """DeepSeek backend using OpenAI-compatible SDK.

    Note: does NOT call super().__init__() to avoid creating
    a default OpenAI client. Directly sets DeepSeek-specific client.
    """

    def __init__(self, config: dict):
        self.client = OpenAI(
            api_key=config.get("api_key", "mock"),
            base_url=config.get("base_url", "https://api.deepseek.com/v1"),
        )
        self.model = config.get("model", "deepseek-v4-pro")
        self.temperature = config.get("temperature", 0.1)
        self.max_tokens = config.get("max_tokens", 4096)

    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        system_prompt = build_system_prompt(request)
        user_prompt = build_user_prompt(request)

        response = self.client.chat.completions.create(
            model=self.model,
            temperature=self.temperature,
            max_tokens=self.max_tokens,
            response_format={"type": "json_object"},
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
        )

        raw_json = response.choices[0].message.content
        return parse_structured_response(raw_json, request.request_id)

    def health_check(self) -> bool:
        try:
            self.client.models.list(limit=0)
            return True
        except Exception:
            return False
