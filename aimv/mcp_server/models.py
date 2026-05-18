# [AIMV] MCP Server — Pydantic request/response models
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


# --- Diagnostic sub-structures ---

class CostModelDetail(BaseModel):
    """VPlan cost model breakdown (MVP: no instruction_costs detail)"""
    scalar_cost: int = Field(..., ge=-1)
    vector_cost: int = Field(..., ge=-1)
    vf: int = Field(..., ge=0)
    interleave_count: int = Field(..., ge=0)


class DependencyInfo(BaseModel):
    """Memory dependency analysis result — dep_type uses LLVM Dependence::DepName[Dep.Type]"""
    dep_type: str = Field(
        ...,
        pattern=r"^(NoDep|Unknown|IndirectUnsafe|Forward|ForwardButPreventsForwarding|Backward|BackwardVectorizable|BackwardVectorizableButPreventsForwarding)$"
    )
    source_ptr: str
    sink_ptr: str
    alias_result: str


class MemoryInfo(BaseModel):
    """Memory/alignment info"""
    num_stores: int = Field(..., ge=-1, description="-1=unavailable (SLP/Unroll)")
    num_loads: int = Field(..., ge=-1, description="-1=unavailable (SLP/Unroll)")
    num_pred_stores: Optional[int] = Field(
        None, description="MVP: unavailable, LAI doesn't provide. Phase 2 from LoopVectorizationLegality"
    )
    max_alignment: int = Field(..., ge=-1, description="-1=unavailable (SLP/Unroll)")
    stride: str
    memory_check_count: Optional[int] = Field(
        None, description="None=unavailable(legality stage); >=0=specific value"
    )
    memory_check_cost: Optional[int] = Field(
        None, description="None=unavailable; >=0=specific; -1=Invalid InstructionCost"
    )


class LoopInfo(BaseModel):
    """Loop structure info"""
    num_blocks: int
    num_instructions: int
    trip_count: int = Field(
        ..., description="-1=SE unavailable; 0=empty loop; >0=specific value"
    )
    num_branches: int
    num_calls: int


class SingleDiagnostic(BaseModel):
    """Single opt-info diagnostic record"""
    pass_name: str
    remark_id: str
    remark_text: str
    severity: RemarkSeverity
    function_name: str
    loop_location: str
    source_context: str
    ir_snippet: str
    source_accuracy: Optional[str] = Field(
        None, description="None=precise; 'approximate'=line numbers may be off"
    )
    cost_model: Optional[CostModelDetail] = None
    dependencies: List[DependencyInfo] = []
    memory_info: Optional[MemoryInfo] = None
    loop_info: Optional[LoopInfo] = None


# --- Request/Response top-level models ---

class TargetInfo(BaseModel):
    """Target platform info"""
    triple: str
    cpu: str
    features: List[str] = []
    vector_width: int = Field(..., gt=0)


class FunctionInfo(BaseModel):
    """Analyzed function info"""
    name: str
    signature: str
    source_code: str
    source_file: str
    loop_line: int = Field(..., gt=0)


class HistoryRecord(BaseModel):
    """History round record — last 3 rounds sent to AI"""
    round: int
    diagnosis_summary: str
    suggestion_applied: str
    outcome: str


class AnalyzeRequest(BaseModel):
    """POST /api/v1/analyze-vectorization request body"""
    request_id: str
    target: TargetInfo
    function: FunctionInfo
    diagnostics: List[SingleDiagnostic] = Field(..., min_length=1)
    history: List[HistoryRecord] = []
    aimv_level: AimvLevel = AimvLevel.CONSERVATIVE

    @field_validator("diagnostics")
    @classmethod
    def check_diag_count(cls, v):
        if len(v) > 20:
            raise ValueError("too many diagnostics (max 20 per request)")
        return v


class Suggestion(BaseModel):
    """AI returned single modification suggestion"""
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
    """POST /api/v1/analyze-vectorization response body"""
    request_id: str
    suggestions: List[Suggestion] = []
    overall_analysis: str
    confidence: float = Field(..., ge=0.0, le=1.0)
    no_action_possible: bool = False


# --- Loop Transform models (T6.1) ---


class LoopNestInfo(BaseModel):
    """Nested loop structure info"""
    outer_loop_line: int = Field(..., gt=0)
    inner_loop_line: int = Field(..., gt=0)
    outer_trip_count: int = Field(-1, description="-1=unknown")
    inner_trip_count: int = Field(-1, description="-1=unknown")
    is_row_major: Optional[bool] = Field(None, description="None=undetermined")


class LoopTransformRequest(BaseModel):
    """POST /api/v1/analyze-loop-transform request body"""
    request_id: str
    target: TargetInfo
    function: FunctionInfo
    diagnostics: List[SingleDiagnostic] = Field(..., min_length=1)
    loop_nest: Optional[LoopNestInfo] = None
    aimv_level: AimvLevel = AimvLevel.CONSERVATIVE


class TransformSuggestion(BaseModel):
    """AI returned loop transform suggestion"""
    transform_type: str = Field(
        pattern=r"^(interchange|fission|fusion|unroll|unroll_and_jam|distribution)$"
    )
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


class LoopTransformResponse(BaseModel):
    """POST /api/v1/analyze-loop-transform response body"""
    request_id: str
    suggestions: List[TransformSuggestion] = []
    overall_analysis: str
    confidence: float = Field(..., ge=0.0, le=1.0)
    no_action_possible: bool = False
