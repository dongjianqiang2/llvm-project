# [AIMV] T3.8 — aimv_driver CLI + top-level orchestration tests
import json
import pytest
from pathlib import Path
from unittest import mock

from aimv.driver.aimv_driver import (
    main, main_from_json, main_independent,
    emit_summary, _format_termination,
    extract_function_source, extract_function_signature, extract_loop_line,
)
from aimv.driver.config import DriverConfig
from aimv.driver.models import (
    SessionRecord, PerFunctionResult, TerminationReason,
    IterationStatus, RoundRecord, BuildResult,
)


def _make_build_ok(aimv_json_path):
    """Helper: create a successful BuildResult and write empty diagnostics JSON."""
    from pathlib import Path
    import json
    Path(aimv_json_path).parent.mkdir(parents=True, exist_ok=True)
    with open(aimv_json_path, "w") as f:
        json.dump({"diagnostics": []}, f)
    return BuildResult(
        returncode=0, stdout="", stderr="",
        opt_record_path="", aimv_json_path=aimv_json_path,
        elapsed_ms=100,
    )


class TestExtractHelpers:
    def test_extract_function_source(self, tmp_path):
        f = tmp_path / "test.c"
        f.write_text("void foo(int *a) { *a = 1; }\nvoid bar(void) { return; }\n")
        result = extract_function_source(str(f), "foo")
        assert result is not None
        assert "foo" in result
        assert "*a = 1" in result

    def test_extract_function_source_not_found(self, tmp_path):
        f = tmp_path / "test.c"
        f.write_text("void bar(void) { return; }\n")
        result = extract_function_source(str(f), "nonexistent")
        assert result is None

    def test_extract_function_signature(self, tmp_path):
        f = tmp_path / "test.c"
        f.write_text("void foo(int *a) { *a = 1; }\n")
        result = extract_function_signature(str(f), "foo")
        assert "foo" in result
        assert "int *a" in result

    def test_extract_loop_line(self):
        diags = [{"function_name": "foo", "loop_location": "t.c:42:5"}]
        assert extract_loop_line(diags, "foo") == 42

    def test_extract_loop_line_no_match(self):
        diags = [{"function_name": "bar", "loop_location": "t.c:10:5"}]
        assert extract_loop_line(diags, "foo") == 1  # default


class TestCLI:
    def test_from_json_mode(self, tmp_path):
        """--from-json + --source → executes main_from_json."""
        aimv_json = tmp_path / "aimv.json"
        aimv_json.write_text(json.dumps({
            "diagnostics": [{
                "function_name": "foo",
                "severity": "missed",
                "remark_id": "test",
                "remark_text": "test",
                "loop_location": "t.c:1:1",
            }]
        }))
        source = tmp_path / "task.c"
        source.write_text("void foo(int *a) { for(int i=0;i<10;i++) a[i]=0; }\n")

        with mock.patch("aimv.driver.aimv_driver.main_from_json", return_value=0) as mock_main:
            result = main(["--from-json", str(aimv_json), "--source", str(source)])
            mock_main.assert_called_once_with(str(aimv_json), str(source))
            assert result == 0

    def test_independent_mode(self, tmp_path):
        """Passing source file → main_independent."""
        source = tmp_path / "task.c"
        source.write_text("void foo(int *a) { for(int i=0;i<10;i++) a[i]=0; }\n")

        with mock.patch("aimv.driver.aimv_driver.main_independent", return_value=0) as mock_main:
            result = main([str(source), "--function", "foo"])
            mock_main.assert_called_once()
            assert result == 0

    def test_exit_code_no_source(self):
        """Missing source file → exit 2."""
        result = main(["--from-json", "aimv.json"])
        assert result == 2

    def test_exit_code_no_source_arg(self):
        """No source_file argument → exit 2."""
        result = main([])
        assert result == 2

    def test_list_sessions(self, tmp_path):
        """--list-sessions → lists sessions and exits 0."""
        with mock.patch("aimv.driver.aimv_driver.SessionStore") as MockStore:
            mock_store = mock.MagicMock()
            mock_store.list_sessions.return_value = [
                {"session_id": "s1", "function_name": "foo", "status": "vectorized", "rounds": 2}
            ]
            MockStore.return_value = mock_store
            result = main(["--list-sessions", "--output-dir", str(tmp_path)])
            assert result == 0

    def test_verbose_flag(self, tmp_path):
        """--verbose → prints args."""
        source = tmp_path / "task.c"
        source.write_text("void foo() {}\n")
        with mock.patch("aimv.driver.aimv_driver.main_independent", return_value=0):
            result = main(["--verbose", str(source)])
            assert result == 0


