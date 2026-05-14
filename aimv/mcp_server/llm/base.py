# [BiSheng] MCP Server — Abstract LLM Backend interface
from abc import ABC, abstractmethod
from ..models import AnalyzeRequest, AnalyzeResponse


class AbstractLLMBackend(ABC):
    """LLM backend unified interface."""

    @abstractmethod
    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        """Send diagnostic request, return structured analysis result."""
        ...

    @abstractmethod
    def health_check(self) -> bool:
        """Check backend connectivity."""
        ...
