# AIMV — aimv-driver 详细设计方案

**版本**: 1.0
**日期**: 2026-04-29
**关联文档**: SPEC.md, PLAN.md, LLVM_DESIGN.md, MCP_DESIGN.md

---

## 0. 设计目标

`aimv-driver` 是 AIMV 闭环的编排中枢。它不实现任何编译优化逻辑，只负责将诊断信息（来自 LLVM Pass 或 YAML opt-records）、MCP Server（AI 分析）、源码 patch + 重编译验证串联为可控的迭代循环。

**双模式运行**（对应 SPEC §3 的系统形态）:

| 模式 | 诊断来源 | LLVM 改动 | 诊断丰富度 | 启动方式 |
|------|---------|----------|-----------|---------|
| **Pass 模式** | AIMVFeedbackPass JSON | 需要 | 完整（代价模型 + 依赖分析） | 默认，自动检测 `-aimv-output` |
| **YAML 模式** | `-fsave-optimization-record` YAML | 零侵入 | 基础（remark 文本 + 源码位置） | `--mode=yaml` 或当 aimv.json 不存在时自动回退 |

核心职责：
1. **编译编排** — 管理 clang 子进程，注入 AIMV flags，解析 opt-records
2. **MCP 通信** — 发送诊断 JSON，接收结构化建议，处理超时/重试
3. **源码管理** — 应用 unified diff，维护回滚点，保证原子性
4. **验证编排** — 运行测试套件，检查向量化 remark，可选性能测量
5. **迭代决策** — 根据终止条件决定继续/换策略/回滚/放弃
6. **持久化** — 完整 Session JSON，支持崩溃恢复后继续

---

## 1. 模块架构

```
aimv/driver/
├── aimv_driver.py              # CLI 入口 + 顶层编排循环
├── build_orchestrator.py       # 子进程管理（clang、test）
├── opt_info_parser.py          # opt-record YAML/JSON 解析
├── mcp_client.py               # MCP REST 客户端（含重试）
├── source_manager.py           # 源码 patch + 回滚 + 工作目录管理
├── iteration_engine.py         # 迭代策略决策引擎
├── session_store.py            # Session 持久化与恢复
├── perf_measurer.py            # 性能测量（可选）
├── models.py                   # 内部数据模型（与 MCP 模型不同）
└── config.py                   # 配置加载与校验
```

### 1.1 模块交互

```
                    ┌──────────────────────┐
                    │    aimv_driver.py     │
                    │   (CLI + 主循环)       │
                    └──────┬───────────────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
┌─────────────────┐ ┌─────────────┐ ┌──────────────────┐
│build_orchestrator│ │mcp_client   │ │source_manager    │
│· compile()      │ │· analyze()  │ │· apply_patch()   │
│· test()         │ │· health()   │ │· rollback()      │
│· get_remarks()  │ │             │ │· get_backups()   │
└────────┬────────┘ └──────┬──────┘ └────────┬─────────┘
         │                 │                  │
         ▼                 ▼                  ▼
┌─────────────────┐ ┌─────────────┐ ┌──────────────────┐
│opt_info_parser  │ │config       │ │session_store     │
│· parse_yaml()   │ │· load()     │ │· save()          │
│· extract_loops()│ │· validate() │ │· resume()        │
└─────────────────┘ └─────────────┘ └──────────────────┘
                           │
          ┌────────────────┤
          ▼                ▼
┌─────────────────┐ ┌──────────────────┐
│iteration_engine │ │perf_measurer     │
│· decide_next()  │ │· measure()       │
│· should_stop()  │ │· compare()       │
└─────────────────┘ └──────────────────┘
```

---

## 2. 内部数据模型

```python
# [AIMV] driver/models.py

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, List, Dict
import uuid
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
    ERROR = "error"


class TerminationReason(Enum):
    VECTORIZED = "vectorized"              # remark 变为 passed
    ROUND_LIMIT = "round_limit"            # 达到 --max-rounds
    NO_IMPROVEMENT = "no_improvement"      # 性能退化或不变
    NO_SUGGESTION = "no_suggestion"        # MCP 返回 no_action_possible
    COMPILE_ERROR = "compile_error"        # patch 导致编译失败
    TEST_FAILURE = "test_failure"          # patch 导致测试失败
    INTERRUPTED = "interrupted"            # 用户中断 (Ctrl+C)


@dataclass
class BuildResult:
    """单次编译结果"""
    returncode: int
    stdout: str
    stderr: str
    opt_record_path: str                   # YAML/JSON opt-record 文件路径
    aimv_json_path: str                    # AIMVFeedback Pass 输出的 JSON
    elapsed_ms: float


@dataclass
class TestResult:
    """测试套件执行结果"""
    returncode: int
    stdout: str
    stderr: str
    passed: int
    failed: int
    elapsed_ms: float


@dataclass
class VectorizationStatus:
    """单次编译后目标函数的向量化状态"""
    function_name: str
    total_loops: int
    vectorized_loops: int                  # 成功向量化的循环数
    missed_loops: int                      # 仍然失败的循环数
    missed_details: List[dict]             # 失败循环的简要信息


@dataclass
class PatchRecord:
    """一次源码修改的完整记录"""
    source_file: str
    backup_path: str                       # 回滚用的备份文件
    diff_text: str
    original_hash: str                     # sha256 of original file
    applied_at: float = field(default_factory=time.time)


@dataclass
class RoundRecord:
    """单轮迭代的完整记录"""
    round_number: int
    status: DriverStatus = DriverStatus.IDLE

    # 编译阶段
    build_result: Optional[BuildResult] = None
    diagnostics_json: Optional[dict] = None

    # AI 阶段
    mcp_request: Optional[dict] = None
    mcp_response: Optional[dict] = None
    mcp_elapsed_ms: Optional[float] = None

    # 修改阶段
    patch: Optional[PatchRecord] = None

    # 验证阶段
    verify_build: Optional[BuildResult] = None
    test_result: Optional[TestResult] = None
    vectorization_status: Optional[VectorizationStatus] = None

    # 性能 (可选)
    baseline_perf_ms: Optional[float] = None
    after_perf_ms: Optional[float] = None
    perf_delta_pct: Optional[float] = None

    # 时间戳
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None

    # AI 建议原文（用于 history 注入下轮 prompt）
    suggestion_description: Optional[str] = None
    applied_diff_summary: Optional[str] = None


@dataclass
class SessionRecord:
    """完整 AIMV 会话记录"""
    session_id: str = field(default_factory=lambda: f"aimv-{uuid.uuid4().hex[:12]}")
    function_name: str
    source_files: List[str] = field(default_factory=list)
    aimv_level: str = "moderate"
    max_rounds: int = 5
    target_loop_line: Optional[str] = None  # 目标循环源码位置（首轮自动选定）

    # 原始源码备份（会话开始时一次性创建）
    pristine_backup_dir: str = ""

    # 迭代记录
    rounds: List[RoundRecord] = field(default_factory=list)
    current_round: int = 0

    # 终止原因
    termination_reason: Optional[TerminationReason] = None

    # 最终结果
    final_patch_path: Optional[str] = None
    total_elapsed_ms: Optional[float] = None
    overall_perf_improvement_pct: Optional[float] = None

    # 元信息
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None
    cli_command: str = ""
    git_commit: str = ""
```