class TestMainFromJson:
    def test_empty_diagnostics(self, tmp_path):
        """No missed diagnostics → exit 0, "nothing to do"."""
        aimv_json = tmp_path / "aimv.json"
        aimv_json.write_text(json.dumps({
            "diagnostics": [{
                "function_name": "foo",
                "severity": "passed",
                "remark_id": "ok",
                "remark_text": "ok",
            }]
        }))
        source = tmp_path / "task.c"
        source.write_text("void foo() {}\n")
        result = main_from_json(str(aimv_json), str(source))
        assert result == 0

    def test_source_file_not_found(self, tmp_path):
        """Nonexistent source → exit 2."""
        result = main(["--from-json", "/dev/null", "--source", "/nonexistent/file.c"])
        assert result == 2


class TestEmitSummary:
    def test_single_function_vectorized(self, capsys):
        """Single function vectorized → proper stderr output."""
        results = [PerFunctionResult(
            function_name="process_task",
            vectorized=True, rounds_used=2,
            termination_reason=TerminationReason.VECTORIZED,
        )]
        rr = RoundRecord(round_number=1)
        rr.applied_diff_summary = "added restrict"
        results[0].rounds.append(rr)

        session = SessionRecord(aimv_level="conservative")
        config = DriverConfig()
        store = mock.MagicMock()
        store.sessions_dir = Path("/tmp")

        emit_summary(results, "task.c", session, store, config)
        captured = capsys.readouterr()
        assert "[AIMV]" in captured.err
        assert "process_task" in captured.err
        assert "vectorized" in captured.err

    def test_multi_function_mixed(self, capsys):
        """Multi-function mixed results → summary with counts."""
        results = [
            PerFunctionResult(function_name="foo", vectorized=True, rounds_used=1,
                              termination_reason=TerminationReason.VECTORIZED),
            PerFunctionResult(function_name="bar", vectorized=False, rounds_used=3,
                              termination_reason=TerminationReason.ROUND_LIMIT),
        ]
        session = SessionRecord(aimv_level="conservative")
        config = DriverConfig()
        store = mock.MagicMock()
        store.sessions_dir = Path("/tmp")

        emit_summary(results, "task.c", session, store, config)
        captured = capsys.readouterr()
        assert "2 functions analyzed" in captured.err
        assert "1 optimized" in captured.err

    def test_format_termination(self):
        assert "exhausted" in _format_termination(TerminationReason.ROUND_LIMIT, 3)
        assert "no suggestions" in _format_termination(TerminationReason.NO_SUGGESTION, 3)
        assert "regression" in _format_termination(TerminationReason.NO_IMPROVEMENT, 2)
        assert "compile error" in _format_termination(TerminationReason.COMPILE_ERROR, 1)
        assert "test failure" in _format_termination(TerminationReason.TEST_FAILURE, 1)
        assert "interrupted" in _format_termination(TerminationReason.INTERRUPTED, 1)


class TestExitCodes:
    def test_exit_code_success(self, tmp_path):
        """All vectorized → exit 0."""
        source = tmp_path / "task.c"
        source.write_text("void foo() {}\n")
        output = tmp_path / "output"
        output.mkdir()

        from aimv.driver.models import BuildResult, VectorizationStatus

        with mock.patch("aimv.driver.aimv_driver.process_single_function") as mock_psf:
            mock_psf.return_value = PerFunctionResult(
                function_name="foo", vectorized=True,
                termination_reason=TerminationReason.VECTORIZED, rounds_used=1)

            with mock.patch("aimv.driver.aimv_driver._check_cross_function_regression",
                            return_value=False):
                with mock.patch("aimv.driver.aimv_driver.BuildOrchestrator") as MockBO:
                    mock_builder = mock.MagicMock()
                    # initial compile
                    aimv_json = str(output / "aimv-initial.json")
                    mock_builder.compile_with_aimv.return_value = _make_build_ok(aimv_json)
                    mock_builder.check_vectorization.return_value = VectorizationStatus(
                        "foo", 1, 1, 0, [], 1)
                    MockBO.return_value = mock_builder

                    with mock.patch("aimv.driver.aimv_driver.MCPClient"):
                        with mock.patch("aimv.driver.aimv_driver.SourceManager"):
                            with mock.patch("aimv.driver.aimv_driver.SessionStore"):
                                config = DriverConfig(output_dir=str(output))
                                result = main_independent(str(source), "foo", config)
                                assert result == 0

    def test_stderr_format(self, capsys, tmp_path):
        """Output contains [AIMV] prefix."""
        results = [PerFunctionResult(
            function_name="foo", vectorized=True, rounds_used=1,
            termination_reason=TerminationReason.VECTORIZED,
        )]
        session = SessionRecord(aimv_level="conservative")
        config = DriverConfig()
        store = mock.MagicMock()
        store.sessions_dir = Path("/tmp")

        emit_summary(results, "task.c", session, store, config)
        captured = capsys.readouterr()
        for line in captured.err.strip().split("\n"):
            if line.strip():
                assert line.strip().startswith("[AIMV]"), f"Line missing [AIMV] prefix: {line}"
