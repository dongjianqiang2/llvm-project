# [AIMV] Tests for aimv/driver/models.py (T0.3)
import json
import time

import pytest
from aimv.driver.models import (
    IterationStatus, TerminationReason, NextAction,
    BuildResult, TestResult, VectorizationStatus,
    PatchRecord, RoundRecord, PerFunctionResult, SessionRecord,
)


class TestIterationStatus:
    """Verify status enum values match DRIVER_DESIGN §3.1."""

    def test_all_statuses_exist(self):
        expected = {"pending", "compiling", "analyzing", "querying",
                    "patching", "verifying", "success", "failed", "rolled_back"}
        actual = {s.value for s in IterationStatus}
        assert actual == expected


class TestTerminationReason:
    """Verify termination reasons match DRIVER_DESIGN §3.1."""

    def test_all_reasons_exist(self):
        expected = {"vectorized", "round_limit", "no_improvement",
                    "no_suggestion", "compile_error", "test_failure",
                    "lock_timeout", "mcp_error", "interrupted"}
        actual = {r.value for r in TerminationReason}
        assert actual == expected


class TestRoundRecordSerialization:
    """RoundRecord → JSON → back should be consistent."""

    def test_round_record_serialize(self):
        rr = RoundRecord(
            round_number=1,
            status=IterationStatus.QUERYING,
            build_result=BuildResult(
                returncode=0, stdout="", stderr="",
                opt_record_path="", aimv_json_path="/tmp/aimv.json",
                elapsed_ms=100.0,
            ),
        )
        # Manual serialization (SessionStore._serialize style)
        data = {}
        for fname in rr.__dataclass_fields__:
            val = getattr(rr, fname)
            if hasattr(val, "value"):
                data[fname] = val.value
            elif hasattr(val, "__dataclass_fields__"):
                data[fname] = {f: getattr(val, f) for f in val.__dataclass_fields__}
            else:
                data[fname] = val
        json_str = json.dumps(data, default=str)
        parsed = json.loads(json_str)
        assert parsed["round_number"] == 1
        assert parsed["status"] == "querying"
        assert parsed["build_result"]["returncode"] == 0


class TestSessionRecord:
    """Session record with multiple functions."""

    def test_session_id_format(self):
        sr = SessionRecord()
        assert sr.session_id.startswith("aimv-")
        # aimv- followed by 12 hex chars
        hex_part = sr.session_id[5:]
        assert len(hex_part) == 12
        int(hex_part, 16)  # should not raise

    def test_session_multi_function(self):
        sr = SessionRecord(
            source_file="test.c",
            aimv_level="conservative",
            max_rounds=5,
        )
        sr.functions.append(PerFunctionResult(
            function_name="foo",
            termination_reason=TerminationReason.VECTORIZED,
            vectorized=True,
            rounds_used=2,
        ))
        sr.functions.append(PerFunctionResult(
            function_name="bar",
            termination_reason=TerminationReason.ROUND_LIMIT,
            vectorized=False,
            rounds_used=5,
        ))
        assert len(sr.functions) == 2
        assert sr.functions[0].vectorized is True
        assert sr.functions[1].termination_reason == TerminationReason.ROUND_LIMIT


class TestPatchRecord:
    def test_patch_record_fields(self):
        pr = PatchRecord(
            source_file="test.c",
            backup_path="/tmp/test.r0.bak",
            diff_text="--- a/test.c\n+++ b/test.c\n",
            original_hash="abc123",
        )
        assert pr.source_file == "test.c"
        assert pr.original_hash == "abc123"
        assert pr.applied_at > 0