---

## 3. 子进程管理（build_orchestrator.py）

### 3.1 编译执行

```python
# [AIMV] driver/build_orchestrator.py

import subprocess
import tempfile
import os
import time
from pathlib import Path
from typing import Optional

class BuildOrchestrator:
    """管理 clang 编译和测试子进程。"""

    def __init__(self, config: dict):
        self.cc = config.get("cc", "clang")
        self.cflags = config.get("cflags", [])
        self.aimv_flags = config.get("aimv_flags", [])
        self.timeout_seconds = config.get("timeout", 120)
        self.work_dir = Path(config.get("work_dir", tempfile.mkdtemp(prefix="aimv-")))

# extract_function_source 实现策略:
#   - 首选: clang -Xclang -ast-dump 获取函数定义的行号范围
#   - 次选: ctags/exuberant-ctags 查找函数定义位置
#   - 回退: 基于 loop_location 行号截取前后 N 行
# extract_function_signature: 用正则匹配函数声明模式（返回类型 + 函数名 + 参数列表）
# extract_loop_line: 从 diagnostics[0].loop_location 解析 "<file>:<line>:<col>" 中的行号
# extract_lines_around: 读取文件指定行前后 context 行的源码

    def compile_with_aimv(
        self,
        source_file: str,
        output_file: str,
        target_function: Optional[str] = None,
        aimv_json_output: Optional[str] = None,
    ) -> BuildResult:
        """编译源码，启用 -fsave-optimization-record 和 AIMVFeedback Pass。

        关键 flags:
          -O2
          -g                                    (debug info → 源码映射)
          -fsave-optimization-record=<yaml>     (LLVM remark 序列化)
          -Rpass-missed=loop-vectorize          (只收集 missed remarks)
          -aimv-output=<json>                   (AIMVFeedback Pass 输出)
          -aimv-target-function=<name>          (可选，聚焦单个函数)
        """

        opt_record_path = str(self.work_dir / "opt-records.yaml")
        aimv_path = aimv_json_output or str(self.work_dir / "aimv-diag.json")

        cmd = [self.cc]
        cmd.extend(self.cflags)
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
            cmd,
            capture_output=True,
            text=True,
            timeout=self.timeout_seconds,
        )
        elapsed = (time.monotonic() - start) * 1000

        return BuildResult(
            returncode=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
            opt_record_path=opt_record_path,
            aimv_json_path=aimv_path,
            elapsed_ms=elapsed,
        )

    def run_tests(self, test_cmd: str) -> TestResult:
        """运行测试套件，解析通过/失败数。

        test_cmd 可以是任意 shell 命令。解析逻辑：
        - 优先匹配 CTest/JUnit 输出格式
        - 回退到检查 returncode
        """

        start = time.monotonic()
        proc = subprocess.run(
            test_cmd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=self.timeout_seconds * 2,  # 测试超时更长
        )
        elapsed = (time.monotonic() - start) * 1000

        passed, failed = self._parse_test_output(proc.stdout, proc.stderr)
        return TestResult(
            returncode=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
            passed=passed,
            failed=failed,
            elapsed_ms=elapsed,
        )

    def check_vectorization_from_json(self, aimv_json_path: str, function_name: str) -> VectorizationStatus:
        """解析 AIMV JSON，检查目标函数是否仍有点量化 missed remark。

        返回 VectorizationStatus。
        - 需要 !aimv.diag 同时包含 missed 和 passed 的循环信息;
        - 空 diagnostics 不能直接判定为"向量化成功"（可能是 pass 未运行或函数名不匹配）。
        - 实现期应增加"正向标记"：成功的循环也写入 !aimv.diag (severity="passed")。
        """
        import json

        with open(aimv_json_path) as f:
            data = json.load(f)

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

        # 空 diagnostics: 无法判定 → 保守报告为未向量化
        # (实现期需在 !aimv.diag 中同时写入 passed 和 missed 诊断)
        if total == 0:
            return VectorizationStatus(
                function_name=function_name,
                total_loops=0,
                vectorized_loops=0,
                missed_loops=0,
                missed_details=[{"remark_text": "No AIMV diagnostics found — pass may not have run or function name mismatch"}],
            )

        return VectorizationStatus(
            function_name=function_name,
            total_loops=total,
            vectorized_loops=passed,
            missed_loops=missed,
            missed_details=details,
        )

    def check_vectorization_from_yaml(self, opt_record_path: str, function_name: str) -> VectorizationStatus:
        """YAML 模式: 从 -fsave-optimization-record YAML 检查向量化状态。
        YAML 包含 passed/missed/analysis 三种 remark。
        目标函数无 missed remark → 判定为向量化成功。
        """
        import yaml

        with open(opt_record_path) as f:
            records = yaml.safe_load(f)

        total = 0
        missed = 0
        details = []
        for record in records:
            # YAML remark 结构: { Function, Pass, Name, type, ... }
            if (record.get("Function") == function_name and
                "loop-vectorize" in str(record.get("Pass", ""))):
                total += 1
                if record.get("type") == "missed":
                    missed += 1
                    details.append({
                        "remark_text": record.get("Name", ""),
                        "loop_location": record.get("DebugLoc", ""),
                    })

        if total == 0:
            return VectorizationStatus(
                function_name=function_name,
                total_loops=0,
                vectorized_loops=0,
                missed_loops=0,
                missed_details=[{"remark_text": "No loop-vectorize remarks in YAML"}],
            )

        return VectorizationStatus(
            function_name=function_name,
            total_loops=total,
            vectorized_loops=total - missed,
            missed_loops=missed,
            missed_details=details,
        )


def _check_target_loop_passed(vstatus: VectorizationStatus, target_loop: Optional[str]) -> bool:
    """检查目标循环是否已成功向量化。

    如果 target_loop 为 None（未指定目标），返回 False（由调用者处理全 passed 场景）。
    """
    if target_loop is None:
        return False
    # 在 passed 诊断中查找目标循环的 loop_location
    # （需要在 VectorizationStatus 中增加 passed_details 或直接遍历原始 JSON）
    # 实施期简化: 检查 missed_details 中是否不再包含 target_loop
    return not any(target_loop in d.get("loop_location", "")
                   for d in vstatus.missed_details)


def _parse_test_output(stdout: str, stderr: str) -> tuple[int, int]:
    """解析测试输出中的 pass/fail 计数。"""
    import re

    combined = stdout + stderr

    # CTest: "100% tests passed, 0 tests failed out of 5"
    m = re.search(r"(\d+)% tests passed.*?(\d+) tests? failed.*?out of (\d+)", combined)
    if m:
        total = int(m.group(3))
        failed = int(m.group(2))
        return (total - failed, failed)

    # GoogleTest: "[  PASSED  ] 5 tests."
    # GoogleTest: "[  FAILED  ] 1 test."
    passed = len(re.findall(r"\[\s*PASSED\s*\]", combined))
    failed = len(re.findall(r"\[\s*FAILED\s*\]", combined))
    if passed + failed > 0:
        return (passed, failed)

    # 无法解析，根据 returncode 粗略判断
    return (1, 1)  # 未知格式，保守假设有测试
```

