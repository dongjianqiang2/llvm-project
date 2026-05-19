# [AIMV] AIMV Driver — Build orchestrator (compilation + test subprocess management)
import subprocess
import tempfile
import time
import json
import re
from pathlib import Path
from typing import Optional

from .models import BuildResult, TestResult, VectorizationStatus


class BuildOrchestrator:
    """Manage clang compilation and test subprocesses.

    Key constraint: uses -mllvm -aimv-enable (not -faimv) to prevent recursive fork.
    """

    def __init__(self, config):
        # Accept DriverConfig or dict
        if hasattr(config, 'cc'):
            self.cc = config.cc
            self.cflags = config.cflags if isinstance(config.cflags, list) else [config.cflags]
            self.timeout_seconds = 120
        else:
            self.cc = config.get("cc", "clang")
            cflags = config.get("cflags", ["-O2"])
            self.cflags = cflags if isinstance(cflags, list) else cflags.split()
            self.timeout_seconds = config.get("timeout_seconds", 120)
        self.target_triple = ""  # Set from aimv.json to preserve original target
        self.work_dir = Path(tempfile.mkdtemp(prefix="aimv-"))

    def compile_with_aimv(
        self,
        source_file: str,
        output_file: str,
        aimv_json_output: Optional[str] = None,
        target_triple: Optional[str] = None,
    ) -> BuildResult:
        """Compile source with AIMVFeedbackPass enabled.

        Flags (anti-fork design):
          -O2 -g --target=<triple> -mllvm -aimv-enable -mllvm -aimv-output=<json>

        If target_triple is given and --target is not already in cflags,
        injects --target=<triple> to preserve the original compilation target.

        Shadow file handling:
          .aimv-tmp extension not recognized by compiler, auto-inject -x c/c++.
        """
        aimv_path = aimv_json_output or str(self.work_dir / "aimv-diag.json")

        cmd = [self.cc]
        cmd.extend(self.cflags)
        # [AIMV] Preserve target triple from original compilation
        triple = target_triple or self.target_triple
        if triple and triple not in ("", "unknown"):
            has_target = any(a.startswith("--target=") for a in self.cflags)
            if not has_target:
                cmd.append(f"--target={triple}")
        cmd.extend([
            "-c", "-g",
            "-mllvm", "-aimv-enable",
            "-mllvm", f"-aimv-output={aimv_path}",
        ])
        # [AIMV] Shadow file .aimv-tmp extension not recognized by compiler.
        # Must use -x c (or -x c++) to specify language.
        known_c_exts = {".c"}
        known_cxx_exts = {".cpp", ".cc", ".cxx", ".C"}
        src_ext = Path(source_file).suffix
        if src_ext not in known_c_exts and src_ext not in known_cxx_exts:
            orig_ext = Path(source_file.replace(".aimv-tmp", "")).suffix
            lang = "c++" if orig_ext in known_cxx_exts else "c"
            cmd.extend(["-x", lang])
        cmd.extend([source_file, "-o", output_file])

        start = time.monotonic()
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True,
                timeout=self.timeout_seconds,
            )
            elapsed = (time.monotonic() - start) * 1000
        except subprocess.TimeoutExpired:
            elapsed = self.timeout_seconds * 1000
            return BuildResult(
                returncode=-1, stdout="", stderr="compilation timed out",
                opt_record_path="", aimv_json_path=aimv_path,
                elapsed_ms=elapsed,
            )

        return BuildResult(
            returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr,
            opt_record_path="", aimv_json_path=aimv_path,
            elapsed_ms=elapsed,
        )

    def run_tests(self, test_cmd: str) -> TestResult:
        """Run test suite. Empty test_cmd → skip (return pass)."""
        if not test_cmd:
            return TestResult(
                returncode=0, stdout="", stderr="",
                passed=0, failed=0, elapsed_ms=0,
            )

        start = time.monotonic()
        try:
            proc = subprocess.run(
                test_cmd, shell=True, capture_output=True, text=True,
                timeout=self.timeout_seconds * 2,
            )
            elapsed = (time.monotonic() - start) * 1000
        except subprocess.TimeoutExpired:
            return TestResult(
                returncode=-1, stdout="", stderr="test timed out",
                passed=0, failed=1, elapsed_ms=self.timeout_seconds * 2000,
            )

        passed, failed = _parse_test_output(proc.stdout, proc.stderr)
        return TestResult(
            returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr,
            passed=passed, failed=failed, elapsed_ms=elapsed,
        )

    def check_vectorization(self, aimv_json_path: str, function_name: str) -> VectorizationStatus:
        """Parse AIMV JSON, check target function vectorization status."""
        try:
            with open(aimv_json_path) as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            return VectorizationStatus(
                function_name=function_name,
                total_loops=0, vectorized_loops=0, missed_loops=0,
                missed_details=[], passed_remark_count=0,
            )

        total = 0
        missed = 0
        passed = 0
        details = []
        for diag in data.get("diagnostics", []):
            if diag.get("function_name") == function_name:
                total += 1
                if diag.get("severity") == "missed":
                    missed += 1
                    details.append({
                        "remark_id": diag.get("remark_id"),
                        "remark_text": diag.get("remark_text"),
                        "loop_location": diag.get("loop_location"),
                    })
                elif diag.get("severity") == "passed":
                    passed += 1

        return VectorizationStatus(
            function_name=function_name,
            total_loops=total,
            vectorized_loops=passed,
            missed_loops=missed,
            missed_details=details,
            passed_remark_count=passed,
        )


def _parse_test_output(stdout: str, stderr: str) -> tuple:
    """Parse test output for pass/fail counts."""
    combined = stdout + stderr

    # CTest
    m = re.search(r"(\d+)% tests passed.*?(\d+) tests? failed.*?out of (\d+)", combined)
    if m:
        total = int(m.group(3))
        failed = int(m.group(2))
        return (total - failed, failed)

    # GoogleTest
    passed = len(re.findall(r"\[\s*PASSED\s*\]", combined))
    failed = len(re.findall(r"\[\s*FAILED\s*\]", combined))
    if passed + failed > 0:
        return (passed, failed)

    # Fallback to returncode
    return (1, 1) if "error" in combined.lower() else (1, 0)
