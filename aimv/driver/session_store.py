# [BiSheng] AIMV Driver — Session persistence and crash recovery
import json
import os
from pathlib import Path
from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Optional, List
import time


class DriverStatus(Enum):
    IDLE = "idle"
    COMPILING = "compiling"
    ANALYZING_OPT_INFO = "analyzing_opt_info"
    QUERYING_MCP = "querying_mcp"
    PATCHING = "patching"
    VERIFYING = "verifying"
    MEASURING = "measuring"
    SUCCESS = "success"
    GAVE_UP = "gave_up"
    FAILED = "failed"
    ERROR = "error"


class TerminationReason(Enum):
    VECTORIZED = "vectorized"
    ROUND_LIMIT = "round_limit"
    NO_IMPROVEMENT = "no_improvement"
    NO_SUGGESTION = "no_suggestion"
    COMPILE_ERROR = "compile_error"
    TEST_FAILURE = "test_failure"
    INTERRUPTED = "interrupted"


@dataclass
class PatchRecord:
    source_file: str
    backup_path: str
    diff_text: str
    original_hash: str
    applied_at: float = field(default_factory=time.time)


@dataclass
class VectorizationStatus:
    function_name: str
    total_loops: int = 0
    vectorized_loops: int = 0
    missed_loops: int = 0
    missed_details: List[dict] = field(default_factory=list)


@dataclass
class RoundRecord:
    round_number: int
    status: DriverStatus = DriverStatus.IDLE
    diagnostics_json: Optional[dict] = None
    mcp_request: Optional[dict] = None
    mcp_response: Optional[dict] = None
    mcp_elapsed_ms: Optional[float] = None
    patch: Optional[PatchRecord] = None
    vectorization_status: Optional[VectorizationStatus] = None
    compile_success: Optional[bool] = None
    test_success: Optional[bool] = None
    baseline_perf_ms: Optional[float] = None
    after_perf_ms: Optional[float] = None
    perf_delta_pct: Optional[float] = None
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None


@dataclass
class SessionRecord:
    session_id: str = field(default_factory=lambda: f"aimv-{time.time_ns():x}")
    function_name: str = ""
    source_files: List[str] = field(default_factory=list)
    aimv_level: str = "moderate"
    max_rounds: int = 5
    target_loop_line: Optional[str] = None
    pristine_backup_dir: str = ""
    rounds: List[RoundRecord] = field(default_factory=list)
    current_round: int = 0
    termination_reason: Optional[TerminationReason] = None
    final_patch_path: Optional[str] = None
    total_elapsed_ms: Optional[float] = None
    overall_perf_improvement_pct: Optional[float] = None
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None
    cli_command: str = ""


class SessionStore:
    def __init__(self, output_dir: str):
        self.sessions_dir = Path(output_dir) / "sessions"
        self.sessions_dir.mkdir(parents=True, exist_ok=True)

    def save(self, session: SessionRecord):
        data = _serialize(session)
        path = self.sessions_dir / f"{session.session_id}.json"
        tmp_path = path.with_suffix(".tmp")
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False, default=str)
        os.replace(tmp_path, path)

    def load(self, session_id: str) -> Optional[SessionRecord]:
        path = self.sessions_dir / f"{session_id}.json"
        if not path.exists():
            return None
        with open(path, encoding="utf-8") as f:
            return _deserialize(json.load(f))

    def list_sessions(self) -> list[dict]:
        sessions = []
        for path in self.sessions_dir.glob("*.json"):
            with open(path) as f:
                data = json.load(f)
            sessions.append({
                "session_id": data.get("session_id", ""),
                "function_name": data.get("function_name", ""),
                "status": data.get("termination_reason", "in_progress"),
                "rounds": len(data.get("rounds", [])),
                "started_at": data.get("started_at"),
            })
        return sorted(sessions, key=lambda s: s.get("started_at", ""), reverse=True)


def _serialize(obj):
    if hasattr(obj, "__dataclass_fields__"):
        result = {}
        for name in obj.__dataclass_fields__:
            value = getattr(obj, name)
            result[name] = _serialize(value)
        return result
    elif isinstance(obj, list):
        return [_serialize(item) for item in obj]
    elif isinstance(obj, dict):
        return {k: _serialize(v) for k, v in obj.items()}
    elif isinstance(obj, Enum):
        return obj.value
    else:
        return obj


def _deserialize(data: dict) -> SessionRecord:
    session = SessionRecord()
    session.session_id = data.get("session_id", "")
    session.function_name = data.get("function_name", "")
    session.source_files = data.get("source_files", [])
    session.aimv_level = data.get("aimv_level", "moderate")
    session.max_rounds = data.get("max_rounds", 5)
    session.target_loop_line = data.get("target_loop_line")
    tr = data.get("termination_reason")
    if tr:
        session.termination_reason = TerminationReason(tr)
    return session
