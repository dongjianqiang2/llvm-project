# [BiSheng] AIMV Driver — Build orchestrator (compilation + test subprocess management)
import subprocess
import tempfile
import time
import json
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional, List


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
    missed_details: List[dict] = field(default_factory=list)


class BuildOrchestrator:
    def __init__(self, config: dict):
        self.cc = config.get("cc", "clang")
        self.cflags = config.get("cflags", "").split()
        self.timeout_seconds = config.get("timeout_seconds", 120)
        self.work_dir = Path(config.get("work_dir", tempfile.mkdtemp(prefix="aimv-")))

    def compile_with_aimv(
        self, source_file: str, output_file: str,
        target_function: Optional[str] = None,
        aimv_json_output: Optional[str] = None,
    ) -> BuildResult:
        opt_record_path = str(self.work_dir / "opt-records.yaml")
        aimv_path = aimv_json_output or str(self.work_dir / "aimv-diag.json")

        cmd = [self.cc] + self.cflags
        cmd.extend([
            "-g",
            f"-fsave-optimization-record={opt_record_path}",
            "-Rpass-missed=loop-vectorize",
            f"-aimv-output={aimv_path}",
        ])
        if target_function:
            cmd.append(f"-aimv-target-function={target_function}")
        cmd.extend([source_file, "-o", output_file])

        start = time.monotonic()
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=self.timeout_seconds)
        elapsed = (time.monotonic() - start) * 1000

        return BuildResult(
            returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr,
            opt_record_path=opt_record_path, aimv_json_path=aimv_path,
            elapsed_ms=elapsed,
        )

    def check_vectorization_from_json(
        self, aimv_json_path: str, function_name: str,
    ) -> VectorizationStatus:
        with open(aimv_json_path) as f:
            data = json.load(f)
        return self._parse_diagnostics(data.get("diagnostics", []), function_name)

    def check_vectorization_from_yaml(
        self, opt_record_path: str, function_name: str,
    ) -> VectorizationStatus:
        import yaml
        with open(opt_record_path) as f:
            records = yaml.safe_load(f) or []
        diagnostics = []
        for r in records:
            if (r.get("Function") == function_name and
                "loop-vectorize" in str(r.get("Pass", ""))):
                diagnostics.append({
                    "function_name": function_name,
                    "remark_text": r.get("Name", ""),
                    "loop_location": r.get("DebugLoc", ""),
                    "severity": r.get("type", "unknown"),
                })
        return self._parse_diagnostics(diagnostics, function_name)

    def _parse_diagnostics(
        self, diagnostics: list, function_name: str,
    ) -> VectorizationStatus:
        total = 0
        missed = 0
        passed = 0
        details = []
        for d in diagnostics:
            if d.get("function_name") == function_name:
                total += 1
                sev = d.get("severity", "")
                if sev == "missed":
                    missed += 1
                    details.append({
                        "remark_text": d.get("remark_text", ""),
                        "loop_location": d.get("loop_location", ""),
                    })
                elif sev == "passed":
                    passed += 1

        if total == 0:
            return VectorizationStatus(
                function_name=function_name,
                total_loops=0, vectorized_loops=0, missed_loops=0,
                missed_details=[{"remark_text": "No diagnostics found"}],
            )

        return VectorizationStatus(
            function_name=function_name,
            total_loops=total, vectorized_loops=passed,
            missed_loops=missed, missed_details=details,
        )

    def run_tests(self, test_cmd: str) -> TestResult:
        start = time.monotonic()
        proc = subprocess.run(
            test_cmd, shell=True, capture_output=True, text=True,
            timeout=self.timeout_seconds * 2,
        )
        elapsed = (time.monotonic() - start) * 1000
        passed, failed = _parse_test_output(proc.stdout, proc.stderr)
        return TestResult(
            returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr,
            passed=passed, failed=failed, elapsed_ms=elapsed,
        )


def _parse_test_output(stdout: str, stderr: str) -> tuple[int, int]:
    import re
    combined = stdout + stderr
    m = re.search(r"(\d+)% tests passed.*?(\d+) tests? failed.*?out of (\d+)", combined)
    if m:
        total, failed = int(m.group(3)), int(m.group(2))
        return (total - failed, failed)
    # GoogleTest: "[       OK ] TestName" or "[  PASSED  ] TestName"
    ok_passed = len(re.findall(r"\[\s*(?:OK|PASSED)\s*\]", combined))
    failed = len(re.findall(r"\[\s*FAILED\s*\]", combined))
    if ok_passed + failed > 0:
        return (ok_passed, failed)
    return (1, 1)
