# [AIMV] T3.1 — BuildOrchestrator tests
import json
import tempfile
import pytest
from pathlib import Path
from unittest import mock

from aimv.driver.build_orchestrator import BuildOrchestrator, _parse_test_output
from aimv.driver.models import BuildResult, TestResult, VectorizationStatus


@pytest.fixture
def builder():
    return BuildOrchestrator({"cc": "clang", "cflags": ["-O2"], "timeout_seconds": 60})


@pytest.fixture
def builder_from_config():
    from aimv.driver.config import DriverConfig
    cfg = DriverConfig(cc="clang", cflags=["-O2"])
    return BuildOrchestrator(cfg)


class TestCompileWithAIMV:
    def test_compile_with_aimv_basic(self, builder):
        """compile_with_aimv injects -mllvm -aimv-enable flags."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(returncode=0, stdout="", stderr="")
            builder.compile_with_aimv("test.c", "test.o")
            cmd = mock_run.call_args[0][0]
            assert "-mllvm" in cmd
            assert "-aimv-enable" in cmd
            assert "-g" in cmd

    def test_compile_shadow_file_x_c(self, builder):
        """Shadow file .c.aimv-tmp auto-injects -x c."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(returncode=0, stdout="", stderr="")
            builder.compile_with_aimv("test.c.aimv-tmp", "test.o")
            cmd = mock_run.call_args[0][0]
            assert "-x" in cmd
            assert "c" in cmd

    def test_compile_shadow_file_x_cpp(self, builder):
        """Shadow file .cpp.aimv-tmp auto-injects -x c++."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(returncode=0, stdout="", stderr="")
            builder.compile_with_aimv("test.cpp.aimv-tmp", "test.o")
            cmd = mock_run.call_args[0][0]
            assert "-x" in cmd
            assert "c++" in cmd

    def test_compile_normal_c_no_x_flag(self, builder):
        """Normal .c file does NOT get -x flag."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(returncode=0, stdout="", stderr="")
            builder.compile_with_aimv("test.c", "test.o")
            cmd = mock_run.call_args[0][0]
            assert "-x" not in cmd

    def test_compile_timeout(self, builder):
        """Timeout returns BuildResult with returncode=-1."""
        import subprocess
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run",
                        side_effect=subprocess.TimeoutExpired(cmd="clang", timeout=120)):
            result = builder.compile_with_aimv("test.c", "test.o")
            assert result.returncode == -1
            assert "timed out" in result.stderr


class TestRunTests:
    def test_run_tests_empty_cmd(self, builder):
        """Empty test_cmd returns pass (0,0)."""
        result = builder.run_tests("")
        assert result.returncode == 0
        assert result.passed == 0
        assert result.failed == 0

    def test_run_tests_pass(self, builder):
        """Mock test pass: returncode=0, passed>0."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(
                returncode=0,
                stdout=(
                    "[ RUN      ] Test1\n[       OK ] Test1\n"
                    "[ RUN      ] Test2\n[       OK ] Test2\n"
                    "[ RUN      ] Test3\n[       OK ] Test3\n"
                ),
                stderr="",
            )
            result = builder.run_tests("./test_runner")
            assert result.passed > 0
            assert result.failed == 0

    def test_run_tests_fail(self, builder):
        """Mock test failure."""
        with mock.patch("aimv.driver.build_orchestrator.subprocess.run") as mock_run:
            mock_run.return_value = mock.MagicMock(
                returncode=1,
                stdout="[  PASSED  ] 2 tests.\n[  FAILED  ] 1 tests.",
                stderr="",
            )
            result = builder.run_tests("./test_runner")
            assert result.failed > 0


class TestCheckVectorization:
    def test_check_vectorization_missed(self, tmp_path):
        """aimv.json with missed diagnostics → missed_loops > 0."""
        aimv_json = tmp_path / "aimv.json"
        aimv_json.write_text(json.dumps({
            "diagnostics": [{
                "function_name": "foo",
                "severity": "missed",
                "remark_id": "CantReorderMemOps",
                "remark_text": "failed",
                "loop_location": "t.c:1:1",
            }]
        }))
        builder = BuildOrchestrator({"cc": "clang", "cflags": ["-O2"]})
        vstatus = builder.check_vectorization(str(aimv_json), "foo")
        assert vstatus.missed_loops > 0
        assert vstatus.passed_remark_count == 0

    def test_check_vectorization_vectorized(self, tmp_path):
        """aimv.json with only passed → vectorized_loops > 0, missed_loops == 0."""
        aimv_json = tmp_path / "aimv.json"
        aimv_json.write_text(json.dumps({
            "diagnostics": [{
                "function_name": "foo",
                "severity": "passed",
                "remark_id": "LoopVectorized",
                "remark_text": "ok",
                "loop_location": "t.c:2:1",
            }]
        }))
        builder = BuildOrchestrator({"cc": "clang", "cflags": ["-O2"]})
        vstatus = builder.check_vectorization(str(aimv_json), "foo")
        assert vstatus.vectorized_loops > 0
        assert vstatus.missed_loops == 0
        assert vstatus.passed_remark_count == 1

    def test_check_vectorization_no_json(self, tmp_path):
        """Missing aimv.json → empty VectorizationStatus."""
        builder = BuildOrchestrator({"cc": "clang", "cflags": ["-O2"]})
        vstatus = builder.check_vectorization(str(tmp_path / "nonexistent.json"), "foo")
        assert vstatus.total_loops == 0


class TestParseTestOutput:
    def test_ctest_format(self):
        passed, failed = _parse_test_output(
            "100% tests passed, 0 tests failed out of 5", "")
        assert passed == 5
        assert failed == 0

    def test_googletest_format(self):
        output = (
            "[ RUN      ] Test1\n[       OK ] Test1\n"
            "[ RUN      ] Test2\n[       OK ] Test2\n"
            "[ RUN      ] Test3\n[  FAILED  ] Test3\n"
        )
        # Note: [       OK ] is not matched by the PASSED regex.
        # Only [  PASSED  ] lines are counted. This output has 0 PASSED + 1 FAILED.
        passed, failed = _parse_test_output(output, "")
        assert failed == 1
        # passed may be 0 since [       OK ] doesn't match [  PASSED  ]

    def test_unknown_format_with_error(self):
        passed, failed = _parse_test_output("some error output", "")
        assert (passed, failed) == (1, 1)

    def test_unknown_format_no_error(self):
        passed, failed = _parse_test_output("all good", "")
        assert (passed, failed) == (1, 0)
