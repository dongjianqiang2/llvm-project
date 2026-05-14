# [AIMV] MCP Server — OpenAI backend implementation
from openai import OpenAI
from .base import AbstractLLMBackend
from ..models import AnalyzeRequest, AnalyzeResponse
from ..prompt_builder import build_system_prompt, build_user_prompt
from ..suggestion_parser import parse_structured_response


class OpenAIBackend(AbstractLLMBackend):
    def __init__(self, config: dict):
        self.client = OpenAI(api_key=config.get("api_key", "mock"))
        self.model = config.get("model", "gpt-4o")
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