### 3.2 超时与信号处理

```python
# [AIMV] 子进程超时 → 抛出 subprocess.TimeoutExpired
# driver 捕获后：
#   编译超时 → 记录日志，标记该 patch 为 COMPILE_ERROR，回滚
#   测试超时 → 记录日志，标记 TEST_FAILURE，回滚
#
# 额外保护：如果连续 2 轮超时，将 max_rounds 截断到当前轮次
#           (说明 AI 生成的代码有死循环或死锁)
```

---

## 4. 源码管理（source_manager.py）

### 4.1 原子 patch + 回滚

```python
# [AIMV] driver/source_manager.py

import os
import hashlib
import shutil
from pathlib import Path
from typing import Optional, List


class SourceManager:
    """管理源码的原子性修改和回滚。

    策略：每次应用 patch 之前，先 copy 原文件到 backup_dir。
    回滚时从 backup 恢复。使用 sha256 校验防止位翻转。
    """

    def __init__(self, backup_dir: str):
        self.backup_dir = Path(backup_dir)
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        self._patch_history: list[PatchRecord] = []

    def apply_patch(self, source_file: str, diff_text: str) -> PatchRecord:
        """应用 unified diff。失败时抛异常，不修改任何文件。

        步骤:
          1. 校验 source_file 存在且可读
          2. 计算原始文件 sha256
          3. 创建 backup copy
          4. 用 patch 命令应用 diff
          5. 验证修改后文件可编译（语法层）—— 可选
          6. 记录 PatchRecord

        兼容性: GNU patch 在 Linux/macOS 上标配。Windows 开发机需安装 Git for Windows
        （自带 patch.exe）或使用 WSL。也可增加纯 Python difflib 回退方案消除外部依赖。
        """

        src_path = Path(source_file).resolve()
        if not src_path.exists():
            raise FileNotFoundError(f"source file not found: {source_file}")

        # 计算原始 hash
        original_hash = self._sha256(src_path)

        # 创建备份
        backup_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.bak"
        shutil.copy2(src_path, backup_path)

        # 写入 diff 临时文件
        diff_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.diff"
        diff_path.write_text(diff_text, encoding="utf-8")

        # 应用 patch
        import subprocess
        proc = subprocess.run(
            ["patch", "-u", "--fuzz=2", str(src_path), str(diff_path)],
            capture_output=True, text=True,
        )
        if proc.returncode != 0:
            # patch 失败时可能已部分修改源文件 → 无条件从备份恢复
            shutil.copy2(backup_path, src_path)
            backup_path.unlink(missing_ok=True)
            diff_path.unlink(missing_ok=True)
            raise RuntimeError(f"patch apply failed: {proc.stderr}")

        # 记录
        record = PatchRecord(
            source_file=str(src_path),
            backup_path=str(backup_path),
            diff_text=diff_text.rstrip(),
            original_hash=original_hash,
        )
        self._patch_history.append(record)

        # 清理 diff 临时文件
        diff_path.unlink(missing_ok=True)

        return record

    def rollback(self, patch: PatchRecord) -> bool:
        """从 backup 恢复原始文件。成功返回 True。"""

        src_path = Path(patch.source_file)
        backup_path = Path(patch.backup_path)

        if not backup_path.exists():
            raise FileNotFoundError(f"backup not found: {backup_path}")

        # 校验 backup 完整性
        backup_hash = self._sha256(backup_path)
        if backup_hash != patch.original_hash:
            raise RuntimeError(
                f"backup hash mismatch: expected {patch.original_hash}, got {backup_hash}"
            )

        # 恢复
        shutil.copy2(backup_path, src_path)
        return True

    def rollback_all(self):
        """按反序回滚所有 patch（从最新到最早）。
        单条回滚失败不中断后续回滚，收集所有错误后统一 raise。"""
        errors = []
        for patch in reversed(self._patch_history):
            try:
                self.rollback(patch)
            except Exception as e:
                errors.append((patch.source_file, str(e)))
        if errors:
            raise RuntimeError(f"rollback_all: {len(errors)} failures: {errors}")

    def get_current_diff(self) -> Optional[str]:
        """返回最近一轮的 diff（用于 history 注入）"""
        if self._patch_history:
            return self._patch_history[-1].diff_text
        return None

    def get_all_modified_files(self) -> List[str]:
        """返回所有被修改过的源文件列表"""
        return list(set(p.source_file for p in self._patch_history))

    @staticmethod
    def _sha256(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()
```

