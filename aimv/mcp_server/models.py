# [BiSheng] MCP Server — Pydantic request/response models
# Matches aimv_design_doc/MCP_DESIGN.md §3 (authoritative API contract)

from pydantic import BaseModel, Field, field_validator
from typing import Optional, List
from enum import Enum


class RemarkSeverity(str, Enum):
    PASSED = "passed"
    MISSED = "missed"
    ANALYSIS = "analysis"


class AimvLevel(str, Enum):
    CONSERVATIVE = "conservative"
    MODERATE = "moderate"
    AGGRESSIVE = "aggressive"


class CostModelDetail(BaseModel):
    scalar_cost: int = Field(..., ge=-1)
    vector_cost: int = Field(..., ge=-1)
    vf: int = Field(..., ge=0)
    interleave_count: int = Field(..., ge=0)


class DependencyInfo(BaseModel):
    """dep_type directly uses LLVM Dependence::DepName[Dep.Type]"""
    dep_type: str = Field(
        ...,
        pattern=r"^(NoDep|Unknown|IndirectUnsafe|Forward|ForwardButPreventsForwarding|Backward|BackwardVectorizable|BackwardVectorizableButPreventsForwarding)$"
    )
    source_ptr: str
    sink_ptr: str
    alias_result: str


class MemoryInfo(BaseModel):
    num_stores: int = Field(..., ge=0)
    num_loads: int = Field(..., ge=0)
    num_pred_stores: int = Field(0, ge=0)
    max_alignment: int = Field(..., ge=0)       # bytes
    stride: str
    memory_check_count: int = Field(..., ge=0)
    memory_check_cost: int = Field(..., ge=-1)


class LoopInfo(BaseModel):
    num_blocks: int
    num_instructions: int
    trip_count: int
    num_branches: int
    num_calls: int


class SingleDiagnostic(BaseModel):
    pass_name: str
    remark_id: str
    remark_text: str
    severity: RemarkSeverity
    function_name: str
    loop_location: str
    source_context: str
    ir_snippet: str
    cost_model: Optional[CostModelDetail] = None
    dependencies: List[DependencyInfo] = []
    memory_info: Optional[MemoryInfo] = None
    loop_info: Optional[LoopInfo] = None


class TargetInfo(BaseModel):
    triple: str
    cpu: str
    features: List[str] = []
    vector_width: int = Field(..., gt=0)


class FunctionInfo(BaseModel):
    name: str
    signature: str
    source_code: str
    source_file: str
    loop_line: int = Field(..., gt=0)


class AnalyzeRequest(BaseModel):
    request_id: str
    target: TargetInfo
    function: FunctionInfo
    diagnostics: List[SingleDiagnostic] = Field(..., min_length=1)
    history: List[dict] = []
    aimv_level: AimvLevel = AimvLevel.MODERATE

    @field_validator("diagnostics")
    @classmethod
    def check_diag_count(cls, v):
        if len(v) > 20:
            raise ValueError("too many diagnostics (max 20 per request)")
        return v


class Suggestion(BaseModel):
    description: str
    reasoning: str
    source_file: str
    line_start: int
    line_end: int
    original: str
    modified: str
    diff: str
    estimated_impact: str = Field(pattern=r"^(high|medium|low)$")
    safety_concern: Optional[str] = None


class AnalyzeResponse(BaseModel):
    request_id: str
    suggestions: List[Suggestion] = []
    overall_analysis: str
    confidence: float = Field(..., ge=0.0, le=1.0)
    no_action_possible: bool = False
