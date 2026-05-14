"""T5.1-T5.2, T6.6 — Prompt template tests."""
import sys
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

TEMPLATE_DIR = Path(__file__).resolve().parent.parent.parent / "aimv" / "mcp_server" / "templates"


class TestPromptTemplates:
    def test_cost_reject_template_exists(self):
        path = TEMPLATE_DIR / "cost_reject_prompt.txt"
        assert path.exists(), f"Template not found: {path}"

    def test_cost_reject_template_has_content(self):
        content = (TEMPLATE_DIR / "cost_reject_prompt.txt").read_text()
        assert "pragma" in content.lower()
        assert "cost" in content.lower()

    def test_align_template_exists(self):
        path = TEMPLATE_DIR / "align_prompt.txt"
        assert path.exists(), f"Template not found: {path}"

    def test_align_template_has_content(self):
        content = (TEMPLATE_DIR / "align_prompt.txt").read_text()
        assert "align" in content.lower()
        assert "stride" in content.lower()

    def test_loop_transform_template_exists(self):
        path = TEMPLATE_DIR / "loop_transform_prompt.txt"
        assert path.exists(), f"Template not found: {path}"

    def test_loop_transform_template_has_content(self):
        content = (TEMPLATE_DIR / "loop_transform_prompt.txt").read_text()
        assert "fission" in content.lower() or "distribution" in content.lower()
        assert "interchange" in content.lower()

    def test_all_templates_non_empty(self):
        for p in TEMPLATE_DIR.glob("*.txt"):
            content = p.read_text()
            assert len(content) > 50, f"Template {p.name} is too short: {len(content)} chars"


class TestPromptBuilderWithTemplates:
    """Verify prompt_builder works with real template content."""
    def test_build_system_prompt_includes_template_rules(self):
        from aimv.mcp_server.models import (
            AnalyzeRequest, TargetInfo, FunctionInfo, SingleDiagnostic,
            RemarkSeverity, AimvLevel,
        )
        from aimv.mcp_server.prompt_builder import build_system_prompt

        req = AnalyzeRequest(
            request_id="r1",
            target=TargetInfo(triple="arm", cpu="cortex-a9", features=["neon"], vector_width=128),
            function=FunctionInfo(name="f", signature="void f()", source_code="void f(){}",
                                   source_file="f.c", loop_line=1),
            diagnostics=[SingleDiagnostic(
                pass_name="LoopVectorize", remark_id="CantReorderMemOps",
                remark_text="failed", severity=RemarkSeverity.MISSED,
                function_name="f", loop_location="f.c:1:1",
                source_context="", ir_snippet="",
            )],
            aimv_level=AimvLevel.MODERATE,
        )
        prompt = build_system_prompt(req)
        assert "cortex-a9" in prompt
        assert "DO NOT change program semantics" in prompt