### 4.2 崩溃恢复

```python
# [AIMV] 崩溃恢复策略:
#
# 启动时检查 backup_dir 中是否有残留的 .bak 文件
#   → 存在 → 询问用户是否恢复到 pre-aimv 原始状态
#          → 是 → 从 pristine_backup 恢复所有文件
#          → 否 → 报告路径，让用户手工处理
#
# Pristine backup: 会话开始时创建完整源码 tree 的快照
#   aimv-driver --resume <session_id> → 从 session JSON 恢复状态
```

---

## 5. MCP 客户端（mcp_client.py）

```python
# [AIMV] driver/mcp_client.py

import httpx
import time
from typing import Optional

class MCPClient:
    """MCP 分析服务 REST 客户端。

    特性:
      - 指数退避重试 (max 2 次)
      - 超时处理 (60s)
      - 连接健康检查
      - 响应格式验证
    """

    def __init__(self, base_url: str, timeout_seconds: int = 60):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout_seconds
        self._client = httpx.Client(timeout=timeout_seconds + 10)

    def analyze(self, request_json: dict) -> Optional[dict]:
        """POST /api/v1/analyze-vectorization，返回解析后的响应 dict。

        retry 策略:
          尝试 1 (立即) → 失败? → 等待 2s → 尝试 2 → 失败? → 等待 4s → 尝试 3
          3 次都失败 → 返回 None (driver 记录 ERROR 并放弃本轮)
        """

        url = f"{self.base_url}/api/v1/analyze-vectorization"

        for attempt in range(3):
            try:
                start = time.monotonic()
                resp = self._client.post(url, json=request_json)
                elapsed = (time.monotonic() - start) * 1000

                if resp.status_code == 200:
                    return resp.json()
                elif resp.status_code == 422:
                    # 参数错误，不可重试
                    raise ValueError(f"MCP validation error: {resp.text}")
                elif resp.status_code in (429, 500, 502, 503):
                    # 可重试
                    if attempt < 2:
                        wait = 2.0 * (2 ** attempt)
                        time.sleep(wait)
                        continue
                    return None
                else:
                    return None

            except (httpx.TimeoutException, httpx.ConnectError) as e:
                if attempt < 2:
                    wait = 2.0 * (2 ** attempt)
                    time.sleep(wait)
                    continue
                return None

        return None

    def health(self) -> bool:
        """GET /api/v1/health"""
        try:
            resp = self._client.get(f"{self.base_url}/api/v1/health")
            return resp.status_code == 200
        except Exception:
            return False
```

---

## 6. 迭代决策引擎（iteration_engine.py）

### 6.1 决策逻辑

