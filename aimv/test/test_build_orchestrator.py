"""T3.1-T3.3 — BuildOrchestrator tests."""
import sys
import json
import tempfile
import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from aimv.driver.build_orchestrator import (
    BuildOrchestrator, BuildResult, TestResult, VectorizationStatus
)


@pytest.fixture
def builder():
    return BuildOrchestrator({"cc": "clang", "cflags": "-O2", "timeout_seconds": 60})


class TestVectorizationStatusFromJSON:
    def test_passed_and_missed(self, builder):
        diags = [
            {"function_name": "foo", "severity": "missed",
             "remark_text": "failed", "loop_location": "t.c:1:1"},
            {"function_name": "foo", "severity": "passed",
             "remark_text": "ok", "loop_location": "t.c:2:1"},
        ]
        vstatus = builder._parse_diagnostics(diags, "foo")
        assert vstatus.total_loops == 2
        assert vstatus.vectorized_loops == 1
        assert vstatus.missed_loops == 1

    def test_empty_diagnostics(self, builder):
        vstatus = builder._parse_diagnostics([], "foo")
        assert vstatus.total_loops == 0

    def test_no_matching_function(self, builder):
        diags = [{"function_name": "bar", "severity": "missed"}]
        vstatus = builder._parse_diagnostics(diags, "foo")
        assert vstatus.total_loops == 0

    def test_all_passed(self, builder):
        diags = [
            {"function_name": "foo", "severity": "passed"},
            {"function_name": "foo", "severity": "passed"},
        ]
        vstatus = builder._parse_diagnostics(diags, "foo")
        assert vstatus.missed_loops == 0
        assert vstatus.vectorized_loops == 2


class TestParseTestOutput:
    def test_ctest_format(self):
        from aimv.driver.build_orchestrator import _parse_test_output
        passed, failed = _parse_test_output(
            "100% tests passed, 0 tests failed out of 5", "")
        assert passed == 5
        assert failed == 0

    def test_googletest_format(self):
        from aimv.driver.build_orchestrator import _parse_test_output
        output = (
            "[ RUN      ] Test1\n[       OK ] Test1\n"
            "[ RUN      ] Test2\n[       OK ] Test2\n"
            "[ RUN      ] Test3\n[       OK ] Test3\n"
            "[ RUN      ] Test4\n[  FAILED  ] Test4\n"
        )
        passed, failed = _parse_test_output(output, "")
        assert passed == 3
        assert failed == 1

    def test_unknown_format(self):
        from aimv.driver.build_orchestrator import _parse_test_output
        passed, failed = _parse_test_output("some random output", "")
        assert (passed, failed) == (1, 1)


class TestCheckTargetLoopPassed:
    def test_target_passed(self):
        from aimv.driver.aimv_driver import _check_target_loop_passed
        vstatus = VectorizationStatus(
            function_name="foo", total_loops=2, vectorized_loops=1,
            missed_loops=1,
            missed_details=[{"loop_location": "t.c:5:1"}])
        # target_loop is "t.c:1:1" which is NOT in missed_details → passed
        assert _check_target_loop_passed(vstatus, "t.c:1:1") is True

    def test_target_still_missed(self):
        from aimv.driver.aimv_driver import _check_target_loop_passed
        vstatus = VectorizationStatus(
            function_name="foo", total_loops=2, vectorized_loops=0,
            missed_loops=2,
            missed_details=[{"loop_location": "t.c:5:1"}])
        assert _check_target_loop_passed(vstatus, "t.c:5:1") is False

    def test_none_target(self):
        from aimv.driver.aimv_driver import _check_target_loop_passed
        vstatus = VectorizationStatus(
            function_name="foo", total_loops=1, vectorized_loops=1,
            missed_loops=0)
        assert _check_target_loop_passed(vstatus, None) is False
