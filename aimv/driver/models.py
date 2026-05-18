# [AIMV] AIMV Driver — Internal data models
# Matches DRIVER_DESIGN §3 data model definitions
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, List, Dict
import uuid
import time


class IterationStatus(Enum):
    PENDING = "pending"
    COMPILING = "compiling"
    ANALYZING = "analyzing"
    QUERYING = "querying"
    PATCHING = "patching"
    VERIFYING = "verifying"
    SUCCESS = "success"
    FAILED = "failed"
    ROLLED_BACK = "rolled_back"


class TerminationReason(Enum):
    VECTORIZED = "vectorized"
    ROUND_LIMIT = "round_limit"
    NO_IMPROVEMENT = "no_improvement"
    NO_SUGGESTION = "no_suggestion"
    COMPILE_ERROR = "compile_error"
    TEST_FAILURE = "test_failure"
    LOCK_TIMEOUT = "lock_timeout"
    MCP_ERROR = "mcp_error"
    INTERRUPTED = "interrupted"


class NextAction(Enum):
    CONTINUE = "continue"
    RETRY_SAME = "retry_same"
    ESCALATE_LEVEL = "escalate_level"
    ROLLBACK = "rollback"
    STOP = "stop"


@dataclass
class BuildResult:
    returncode: int
    stdout: str
    stderr: str
    opt_record_path: str
    aimv_json_path: str
    elapsed_ms: float


@dataclass
class TestResult:
    returncode: int
    stdout: str
    stderr: str
    passed: int
    failed: int
    elapsed_ms: float


@dataclass
class VectorizationStatus:
    function_name: str
    total_loops: int
    vectorized_loops: int
    missed_loops: int
    missed_details: List[dict]
    passed_remark_count: int


@dataclass
class PatchRecord:
    source_file: str
    backup_path: str
    diff_text: str
    original_hash: str
    applied_at: float = field(default_factory=time.time)


@dataclass
class RoundRecord:
    round_number: int
    status: IterationStatus = IterationStatus.PENDING
    build_result: Optional[BuildResult] = None
    diagnostics_json: Optional[dict] = None
    mcp_request: Optional[dict] = None
    mcp_response: Optional[dict] = None
    mcp_elapsed_ms: Optional[float] = None
    patch: Optional[PatchRecord] = None
    verify_build: Optional[BuildResult] = None
    test_result: Optional[TestResult] = None
    vectorization_status: Optional[VectorizationStatus] = None
    suggestion_description: Optional[str] = None
    applied_diff_summary: Optional[str] = None
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None


@dataclass
class PerFunctionResult:
    function_name: str
    rounds: List[RoundRecord] = field(default_factory=list)
    termination_reason: Optional[TerminationReason] = None
    vectorized: bool = False
    rounds_used: int = 0
    history: List[Dict] = field(default_factory=list)
    cross_function_regression: bool = False


@dataclass
class SessionRecord:
    session_id: str = field(default_factory=lambda: f"aimv-{uuid.uuid4().hex[:12]}")
    source_file: str = ""
    aimv_level: str = "conservative"
    max_rounds: int = 5
    functions: List[PerFunctionResult] = field(default_factory=list)
    pristine_backup_path: str = ""
    termination_reason: Optional[TerminationReason] = None
    final_patch_path: Optional[str] = None
    total_elapsed_ms: Optional[float] = None
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None
    cli_command: str = ""