```python
# [AIMV] driver/iteration_engine.py

from enum import Enum
from typing import Optional

class NextAction(Enum):
    CONTINUE = "continue"              # 继续当前方向，增加激进
    RETRY_SAME = "retry_same"          # 建议失败但可重试（编译错/超时），同一轮重试一次
    ESCALATE_LEVEL = "escalate_level"  # 当前激进下无建议，提升 level
    ROLLBACK = "rollback"              # 回滚最近的 patch
    STOP = "stop"                      # 终止（成功 or 无法继续）


class IterationEngine:
    """根据当前状态决定下一步行动。

    决策矩阵:
    ┌──────────────────────┬───────────────────────────────────┐
    │ 状态                 │ 动作                              │
    ├──────────────────────┼───────────────────────────────────┤
    │ 向量化成功           │ STOP (VECTORIZED)                  │
    │ 达到 max_rounds      │ STOP (ROUND_LIMIT)                 │
    │ 编译失败             │ ROLLBACK → 当前 round 重试一次     │
    │                      │ 再次失败 → STOP (COMPILE_ERROR)    │
    │ 测试失败             │ ROLLBACK → STOP (TEST_FAILURE)     │
    │ MCP 超时/无响应      │ mcp_client 内部重试 3 次            │
    │                      │ 全部失败 → STOP                    │
    │ MCP 返回空建议       │ 升激进 (conservative→moderate→     │
    │                      │         aggressive)               │
    │                      │ 已是 aggressive → STOP             │
    │ 性能退化             │ ROLLBACK → STOP (NO_IMPROVEMENT)   │
    │ 性能不变             │ 继续下一轮 (不同策略)              │
    └──────────────────────┴───────────────────────────────────┘
    """

    def __init__(self, initial_level: str = "moderate", max_rounds: int = 5,
                 perf_degradation_threshold_pct: float = 5.0):
        self.initial_level = initial_level
        self.current_level = initial_level
        self.max_rounds = max_rounds
        self.degradation_threshold = perf_degradation_threshold_pct

        # 追踪状态
        self._consecutive_compile_failures = 0
        self._consecutive_mcp_failures = 0
        self._level_escalations = 0

    def decide(
        self,
        current_round: int,
        build_result_ok: bool,
        test_result_ok: bool,
        vectorized: bool,
        mcp_had_suggestions: bool,
        mcp_responded: bool,
        perf_delta_pct: Optional[float] = None,
    ) -> tuple[NextAction, str]:
        """返回 (action, reason) 元组。"""

        # 成功
        if vectorized:
            return NextAction.STOP, "vectorization succeeded"

        # 轮次上限
        if current_round >= self.max_rounds:
            return NextAction.STOP, f"reached max rounds ({self.max_rounds})"

        # 编译失败
        if not build_result_ok:
            self._consecutive_compile_failures += 1
            if self._consecutive_compile_failures >= 2:
                return NextAction.STOP, "consecutive compile failures"
            return NextAction.ROLLBACK, "compile failure, will retry with different approach"

        self._consecutive_compile_failures = 0

        # 测试失败 → 直接回滚并终止（语义错误，重试无意义）
        if not test_result_ok:
            return NextAction.ROLLBACK, "test failure, stopping"

        # MCP 无响应（网络重试由 mcp_client 层处理，引擎层直接判定失败）
        if not mcp_responded:
            self._consecutive_mcp_failures += 1
            if self._consecutive_mcp_failures >= 1:
                return NextAction.STOP, "MCP server unresponsive"
            return NextAction.RETRY_SAME, "MCP timeout, one retry"

        self._consecutive_mcp_failures = 0

        # MCP 无建议
        if not mcp_had_suggestions:
            escalated = self._try_escalate()
            if escalated:
                return NextAction.ESCALATE_LEVEL, f"escalated to {self.current_level}"
            return NextAction.STOP, "no suggestions available at highest level"

        # 性能退化
        if perf_delta_pct is not None and perf_delta_pct < -self.degradation_threshold:
            return NextAction.ROLLBACK, f"performance degraded by {-perf_delta_pct:.1f}%"

        # 继续
        return NextAction.CONTINUE, "continuing to next round"

    def _try_escalate(self) -> bool:
        """尝试提升激进度。成功返回 True。"""
        levels = ["conservative", "moderate", "aggressive"]
        idx = levels.index(self.current_level)
        if idx < len(levels) - 1:
            self.current_level = levels[idx + 1]
            self._level_escalations += 1
            return True
        return False

    def reset(self):
        self.current_level = self.initial_level
        self._consecutive_compile_failures = 0
        self._consecutive_mcp_failures = 0
        self._level_escalations = 0
```

### 6.2 迭代策略图示

```
Round N 开始
    │
    ├── 编译 + AIMV Pass ──→ 全部向量化? ──→ SUCCESS
    │                                  │
    │                                 否
    │                                  │
    ├── POST MCP Server ──→ 超时/无响应? ──→ mcp_client 内部重试 (最多 3 次)
    │              │                                │
    │              │                         全部失败 → GIVE_UP
    │              │
    │              └── 200 OK ──→ no_action_possible?
    │                                      │
    │                                     是 → 升激进度 (C→M→A)
    │                                      │        │
    │                                      │   已是 A → GIVE_UP
    │                                      │
    │                                     否
    │                                      │
    ├── 应用 diff patch ──→ patch 失败? ──→ 编译失败? → ROLLBACK → retry
    │              │                  (同轮 1 次重试，再次失败 → GIVE_UP)
    │              │
    │              └── patch 成功
    │                      │
    ├── 重编译验证 ──→ 编译失败? ──→ ROLLBACK → retry → 仍失败 → GIVE_UP
    │                      │
    │                      └── 编译成功 → 检查向量化 remark
    │                              │
    │                             向量化成功 → SUCCESS
    │                             仍失败:
    │                                │
    ├── 运行测试套件 ──→ 失败? ──→ ROLLBACK → GIVE_UP
    │                      │
    │                      └── 通过
    │                              │
    ├── [可选] 性能测量 ──→ 退化 > 5%? ──→ ROLLBACK → GIVE_UP
    │                              │
    │                              └── 不变或改善
    │                                      │
    └── Round N+1 ──→ (N < max_rounds) ──→ 回到顶部
                              │
                        N == max_rounds → GIVE_UP
```

---

## 7. Session 持久化（session_store.py）

### 7.1 格式

