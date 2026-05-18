# [AIMV] AIMV Driver — Session persistence and crash recovery
import json
import os
from pathlib import Path
from typing import Optional

from .models import (
    SessionRecord, RoundRecord, TerminationReason, IterationStatus,
    BuildResult, TestResult, VectorizationStatus, PatchRecord,
    PerFunctionResult,
)


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
                "function_name": _first_function_name(data),
                "status": data.get("termination_reason", "in_progress"),
                "rounds": sum(
                    len(fn.get("rounds", []))
                    for fn in data.get("functions", [])
                ),
                "started_at": data.get("started_at"),
            })
        return sorted(sessions, key=lambda s: s.get("started_at", ""), reverse=True)


def _first_function_name(data: dict) -> str:
    fns = data.get("functions", [])
    return fns[0].get("function_name", "") if fns else ""


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
    elif hasattr(obj, "value"):
        # Enum
        return obj.value
    else:
        return obj


def _deserialize(data: dict) -> SessionRecord:
    session = SessionRecord()
    session.session_id = data.get("session_id", "")
    session.source_file = data.get("source_file", "")
    session.aimv_level = data.get("aimv_level", "conservative")
    session.max_rounds = data.get("max_rounds", 5)
    session.pristine_backup_path = data.get("pristine_backup_path", "")
    session.cli_command = data.get("cli_command", "")
    session.started_at = data.get("started_at", 0)
    session.finished_at = data.get("finished_at")
    session.total_elapsed_ms = data.get("total_elapsed_ms")
    session.final_patch_path = data.get("final_patch_path")

    tr = data.get("termination_reason")
    if tr:
        try:
            session.termination_reason = TerminationReason(tr)
        except ValueError:
            pass

    # Deserialize functions list
    for fn_data in data.get("functions", []):
        pfr = PerFunctionResult(function_name=fn_data.get("function_name", ""))
        pfr.vectorized = fn_data.get("vectorized", False)
        pfr.rounds_used = fn_data.get("rounds_used", 0)
        pfr.cross_function_regression = fn_data.get("cross_function_regression", False)
        pfr.history = fn_data.get("history", [])

        fn_tr = fn_data.get("termination_reason")
        if fn_tr:
            try:
                pfr.termination_reason = TerminationReason(fn_tr)
            except ValueError:
                pass

        for r_data in fn_data.get("rounds", []):
            rr = _deserialize_round(r_data)
            pfr.rounds.append(rr)

        session.functions.append(pfr)

    return session


def _deserialize_round(data: dict) -> RoundRecord:
    rr = RoundRecord(round_number=data.get("round_number", 0))

    status_val = data.get("status")
    if status_val:
        try:
            rr.status = IterationStatus(status_val)
        except ValueError:
            pass

    rr.diagnostics_json = data.get("diagnostics_json")
    rr.mcp_request = data.get("mcp_request")
    rr.mcp_response = data.get("mcp_response")
    rr.mcp_elapsed_ms = data.get("mcp_elapsed_ms")
    rr.suggestion_description = data.get("suggestion_description")
    rr.applied_diff_summary = data.get("applied_diff_summary")
    rr.started_at = data.get("started_at", 0)
    rr.finished_at = data.get("finished_at")

    br_data = data.get("build_result")
    if br_data and isinstance(br_data, dict):
        rr.build_result = BuildResult(**br_data)

    vb_data = data.get("verify_build")
    if vb_data and isinstance(vb_data, dict):
        rr.verify_build = BuildResult(**vb_data)

    tr_data = data.get("test_result")
    if tr_data and isinstance(tr_data, dict):
        rr.test_result = TestResult(**tr_data)

    vs_data = data.get("vectorization_status")
    if vs_data and isinstance(vs_data, dict):
        rr.vectorization_status = VectorizationStatus(**vs_data)

    p_data = data.get("patch")
    if p_data and isinstance(p_data, dict):
        rr.patch = PatchRecord(**p_data)

    return rr