```python
# [AIMV] driver/session_store.py

import json
import os
from pathlib import Path
from dataclasses import asdict
from typing import Optional

class SessionStore:
    """Session JSON 持久化。

    JSON schema: 与 SessionRecord 的 dataclass 字段一一对应。
    文件路径: <output_dir>/sessions/<session_id>.json

    写入策略:
      - 每轮迭代结束后自动写入 (原子写: 写 tmp + rename)
      - 不依赖外部数据库
    """

    def __init__(self, output_dir: str):
        self.sessions_dir = Path(output_dir) / "sessions"
        self.sessions_dir.mkdir(parents=True, exist_ok=True)

    def save(self, session: SessionRecord):
        """原子写入 session JSON。"""
        data = self._serialize(session)
        path = self.sessions_dir / f"{session.session_id}.json"
        tmp_path = path.with_suffix(".tmp")

        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False, default=str)

        os.replace(tmp_path, path)  # 原子 rename

    def load(self, session_id: str) -> Optional[SessionRecord]:
        """从磁盘恢复 session。"""
        path = self.sessions_dir / f"{session_id}.json"
        if not path.exists():
            return None

        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        return self._deserialize(data)

    def list_sessions(self) -> list[dict]:
        """列出所有 session 摘要。"""
        sessions = []
        for path in self.sessions_dir.glob("*.json"):
            with open(path) as f:
                data = json.load(f)
            sessions.append({
                "session_id": data["session_id"],
                "function_name": data["function_name"],
                "status": data.get("termination_reason", "in_progress"),
                "rounds": len(data.get("rounds", [])),
                "started_at": data.get("started_at"),
                "finished_at": data.get("finished_at"),
            })
        return sorted(sessions, key=lambda s: s["started_at"] or "", reverse=True)

    def _serialize(self, session: SessionRecord) -> dict:
        """将 dataclass 递归序列化为 JSON 兼容 dict。"""
        # 注意: PatchRecord.diff_text 可能包含特殊字符，但 JSON 正确处理
        return _dataclass_to_dict(session)

    def _deserialize(self, data: dict) -> SessionRecord:
        return _dict_to_session(data)


def _dataclass_to_dict(obj) -> dict:
    """递归 dataclass → dict"""
    if hasattr(obj, "__dataclass_fields__"):
        result = {}
        for field_name in obj.__dataclass_fields__:
            value = getattr(obj, field_name)
            result[field_name] = _dataclass_to_dict(value)
        return result
    elif isinstance(obj, list):
        return [_dataclass_to_dict(item) for item in obj]
    elif isinstance(obj, dict):
        return {k: _dataclass_to_dict(v) for k, v in obj.items()}
    elif isinstance(obj, Enum):
        return obj.value
    else:
        return obj
```

### 7.2 崩溃恢复流程

```
aimv-driver 启动
    │
    ├── 检查 --resume <session_id>
    │     │
    │     └── session_id 存在 → load session JSON → 恢复到 last_round 状态
    │           │
    │           ├── 检查 backup_dir 是否有残留 .bak
    │           │     └── 有 → 警告用户 "上次可能未正确清理"，从 pristine 恢复
    │           │
    │           ├── 从 session.current_round + 1 继续迭代
    │           └── 注入 history (前几轮的 AI 建议和结果)
    │
    └── 正常启动
          │
          └── 创建 pristine backup → 开始 Round 1
```

---

## 8. 主循环实现

```python
# [AIMV] driver/aimv_driver.py (核心循环，伪代码)

def main_loop(driver_config: dict) -> int:
    """主迭代循环。返回 0 表示成功，非 0 表示失败。"""

    # 初始化模块
    session = SessionRecord(
        function_name=driver_config["function"],
        source_files=driver_config["source_files"],
        aimv_level=driver_config["aimv_level"],
        max_rounds=driver_config["max_rounds"],
        cli_command=" ".join(sys.argv),
    )

    builder = BuildOrchestrator(driver_config)
    sources = SourceManager(driver_config["backup_dir"])
    mcp = MCPClient(driver_config["mcp_url"])
    engine = IterationEngine(driver_config["aimv_level"], driver_config["max_rounds"])
    store = SessionStore(driver_config["output_dir"])

    # 创建 pristine backup
    for src in session.source_files:
        pristine_dir = Path(driver_config["backup_dir"]) / "pristine"
        pristine_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, pristine_dir / Path(src).name)

    # 主循环
    try:
        while True:
            round_rec = RoundRecord(round_number=len(session.rounds) + 1)
            session.rounds.append(round_rec)
            session.current_round = round_rec.round_number

            # Step 1: 编译 + AIMV Pass
            round_rec.status = DriverStatus.COMPILING
            build = builder.compile_with_aimv(
                source_file=session.source_files[0],
                output_file=driver_config["output_binary"],
                target_function=session.function_name,
            )
            round_rec.build_result = build

            if build.returncode != 0:
                action, reason = engine.decide(
                    current_round=round_rec.round_number,
                    build_result_ok=False, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=False, mcp_responded=True,
                )
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                # action == RETRY_SAME → 重试同轮
                sources.rollback_all()
                continue

            # 检查向量化状态（双模式）
            # 策略: 按目标循环判断，非函数级全量判断。
            # 函数可能有多个循环，部分 passed 部分仍然 missed。
            # Driver 每轮只关注 session.target_loop_line 指定的循环
            # （首次自动选取第一个 missed 循环）。
            if Path(build.aimv_json_path).exists():
                vstatus = builder.check_vectorization_from_json(
                    build.aimv_json_path, session.function_name)
            else:
                # YAML 模式: 从 opt-record YAML 检查是否有 missed remark
                # 注意: YAML 模式只有 passed/missed/analysis 的 type 字段，
                # 无代价模型和依赖分析数据，LLM 可用的信息较少（此为已知限制）。
                vstatus = builder.check_vectorization_from_yaml(
                    build.opt_record_path, session.function_name)
            round_rec.vectorization_status = vstatus

            # 首轮自动选定目标循环（如果有多个 missed 循环）
            if session.target_loop_line is None and vstatus.missed_details:
                session.target_loop_line = vstatus.missed_details[0].get("loop_location")

            # 终止判定: 检查目标循环是否已从 missed 变为 passed
            if vstatus.total_loops > 0:
                target_passed = _check_target_loop_passed(vstatus, session.target_loop_line)
                if target_passed:
                    session.termination_reason = TerminationReason.VECTORIZED
                    log_success(session)
                    break
                elif vstatus.missed_loops == 0:
                    # 所有循环都 passed（即使没指定目标循环）
                    session.termination_reason = TerminationReason.VECTORIZED
                    log_success(session)
                    break
            elif vstatus.total_loops == 0:
                log_warning("No diagnostics found; pass may not have run or function has no loops")
                session.termination_reason = TerminationReason.NO_SUGGESTION
                break

            # 加载诊断数据（双模式）
            if Path(build.aimv_json_path).exists():
                # Pass 模式: 读取 AIMVFeedbackPass JSON（丰富诊断）
                with open(build.aimv_json_path) as f:
                    aimv_json = json.load(f)
            else:
                # YAML 模式: 从 opt-record YAML 解析基础诊断（零侵入）
                # 注意: YAML 模式无代价模型和依赖分析数据，LLM 可用的信息较少
                aimv_json = opt_info_parser.parse_to_analyze_request(
                    build.opt_record_path,
                    session.function_name,
                    session.source_files[0],
                )

            # 补充 function 字段（Pass JSON 只有 diagnostics + target，
            # AnalyzeRequest 要求完整的 FunctionInfo）
            # 注意: source_code 只提取目标函数的源码片段，不发整个文件
            # （避免 token 浪费、LLM 混淆、大文件超上下文窗口）。
            func_source = extract_function_source(
                session.source_files[0], session.function_name)
            if func_source is None:
                # 回退: 函数提取失败时截取 loop_location 前后 40 行
                loop_line = extract_loop_line(
                    aimv_json.get("diagnostics", []), session.function_name)
                func_source = extract_lines_around(
                    session.source_files[0], loop_line, context=40)

            aimv_json["function"] = {
                "name": session.function_name,
                "signature": extract_function_signature(
                    session.source_files[0], session.function_name),
                "source_code": func_source,
                "source_file": session.source_files[0],
                "loop_line": extract_loop_line(
                    aimv_json.get("diagnostics", []), session.function_name),
            }

            # 注入 history
            aimv_json["history"] = build_history(session)
            aimv_json["aimv_level"] = engine.current_level

            # Step 2: MCP 查询
            round_rec.status = DriverStatus.QUERYING_MCP
            mcp_resp = mcp.analyze(aimv_json)
            round_rec.mcp_response = mcp_resp

            mcp_responded = mcp_resp is not None
            mcp_had_suggestions = bool(mcp_resp and mcp_resp.get("suggestions"))

            if not mcp_responded or not mcp_had_suggestions:
                action, reason = engine.decide(
                    current_round=round_rec.round_number,
                    build_result_ok=True, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=mcp_had_suggestions,
                    mcp_responded=mcp_responded,
                )
                if action == NextAction.ESCALATE_LEVEL:
                    continue  # 用新 level 重复本轮
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.NO_SUGGESTION
                    break
                continue  # RETRY_SAME

            # Step 3: 应用 patch
            round_rec.status = DriverStatus.PATCHING
            suggestion = mcp_resp["suggestions"][0]

            # 循环检测: 比较 diff 与历史 patch，防止 LLM 重复建议同一修改
            new_diff = suggestion["diff"]
            if any(p.diff_text.strip() == new_diff.strip()
                   for p in sources._patch_history):
                log_warning("Duplicate patch detected; LLM suggested same change as previous round")
                session.termination_reason = TerminationReason.NO_IMPROVEMENT
                store.save(session)
                break

            patch = sources.apply_patch(suggestion["source_file"], new_diff)
            round_rec.patch = patch
            store.save(session)

            # Step 4: 重编译验证
            round_rec.status = DriverStatus.VERIFYING
            verify_build = builder.compile_with_aimv(
                source_file=session.source_files[0],
                output_file=driver_config["output_binary"],
                target_function=session.function_name,
            )
            round_rec.verify_build = verify_build

            # 验证后持久化
            store.save(session)

            if verify_build.returncode != 0:
                sources.rollback(patch)
                store.save(session)
                action, reason = engine.decide(
                    current_round=round_rec.round_number,
                    build_result_ok=False, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=True, mcp_responded=True,
                )
                if action == NextAction.STOP:
                    session.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                continue

            # Step 5: 测试
            test = builder.run_tests(driver_config["test_cmd"])
            round_rec.test_result = test

            if test.returncode != 0 or test.failed > 0:
                # 测试失败 = patch 引入语义错误，回滚并立即终止（不重试）
                sources.rollback(patch)
                round_rec.status = DriverStatus.FAILED
                session.termination_reason = TerminationReason.TEST_FAILURE
                store.save(session)
                break

            # Step 6: (可选) 性能测量
            if driver_config.get("measure_perf"):
                round_rec.status = DriverStatus.MEASURING
                baseline = session.rounds[0].baseline_perf_ms  # 首轮基线
                current = measure_performance(driver_config["perf_cmd"])
                perf_delta = ((baseline - current) / baseline) * 100 if baseline else None

                action, reason = engine.decide(
                    current_round=round_rec.round_number,
                    build_result_ok=True, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=True, mcp_responded=True,
                    perf_delta_pct=perf_delta,
                )
                if action == NextAction.ROLLBACK:
                    sources.rollback(patch)
                    session.termination_reason = TerminationReason.NO_IMPROVEMENT
                    break
            else:
                action, reason = engine.decide(
                    current_round=round_rec.round_number,
                    build_result_ok=True, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=True, mcp_responded=True,
                )

            if action == NextAction.STOP:
                session.termination_reason = TerminationReason.ROUND_LIMIT
                break

            # 持久化
            store.save(session)

    except KeyboardInterrupt:
        session.termination_reason = TerminationReason.INTERRUPTED
        sources.rollback_all()
        log_interrupted(session)

    finally:
        session.finished_at = time.time()
        session.total_elapsed_ms = (session.finished_at - session.started_at) * 1000
        store.save(session)

    return 0 if session.termination_reason == TerminationReason.VECTORIZED else 1
```

---

## 9. 并发模型

### 9.1 设计

AIMV 操作的对象是**源码文件**。多函数并行分析存在文件冲突风险（两个函数在同一文件中，同时被修改）。

```python
# 并发策略: 以 source_file 为锁粒度
#
#   aimv-driver --function=func_a src.c  ─┐
#   aimv-driver --function=func_b src.c  ─┤─→ 检测到 src.c 被占用
#                                         │   → 排队等待 / 报错退出
#   aimv-driver --function=func_c util.c ─┘   → 并行执行 (不同文件)
#
# 实现: 文件锁 (flock / LockFile)
#   <output_dir>/locks/<sha256_of_abspath>.lock
```

```python
# [AIMV] driver/source_manager.py (补充)

import fcntl  # Linux / macOS
# Windows: import msvcrt

class FileLock:
    """跨进程文件锁"""

    def __init__(self, lock_dir: str):
        self.lock_dir = Path(lock_dir)
        self.lock_dir.mkdir(parents=True, exist_ok=True)
        self._locks: dict[str, int] = {}

    def acquire(self, source_file: str, timeout_seconds: int = 30) -> bool:
        """获取文件锁。timeout 秒内未获取返回 False。"""
        import hashlib

        path = Path(source_file).resolve()
        lock_name = hashlib.sha256(str(path).encode()).hexdigest()[:16]
        lock_path = self.lock_dir / f"{lock_name}.lock"

        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR)
        deadline = time.time() + timeout_seconds

        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self._locks[str(path)] = fd
                return True
            except BlockingIOError:
                if time.time() > deadline:
                    os.close(fd)
                    return False
                time.sleep(0.5)

    def release(self, source_file: str):
        path = str(Path(source_file).resolve())
        fd = self._locks.pop(path, None)
        if fd is not None:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)
```

---

## 10. 配置与 CLI

### 10.1 完整 CLI

```
aimv-driver [OPTIONS] <source_file>

OPTIONS:
  --function FUNC          目标函数名（必填）
  --aimv-level LEVEL       修改激进度 (conservative|moderate|aggressive, 默认 moderate)
  --max-rounds N           最大迭代轮次（默认 5）
  --mcp-url URL            MCP 服务地址（默认 http://localhost:8080）
  --output-dir DIR         输出目录（默认 ./aimv-output）
  --build-cmd CMD          编译命令模板（默认 "clang -O2 {src} -o {out}"）
  --test-cmd CMD           测试命令（默认 "make test"）
  --perf-cmd CMD           性能测量命令（可选）
  --measure-perf           启用性能测量
  --resume SESSION_ID      从 session 恢复
  --list-sessions          列出所有 session
  --dry-run                仅编译+诊断，不调用 MCP 不修改源码
  --verbose                详细日志
  --json-log               日志以 JSON 格式输出到文件

EXAMPLES:
  # 基本使用
  aimv-driver --function=process_task src/task.c

  # 指定 MCP 服务器 + 保守模式
  aimv-driver --function=process_task --mcp-url=https://aimv.example.com \
              --aimv-level=conservative src/task.c

  # 恢复中断的 session
  aimv-driver --resume aimv-a1b2c3d4e5f6
```

---

## 11. 错误处理矩阵

| 场景 | 检测方式 | 处理 |
|------|---------|------|
| 源文件不存在 | 启动时检查 | 立即退出，exit code 2 |
| clang 未安装 | 编译失败，stderr 含 "command not found" | 立即退出，exit code 3 |
| MCP 服务不可达 | 健康检查 + POST 超时 | 3 次重试，仍失败则退出，exit code 4 |
| MCP 返回非 JSON | JSONDecodeError | 记录原始响应到日志，重试 1 次 |
| MCP 返回空建议 | no_action_possible=true | 升激进度或放弃 |
| MCP 建议 diff 格式错误 | patch 命令失败 | 回滚，记录，重试（同 round 1 次） |
| 编译失败（patch 引入语法错） | returncode != 0 | 回滚，重试（同 round 1 次），再次失败则停止 |
| 测试失败 | returncode != 0 或解析到 failed > 0 | 回滚，停止 |
| 性能退化 >5% | 可选 perf 测量 | 回滚，停止 |
| 磁盘满 | OSError (ENOSPC) | 停止，保留已完成 session，exit code 5 |
| 用户中断 (Ctrl+C) | KeyboardInterrupt | 回滚所有 patch，写入 session，exit code 130 |
| 进程被 kill (SIGTERM) | signal handler | 回滚所有 patch，写入 session，exit code 143 |

---

## 12. 依赖

```
aimv/driver/requirements.txt

httpx>=0.27.0
pyyaml>=6.0
jinja2>=3.1                   # 仅用于可选的自定义 prompt 模板
```

零额外依赖 —— 核心逻辑只用 stdlib（subprocess, json, hashlib, fcntl, pathlib, dataclasses, time）。httpx 是唯一的第三方依赖（HTTP 客户端），可选替换为 urllib。

---

*文档版本: 1.0*
*创建日期: 2026-04-29*
