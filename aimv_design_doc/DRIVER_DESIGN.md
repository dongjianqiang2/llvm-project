# AIMV — aimv-driver 详细设计方案

**版本**: 2.0
**日期**: 2026-05-17
**关联文档**: SPEC.md v1.4, PLAN.md v1.1, LLVM_DESIGN.md, MCP_DESIGN.md
**数据模型权威源**: MCP_DESIGN.md > PLAN.md > SPEC.md

---

## 0. 设计目标

`aimv-driver` 是 AIMV 闭环的编排中枢。它不实现任何编译优化逻辑，只负责将诊断信息（来自 LLVM Pass 或 YAML opt-records）、MCP Server（AI 分析）、源码 patch + 重编译验证串联为可控的迭代循环。

**双模式运行**（对应 SPEC §3.1 + §3.2）:

| 模式 | 诊断来源 | 启动方式 | 典型场景 |
|------|---------|---------|---------|
| **`--from-json` 模式** | AIMVFeedbackPass JSON（首次），后续轮次自产 | `aimv-driver --from-json=aimv.json --source=task.c`（由 clang Driver fork+exec） | clang `-faimv` 原生集成，主入口 |
| **独立模式** | 自行编译获取 YAML/JSON | `aimv-driver --function=foo task.c` | 开发者手动控制、CI 集成 |

核心职责：
1. **编译编排** — 管理 clang 子进程，注入 AIMV flags（`-mllvm -aimv-enable`，永不传 `-faimv`），解析 opt-records
2. **MCP 通信** — 发送诊断 JSON，接收结构化建议，处理超时/重试
3. **源码管理** — 影子文件 + 原子替换协议（SPEC §3.1），FileLock 并发控制
4. **验证编排** — 运行测试套件，检查向量化 remark，可选性能测量
5. **迭代决策** — 根据终止条件（向量化成功 / 轮次上限 / 收益退化）决定继续/回滚/放弃
6. **持久化** — 完整 Session JSON，traceability artifacts（累积 patch、session 记录、备份）

---

## 1. 模块架构

```
aimv/driver/
├── aimv_driver.py              # CLI 入口 + 顶层编排循环
├── build_orchestrator.py       # 子进程管理（clang、test）
├── opt_info_parser.py          # opt-record YAML/JSON 解析
├── mcp_client.py               # MCP REST 客户端（含重试）
├── source_manager.py           # 源码 patch + 回滚 + 影子文件协议 + FileLock
├── iteration_engine.py         # 迭代策略决策引擎
├── session_store.py            # Session 持久化与恢复
├── perf_measurer.py            # 性能测量（可选，Phase 2）
├── models.py                   # 内部数据模型（PerFunctionResult, RoundRecord 等）
├── config.py                   # 配置加载（~/.aimv/config + env vars + defaults）
└── requirements.txt
```

### 1.1 模块交互

```
                    ┌──────────────────────────────────┐
                    │        aimv_driver.py             │
                    │   CLI + --from-json 入口          │
                    │   多函数顺序编排主循环             │
                    └──────┬───────────────────────────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
┌─────────────────┐ ┌─────────────┐ ┌──────────────────┐
│build_orchestrator│ │mcp_client   │ │source_manager    │
│· compile()      │ │· analyze()  │ │· apply_shadow()  │
│· test()         │ │· health()   │ │· rollback()      │
│· get_remarks()  │ │             │ │· check_stale()   │
└────────┬────────┘ └──────┬──────┘ │· FileLock        │
         │                 │        └────────┬─────────┘
         ▼                 ▼                 │
┌─────────────────┐ ┌─────────────┐          │
│opt_info_parser  │ │config       │ ┌────────┴─────────┐
│· parse_yaml()   │ │· load()     │ │session_store     │
│· parse_json()   │ │· validate() │ │· save()          │
│· extract_loops()│ │             │ │· resume()        │
└─────────────────┘ └─────────────┘ └──────────────────┘
                           │
          ┌────────────────┤
          ▼                ▼
┌─────────────────┐ ┌──────────────────┐
│iteration_engine │ │perf_measurer     │
│· decide_next()  │ │· measure()       │  (Phase 2)
│· should_stop()  │ │· compare()       │
└─────────────────┘ └──────────────────┘
```

---

## 2. `--from-json` 入口（主模式）

这是 clang Driver fork+exec 调用时的主入口，对应 PLAN §4.3 描述的完整执行路径。

### 2.1 执行流程

```python
# [AIMV] driver/aimv_driver.py

def main_from_json(aimv_json_path: str, source_file: str) -> int:
    """--from-json 入口：由 clang Driver fork+exec 调用。

    执行路径（PLAN §4.3）:
      1. 读 aimv.json → 提取 severity=="missed" 的函数名列表
      2. 若列表为空 → stderr "nothing to do" → exit 0
      3. 加载配置: ~/.aimv/config + 环境变量 → defaults
      4. 对每个失败函数顺序处理:
         a. 读当前源文件（已包含前面函数已通过的变更）
         b. 迭代循环: 编译 → MCP → 影子文件 patch → 验证
         c. 向量化成功 → 处理下一个函数
         d. 到达 max_rounds → 该函数放弃，处理下一个函数
      5. 全部函数处理完毕 → 输出汇总到 stderr → exit 0
    """
    import json
    import sys

    # Step 1: 读取 aimv.json
    # 注: aimv.json 是首轮编译由 AIMVFeedbackPass 产出的完整诊断文件。
    #     后续轮次中 BuildOrchestrator 会重新编译并产出新的 JSON 到独立路径
    #     (aimv-<func>-r<N>.json)，首轮的 aimv.json 不变。
    #     Driver 每轮从新的 JSON 文件读取最新诊断。
    with open(aimv_json_path) as f:
        aimv_data = json.load(f)

    diagnostics = aimv_data.get("diagnostics", [])

    # 提取所有 severity=="missed" 的函数名（去重，保持顺序）
    failed_functions = list(dict.fromkeys(
        d["function_name"]
        for d in diagnostics
        if d.get("severity") == "missed"
    ))

    # Step 2: 空诊断 → 直接退出
    if not failed_functions:
        print("[AIMV] all loops already vectorized, nothing to do",
              file=sys.stderr)
        return 0

    # Step 3: 加载配置
    config = load_config()  # ~/.aimv/config > env vars > defaults

    # 初始化模块
    builder = BuildOrchestrator(config)
    mcp = MCPClient(config.mcp_url, config.mcp_timeout, api_key=config.mcp_api_key)
    engine = IterationEngine(config.aimv_level, config.max_rounds)
    store = SessionStore(config.output_dir)
    sources = SourceManager(config.output_dir)

    # 创建 pristine backup（会话级，仅一次）
    pristine_dir = Path(config.output_dir) / "backups" / "pristine"
    pristine_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_file, pristine_dir / Path(source_file).name)

    # 创建 session
    session = SessionRecord(
        source_file=source_file,
        aimv_level=config.aimv_level,
        max_rounds=config.max_rounds,
        session_id=f"aimv-{uuid.uuid4().hex[:12]}",
    )

    # Step 4: 对每个失败函数顺序处理
    results: list[PerFunctionResult] = []

    for func_name in failed_functions:
        func_result = process_single_function(
            function_name=func_name,
            source_file=source_file,
            initial_diagnostics=[
                d for d in diagnostics
                if d["function_name"] == func_name and d.get("severity") == "missed"
            ],
            config=config,
            builder=builder,
            mcp=mcp,
            engine=IterationEngine(config.aimv_level, config.max_rounds),
            sources=sources,
            store=store,
            session=session,
        )
        results.append(func_result)
        # 函数 A 验证通过后变更已通过 atomic mv 写入原文件
        # 函数 B 编译时看到的源码已包含 A 的变更

    # Step 5: 输出汇总到 stderr
    emit_summary(results, source_file, session, store, config)

    return 0
```

### 2.2 防无限 fork 设计

```
Round 1 (用户触发):
  clang -O2 -faimv -c task.c           ← 用户命令，含 -faimv
    ├─ Driver 编译 + AIMVFeedbackPass
    └─ Driver fork aimv-driver --from-json=aimv.json

Round 1-N (aimv-driver 内部，BuildOrchestrator 调用 clang):
  clang -O2 -mllvm -aimv-enable -mllvm -aimv-output=<path> -c task.c
    ├─ 不含 -faimv → 不会触发 Driver 再 fork
    ├─ 只运行 LLVM 层的 AIMVFeedbackPass 收集诊断
    └─ aimv-driver 读新 aimv.json 判断下一轮

关键: -faimv 只存在于 clang Driver 层（C++）。
      aimv-driver 内部只传 LLVM 后端 flag (-mllvm -aimv-enable)，
      永远不传 -faimv → 不会产生 fork 链。
```

### 2.3 空诊断行为

`aimv.json` 中所有 diagnostics 的 `severity == "passed"` 时，driver 直接 exit 0：

```
[AIMV] all loops already vectorized, nothing to do
```

clang Driver 将该 exit 0 视为成功。

---

## 3. 内部数据模型

### 3.1 状态枚举

```python
# [AIMV] driver/models.py

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional, List, Dict
import uuid
import time


class IterationStatus(Enum):
    PENDING = "pending"
    COMPILING = "compiling"
    ANALYZING = "analyzing"
    QUERYING = "querying"           # MCP 查询（不持锁）
    PATCHING = "patching"           # 影子文件 patch（持锁区间内）
    VERIFYING = "verifying"         # 重编译 + 测试（同一持锁区间内）
    SUCCESS = "success"
    FAILED = "failed"
    ROLLED_BACK = "rolled_back"


class TerminationReason(Enum):
    VECTORIZED = "vectorized"              # 函数成功向量化
    ROUND_LIMIT = "round_limit"            # 达到 max_rounds
    NO_IMPROVEMENT = "no_improvement"      # passed remark 数量减少 → 回滚
    NO_SUGGESTION = "no_suggestion"        # MCP 返回 no_action_possible
    COMPILE_ERROR = "compile_error"        # patch 导致编译失败
    TEST_FAILURE = "test_failure"          # patch 导致测试失败
    INTERRUPTED = "interrupted"            # 用户中断 (Ctrl+C)
```

### 3.2 编译与测试结果

```python
# [AIMV] driver/models.py

@dataclass
class BuildResult:
    """单次编译结果"""
    returncode: int
    stdout: str
    stderr: str
    opt_record_path: str                   # YAML opt-record 文件路径
    aimv_json_path: str                    # AIMVFeedbackPass 输出的 JSON
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
    passed_remark_count: int               # passed remark 总数（用于退化检测）
```

### 3.3 Patch 记录

```python
# [AIMV] driver/models.py

@dataclass
class PatchRecord:
    """一次源码修改的完整记录"""
    source_file: str
    backup_path: str                       # 回滚用的备份文件
    diff_text: str
    original_hash: str                     # sha256 of original file
    applied_at: float = field(default_factory=time.time)
```

### 3.4 轮次记录

```python
# [AIMV] driver/models.py

@dataclass
class RoundRecord:
    """单轮迭代的完整记录"""
    round_number: int
    status: IterationStatus = IterationStatus.PENDING

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

    # AI 建议摘要（用于 history 注入下轮 prompt）
    suggestion_description: Optional[str] = None
    applied_diff_summary: Optional[str] = None

    # 时间戳
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None
```

### 3.5 PerFunctionResult（多函数顺序处理）

```python
# [AIMV] driver/models.py

@dataclass
class PerFunctionResult:
    """单函数迭代结果（多函数文件中每个函数独立记录）

    对应 PLAN §3.2 PerFunctionResult 数据模型。
    每个函数独立轮次计数，独立终止原因。
    函数 A 成功后立即 atomic mv 到原文件，
    后续函数 B 编译时看到已包含 A 变更的源码。
    """
    function_name: str
    rounds: List[RoundRecord] = field(default_factory=list)
    termination_reason: Optional[TerminationReason] = None
    vectorized: bool = False
    rounds_used: int = 0
    # 历史记录（用于 MCP 请求的 history 字段，最多保留最近 3 轮）
    history: List[Dict] = field(default_factory=list)
```

### 3.6 Session 记录

```python
# [AIMV] driver/models.py

@dataclass
class SessionRecord:
    """完整 AIMV 会话记录"""
    session_id: str = field(default_factory=lambda: f"aimv-{uuid.uuid4().hex[:12]}")
    source_file: str = ""
    aimv_level: str = "conservative"         # 默认 conservative（SPEC §3.1）
    max_rounds: int = 5
    functions: List[PerFunctionResult] = field(default_factory=list)

    # 原始源码备份（会话开始时一次性创建）
    pristine_backup_path: str = ""

    # 终止原因（最后一个函数的原因）
    termination_reason: Optional[TerminationReason] = None

    # 最终结果
    final_patch_path: Optional[str] = None   # <source>.aimv.patch
    total_elapsed_ms: Optional[float] = None

    # 元信息
    started_at: float = field(default_factory=time.time)
    finished_at: Optional[float] = None
    cli_command: str = ""

    # 注: SessionRecord 包含 PLAN §3.2 中未定义的扩展字段
    #     (pristine_backup_path, final_patch_path, total_elapsed_ms, cli_command)。
    #     这些字段为 Driver 内部使用，不参与 MCP 通信。
    #     PLAN 的 SessionRecord 是最小核心模型，Driver 实现可扩展。
```

---

## 4. 子进程管理（build_orchestrator.py）

### 4.1 编译执行

```python
# [AIMV] driver/build_orchestrator.py

import subprocess
import tempfile
import os
import time
from pathlib import Path
from typing import Optional

class BuildOrchestrator:
    """管理 clang 编译和测试子进程。

    关键约束：编译时使用 -mllvm -aimv-enable（不传 -faimv），
    防止递归 fork。详见 SPEC §3.1 防无限 fork 设计。
    """

    def __init__(self, config: dict):
        self.cc = config.get("cc", "clang")
        self.cflags = config.get("cflags", [])
        self.aimv_flags = config.get("aimv_flags", [])
        self.timeout_seconds = config.get("timeout", 120)
        self.work_dir = Path(config.get("work_dir", tempfile.mkdtemp(prefix="aimv-")))

    def compile_with_aimv(
        self,
        source_file: str,
        output_file: str,
        aimv_json_output: Optional[str] = None,
    ) -> BuildResult:
        """编译源码，启用 AIMVFeedbackPass。

        Flags（防 fork 设计）:
          -O2
          -g                                          (debug info → 源码映射)
          -mllvm -aimv-enable                         (LLVM 后端 flag，不含 -faimv!)
          -mllvm -aimv-output=<json>                  (AIMVFeedback Pass 输出)
        """

        aimv_path = aimv_json_output or str(self.work_dir / "aimv-diag.json")

        cmd = [self.cc]
        cmd.extend(self.cflags)
        cmd.extend([
            "-g",
            "-mllvm", "-aimv-enable",
            "-mllvm", f"-aimv-output={aimv_path}",
        ])
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
            opt_record_path="",              # Pass 模式不产生 YAML
            aimv_json_path=aimv_path,
            elapsed_ms=elapsed,
        )

    def run_tests(self, test_cmd: str) -> TestResult:
        """运行测试套件。test_cmd 为空则跳过（返回 pass）。"""
        if not test_cmd:
            return TestResult(
                returncode=0, stdout="", stderr="",
                passed=0, failed=0, elapsed_ms=0,
            )

        start = time.monotonic()
        proc = subprocess.run(
            test_cmd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=self.timeout_seconds * 2,
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

    def check_vectorization(self, aimv_json_path: str, function_name: str) -> VectorizationStatus:
        """解析 AIMV JSON，检查目标函数的向量化状态。

        需要 !aimv.diag 同时包含 missed 和 passed 诊断。
        passed_remark_count 用于收益退化检测（新 patch 后 count 减少则回滚）。
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

        return VectorizationStatus(
            function_name=function_name,
            total_loops=total,
            vectorized_loops=passed,
            missed_loops=missed,
            missed_details=details,
            passed_remark_count=passed,
        )

    @staticmethod
    def _parse_test_output(stdout: str, stderr: str) -> tuple:
        """解析测试输出中的 pass/fail 计数。"""
        import re
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

        # 回退到 returncode
        return (1, 1) if "error" in combined.lower() else (1, 0)
```

### 4.2 超时与信号处理

```python
# [AIMV] 子进程超时 → 抛出 subprocess.TimeoutExpired
# driver 捕获后:
#   编译超时 → 标记 COMPILE_ERROR，回滚
#   测试超时 → 标记 TEST_FAILURE，回滚
#
# 连续 2 轮超时 → 截断 max_rounds 到当前轮次
# （说明 AI 生成的代码有死循环或死锁）
```

---

## 5. 源码管理（source_manager.py）

### 5.1 影子文件 + 原子替换协议

完整时序遵循 SPEC §3.1。锁仅覆盖文件 I/O（秒级），MCP 查询（数十秒）不占锁。

```python
# [AIMV] driver/source_manager.py

import os
import hashlib
import shutil
import fcntl
import time
from pathlib import Path
from typing import Optional, List


class SourceManager:
    """源码的影子文件 patch、原子替换、回滚和 FileLock 管理。

    影子文件协议（SPEC §3.1 完整时序）:
      1. [无锁] MCP 查询 → 获得 AI 建议
      2. [获取锁]
         a. cp source → source.aimv-tmp（基于当前版本快照）
         b. diff 基于快照生成
         c. patch source.aimv-tmp（在影子上修改）
         d. clang -c source.aimv-tmp -mllvm -aimv-enable ...（编译验证）
            注意: 影子文件名 source.c.aimv-tmp 需在编译时正确处理:
              - -mllvm -aimv-output 指向独立的 JSON 路径（非原 aimv.json）
              - 编译器基于文件扩展名（.c）识别语言，aimv-tmp 后缀不影响
              - 若编译器对文件名敏感，可使用 -x c 显式指定语言
         e. 通过 → mv source.aimv-tmp source（rename(2) 原子替换）
            失败 → rm source.aimv-tmp
      3. [释放锁]

    锁作用域: 仅步骤 2a-2e（文件 I/O，秒级）。
    MCP 查询不占锁，不阻塞其他 clang 进程。

    多函数: 每函数独立应用原子替换。函数 A 成功后立即 mv，
    不等待 B。函数 B 编译时看到已包含 A 变更的源码。

    中止保护: kill -9 残留 .aimv-tmp 文件，下次启动时警告用户。
    """

    def __init__(self, output_dir: str):
        self.backup_dir = Path(output_dir) / "backups"
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        self.lock_dir = Path(output_dir) / "locks"
        self.lock_dir.mkdir(parents=True, exist_ok=True)
        self._patch_history: List[PatchRecord] = []
        self._locks: dict = {}            # path → fd

    # ── FileLock ──────────────────────────────────────────

    def acquire_lock(self, source_file: str, timeout_seconds: int = 30) -> bool:
        """获取 per-source-file 文件锁。

        锁粒度为单个源文件。不同文件可并行处理，同一文件串行化。
        """
        path = Path(source_file).resolve()
        lock_name = hashlib.sha256(str(path).encode()).hexdigest()[:16]
        lock_path = self.lock_dir / f"{lock_name}.lock"

        fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR)
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

    def release_lock(self, source_file: str):
        """释放文件锁。"""
        path = str(Path(source_file).resolve())
        fd = self._locks.pop(path, None)
        if fd is not None:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    # ── 影子文件协议 ─────────────────────────────────────

    def apply_shadow_patch(
        self,
        source_file: str,
        diff_text: str,
    ) -> PatchRecord:
        """影子文件 patch 协议（SPEC §3.1）。

        调用方必须在调用前 acquire_lock()。
        调用方负责在完成后 release_lock()。

        步骤:
          a. cp source → source.aimv-tmp
          b. 写 diff 临时文件
          c. patch source.aimv-tmp
          d. 返回 PatchRecord（调用方负责后续的编译验证和 atomic mv/rm）
        """
        src_path = Path(source_file).resolve()
        if not src_path.exists():
            raise FileNotFoundError(f"source file not found: {source_file}")

        original_hash = self._sha256(src_path)

        # Step a: cp source → source.aimv-tmp
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        shutil.copy2(src_path, shadow_path)

        # 创建备份（用于 rollback）
        backup_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.bak"
        shutil.copy2(src_path, backup_path)

        # Step b: 写 diff 临时文件
        diff_path = self.backup_dir / f"{src_path.stem}.r{len(self._patch_history)}.diff"
        diff_path.write_text(diff_text, encoding="utf-8")

        # Step c: patch source.aimv-tmp
        import subprocess
        proc = subprocess.run(
            ["patch", "-u", "--fuzz=2", str(shadow_path), str(diff_path)],
            capture_output=True, text=True,
        )
        diff_path.unlink(missing_ok=True)

        if proc.returncode != 0:
            # patch 失败 → rm shadow
            shadow_path.unlink(missing_ok=True)
            backup_path.unlink(missing_ok=True)
            raise RuntimeError(f"patch apply failed: {proc.stderr}")

        record = PatchRecord(
            source_file=str(src_path),
            backup_path=str(backup_path),
            diff_text=diff_text.rstrip(),
            original_hash=original_hash,
        )
        self._patch_history.append(record)
        return record

    def commit_shadow(self, source_file: str) -> bool:
        """影子文件验证通过 → atomic mv 替换原文件。

        rename(2) 在同一文件系统上是原子的。
        其他进程在读原文件不受影响。
        """
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"

        if not shadow_path.exists():
            return False

        os.replace(str(shadow_path), str(src_path))  # atomic rename(2)
        return True

    def discard_shadow(self, source_file: str):
        """影子文件验证失败 → rm shadow。"""
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        shadow_path.unlink(missing_ok=True)

    def rollback(self, patch: PatchRecord) -> bool:
        """从 backup 恢复原始文件。"""
        src_path = Path(patch.source_file)
        backup_path = Path(patch.backup_path)

        if not backup_path.exists():
            raise FileNotFoundError(f"backup not found: {backup_path}")

        backup_hash = self._sha256(backup_path)
        if backup_hash != patch.original_hash:
            raise RuntimeError(
                f"backup hash mismatch: expected {patch.original_hash}, got {backup_hash}"
            )

        shutil.copy2(backup_path, src_path)
        return True

    def rollback_all(self):
        """按反序回滚所有 patch。单条失败不中断后续。"""
        errors = []
        for patch in reversed(self._patch_history):
            try:
                self.rollback(patch)
            except Exception as e:
                errors.append((patch.source_file, str(e)))
        if errors:
            raise RuntimeError(f"rollback_all: {len(errors)} failures: {errors}")

    # ── 残留影子检测 ─────────────────────────────────────

    def check_stale_shadow(self, source_file: str) -> Optional[str]:
        """检测残留影子文件（kill -9 后）。

        返回残留影子路径，或 None。
        下次启动时检测到残留 → 输出警告让用户手动检查，不自动覆盖。
        """
        src_path = Path(source_file).resolve()
        shadow_path = src_path.parent / f"{src_path.name}.aimv-tmp"
        if shadow_path.exists():
            return str(shadow_path)
        return None

    def warn_stale_shadow(self, source_file: str):
        """启动时检测残留影子文件并输出警告。"""
        stale = self.check_stale_shadow(source_file)
        if stale:
            import sys
            print(
                f"[AIMV] WARNING: stale shadow file detected: {stale}\n"
                f"  This may be from a previous aimv-driver process killed by signal.\n"
                f"  Please inspect the file manually before proceeding.\n"
                f"  To discard: rm {stale}",
                file=sys.stderr,
            )

    # ── 累积 Patch 生成 ──────────────────────────────────

    def generate_cumulative_patch(self, source_file: str) -> Optional[str]:
        """生成累积 unified diff（相对于原始源码）。

        输出路径: <source>.aimv.patch
        仅包含最终成功的变更；失败/回滚的中间尝试不记录。
        """
        src_path = Path(source_file).resolve()
        pristine = self.backup_dir / "pristine" / src_path.name
        if not pristine.exists():
            return None

        import subprocess
        proc = subprocess.run(
            ["diff", "-u", str(pristine), str(src_path)],
            capture_output=True, text=True,
        )
        if proc.returncode == 1:  # differences found
            patch_path = src_path.parent / f"{src_path.name}.aimv.patch"
            patch_path.write_text(proc.stdout, encoding="utf-8")
            return str(patch_path)
        return None

    @staticmethod
    def _sha256(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()
```

### 5.2 影子文件时序图

```
Round N 完整时序（锁仅覆盖文件 I/O 阶段）:

  1. [无锁] MCP 查询 → 获得 AI 建议（网络 I/O，数十秒）
     │
     │  ← 其他进程可正常读/写同一文件（排队等锁）或不同文件（并行）
     │
  2. [获取锁]
     a. cp task.c → task.c.aimv-tmp        ← 基于当前版本创建快照
     b. diff 基于快照生成                   ← 确保 diff 上下文正确
     c. patch task.c.aimv-tmp              ← 在影子上修改
     d. clang -c task.c.aimv-tmp           ← 编译验证
        （其他进程读 task.c 不受影响）
     e. 通过 → mv task.c.aimv-tmp task.c   ← rename(2) 是原子的
        失败 → rm task.c.aimv-tmp          ← 丢弃影子
  3. [释放锁]

锁作用域: 仅步骤 2a-2e（文件 I/O，秒级）。
步骤 1 MCP 查询不占锁，不阻塞其他 clang 进程。

多函数原子替换时序:
  函数 A 验证通过 → mv 完成 → 释放锁
  → 处理函数 B（B 的 MCP 查询和编译基于已包含 A 变更的源码）
  → 函数 B 失败不回滚函数 A 的变更
```

---

## 6. MCP 客户端（mcp_client.py）

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

    def __init__(self, base_url: str, timeout_seconds: int = 60,
                 api_key: Optional[str] = None):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout_seconds
        # 构建 headers（含 API Key，对应 MCP_DESIGN §2.2 认证要求）
        headers = {}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        self._client = httpx.Client(timeout=timeout_seconds + 10, headers=headers)

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
                    if attempt < 2:
                        wait = 2.0 * (2 ** attempt)
                        time.sleep(wait)
                        continue
                    return None
                else:
                    return None

            except (httpx.TimeoutException, httpx.ConnectError):
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

## 7. 迭代决策引擎（iteration_engine.py）

### 7.1 终止条件（SPEC §2）

三个终止条件的组合策略：

1. **向量化成功** — 检测到 LoopVectorize pass 成功生成向量指令（passed remark 出现）
2. **轮次上限** — 达到 max_rounds（默认 5 轮，可配置）
3. **收益退化** — 新 patch 后 passed remark 数量减少 → 回滚

### 7.2 决策逻辑

```python
# [AIMV] driver/iteration_engine.py

from enum import Enum
from typing import Optional


class NextAction(Enum):
    CONTINUE = "continue"              # 继续下一轮
    RETRY_SAME = "retry_same"          # 编译错/超时，同轮重试一次
    ESCALATE_LEVEL = "escalate_level"  # 无建议，提升 aimv_level
    ROLLBACK = "rollback"              # 回滚最近的 patch
    STOP = "stop"                      # 终止


class IterationEngine:
    """根据当前状态决定下一步行动。

    每个函数独立引擎实例（独立轮次计数，独立 level 追踪）。

    决策矩阵:
    ┌──────────────────────┬───────────────────────────────────┐
    │ 状态                 │ 动作                              │
    ├──────────────────────┼───────────────────────────────────┤
    │ 向量化成功           │ STOP (VECTORIZED)                  │
    │ 达到 max_rounds      │ STOP (ROUND_LIMIT)                 │
    │ 编译失败             │ ROLLBACK → 同 round 重试一次       │
    │                      │ 再次失败 → STOP (COMPILE_ERROR)    │
    │ 测试失败             │ ROLLBACK → STOP (TEST_FAILURE)     │
    │ MCP 无响应           │ mcp_client 内部重试 3 次            │
    │                      │ 全部失败 → STOP                    │
    │ MCP 无建议           │ 升 level (conservative→moderate→   │
    │                      │         aggressive)                │
    │                      │ 已是 aggressive → STOP             │
    │ 收益退化             │ ROLLBACK → STOP (NO_IMPROVEMENT)   │
    │  (passed remark 减少) │                                    │
    └──────────────────────┴───────────────────────────────────┘
    """

    def __init__(self, initial_level: str = "conservative", max_rounds: int = 5):
        # 参数命名与 DriverConfig 一致: aimv_level → initial_level, max_rounds
        self.initial_level = initial_level
        self.current_level = initial_level
        self.max_rounds = max_rounds

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
        passed_remark_delta: Optional[int] = None,
    ) -> tuple:
        """返回 (NextAction, reason_string) 元组。

        passed_remark_delta: 新 patch 后 passed remark 数量变化。
          正值=增加, 负值=减少（退化）, None=无法比较。
        """

        # 1. 向量化成功
        if vectorized:
            return NextAction.STOP, "vectorization succeeded"

        # 2. 轮次上限
        if current_round >= self.max_rounds:
            return NextAction.STOP, f"reached max rounds ({self.max_rounds})"

        # 3. 编译失败
        if not build_result_ok:
            self._consecutive_compile_failures += 1
            if self._consecutive_compile_failures >= 2:
                return NextAction.STOP, "consecutive compile failures"
            return NextAction.ROLLBACK, "compile failure, will retry"

        self._consecutive_compile_failures = 0

        # 4. 测试失败 → 回滚并终止（语义错误，重试无意义）
        if not test_result_ok:
            return NextAction.ROLLBACK, "test failure, stopping"

        # 5. MCP 无响应
        if not mcp_responded:
            self._consecutive_mcp_failures += 1
            if self._consecutive_mcp_failures >= 1:
                return NextAction.STOP, "MCP server unresponsive"
            return NextAction.RETRY_SAME, "MCP timeout, one retry"

        self._consecutive_mcp_failures = 0

        # 6. MCP 无建议
        if not mcp_had_suggestions:
            escalated = self._try_escalate()
            if escalated:
                return NextAction.ESCALATE_LEVEL, f"escalated to {self.current_level}"
            return NextAction.STOP, "no suggestions at highest level"

        # 7. 收益退化（passed remark 数量减少）
        if passed_remark_delta is not None and passed_remark_delta < 0:
            return NextAction.ROLLBACK, (
                f"regression: passed remark count decreased by {-passed_remark_delta}"
            )

        # 8. 继续
        return NextAction.CONTINUE, "continuing to next round"

    def _try_escalate(self) -> bool:
        """提升 aimv_level: conservative → moderate → aggressive"""
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

### 7.3 迭代策略流程图

```
Round N 开始
    │
    ├── 编译 + AIMV Pass ──→ 全部向量化? ──→ SUCCESS
    │                                  │
    │                                 否
    │                                  │
    ├── POST MCP Server ──→ 超时/无响应? ──→ mcp_client 内部重试 (最多 3 次)
    │              │                                │
    │              │                         全部失败 → STOP
    │              │
    │              └── 200 OK ──→ no_action_possible?
    │                                      │
    │                                     是 → 升 aimv_level (C→M→A)
    │                                      │        │
    │                                      │   已是 A → STOP
    │                                      │
    │                                     否
    │                                      │
    ├── [获取锁] 影子文件 patch ──→ patch 失败? ──→ [rm shadow, 释放锁]
    │              │                  (同轮 1 次重试，再次失败 → STOP)
    │              │
    │              └── patch 成功
    │                      │
    ├── [锁内] 重编译验证 ──→ 编译失败? ──→ ROLLBACK → 重试 → 仍失败 → STOP
    │                      │
    │                      └── 编译成功 → 检查 passed remark
    │                              │
    │                             向量化成功 → [mv shadow→source, 释放锁] → SUCCESS
    │                             passed remark 减少 → [rm shadow, 释放锁] → ROLLBACK → STOP
    │                             仍失败:
    │                                │
    ├── [锁内] 运行测试 ──→ 失败? ──→ [rm shadow, 释放锁] → ROLLBACK → STOP
    │                      │
    │                      └── 通过
    │                              │
    ├── [mv shadow→source, 释放锁]
    │                              │
    └── Round N+1 ──→ (N < max_rounds) ──→ 回到顶部
                        │
                  N == max_rounds → STOP (round_limit)
```

---

## 8. 配置系统（config.py）

### 8.1 配置优先级链

```
~/.aimv/config (YAML)  >  环境变量  >  默认值

环境变量:
  AIMV_MCP_URL       MCP 服务地址
  AIMV_LEVEL         修改激进度 (conservative|moderate|aggressive)
  AIMV_MAX_ROUNDS    最大迭代轮次
  AIMV_TEST_CMD      测试命令

默认值（保守安全基线）:
  mcp_url     = http://localhost:8080
  aimv_level  = conservative           ← 不是 moderate
  max_rounds  = 5
  test_cmd    = ""                      ← 空 = 仅编译验证，跳过测试
```

### 8.2 配置加载实现

```python
# [AIMV] driver/config.py

import os
from pathlib import Path
from dataclasses import dataclass


@dataclass
class DriverConfig:
    mcp_url: str = "http://localhost:8080"
    mcp_timeout: int = 60
    mcp_api_key: str = ""            # MCP 服务 API 密钥（优先从 ~/.aimv/config 读取）
    aimv_level: str = "conservative"         # 默认 conservative
    max_rounds: int = 5
    test_cmd: str = ""                       # 空 = 跳过测试
    cc: str = "clang"
    cflags: list = None
    output_dir: str = "./aimv-output"
    aimv_mode: str = "auto"                  # auto | review

    def __post_init__(self):
        if self.cflags is None:
            self.cflags = ["-O2"]


def load_config() -> DriverConfig:
    """加载配置，按优先级: ~/.aimv/config > 环境变量 > 默认值。"""

    config = DriverConfig()

    # Layer 1: ~/.aimv/config (YAML)
    config_path = Path.home() / ".aimv" / "config"
    if config_path.exists():
        import yaml
        with open(config_path) as f:
            file_config = yaml.safe_load(f) or {}

        mcp_cfg = file_config.get("mcp", {})
        driver_cfg = file_config.get("driver", {})

        if "url" in mcp_cfg:
            config.mcp_url = mcp_cfg["url"]
        if "api_key" in mcp_cfg:
            config.mcp_api_key = mcp_cfg["api_key"]
        if "timeout_seconds" in mcp_cfg:
            config.mcp_timeout = mcp_cfg["timeout_seconds"]
        if "max_rounds" in driver_cfg:
            config.max_rounds = driver_cfg["max_rounds"]
        if "aimv_level" in driver_cfg:
            config.aimv_level = driver_cfg["aimv_level"]
        if "test_cmd" in driver_cfg:
            config.test_cmd = driver_cfg["test_cmd"]

    # Layer 2: 环境变量覆盖
    if env_url := os.environ.get("AIMV_MCP_URL"):
        config.mcp_url = env_url
    if env_api_key := os.environ.get("AIMV_MCP_API_KEY"):
        config.mcp_api_key = env_api_key
    if env_level := os.environ.get("AIMV_LEVEL"):
        config.aimv_level = env_level
    if env_rounds := os.environ.get("AIMV_MAX_ROUNDS"):
        config.max_rounds = int(env_rounds)
    if env_test := os.environ.get("AIMV_TEST_CMD"):
        config.test_cmd = env_test
    if env_mode := os.environ.get("AIMV_MODE"):
        config.aimv_mode = env_mode

    # 校验
    assert config.aimv_level in ("conservative", "moderate", "aggressive")
    assert config.max_rounds > 0

    return config
```

### 8.3 配置文件示例

```yaml
# ~/.aimv/config
mcp:
  url: http://aimv-server:8080
  api_key: ""                       # MCP API 密钥（生产环境应使用环境变量 AIMV_MCP_API_KEY）
  timeout_seconds: 60
driver:
  max_rounds: 5
  aimv_level: conservative       # conservative | moderate | aggressive
  test_cmd: ""                   # 空 = 仅编译验证
```

---

## 9. History 策略

### 9.1 历史记录注入

每轮 MCP 请求携带最近 3 轮的历史，让 AI 避免重复建议。不足 3 轮时发送全部。

```python
# [AIMV] driver/aimv_driver.py

def build_history(func_result: PerFunctionResult, max_entries: int = 3) -> list:
    """从 PerFunctionResult 构建历史记录（最近 3 轮）。

    对应 PLAN §3.1 HistoryRecord 字段:
      round:              轮次号
      diagnosis_summary:  该轮诊断摘要
      suggestion_applied: 应用的 AI 建议摘要
      outcome:            结果（compile_passed, vectorization_still_failed 等）
    """
    rounds = func_result.rounds[-max_entries:]
    history = []
    for r in rounds:
        if r.finished_at is None:
            continue
        history.append({
            "round": r.round_number,
            "diagnosis_summary": _summarize_diagnostics(r.diagnostics_json),
            "suggestion_applied": r.applied_diff_summary or "N/A",
            "outcome": _classify_outcome(r),
        })
    return history


def _summarize_diagnostics(diagnostics_json: Optional[dict]) -> str:
    """提取诊断摘要（1-2 句话）。"""
    if not diagnostics_json:
        return "N/A"
    diags = diagnostics_json.get("diagnostics", [])
    if not diags:
        return "N/A"
    first = diags[0]
    return f"{first.get('remark_id', 'unknown')}: {first.get('remark_text', '')[:100]}"


def _classify_outcome(round_rec: RoundRecord) -> str:
    """分类单轮结果。"""
    if round_rec.vectorization_status and round_rec.vectorization_status.missed_loops == 0:
        return "vectorized"
    if round_rec.verify_build and round_rec.verify_build.returncode != 0:
        return "compile_failed"
    if round_rec.test_result and round_rec.test_result.failed > 0:
        return "test_failed"
    return "compile_passed, vectorization_still_failed"
```

---

## 9.5 MCP 请求构建

```python
# [AIMV] driver/aimv_driver.py

def build_mcp_request(
    function_name: str,
    source_file: str,
    diagnostics: list,
    target: dict,
    history: list,
    aimv_level: str,
    config: DriverConfig,
) -> dict:
    """构造 MCP AnalyzeRequest (对应 MCP_DESIGN.md §2.1)。

    组装:
      - request_id: "aimv-<session_id>-<func_name_hash>"
      - target: 优先从 AIMVFeedbackPass JSON 获取，回退到 config
      - function: 从当前源文件提取 name/signature/source_code/loop_line
      - diagnostics: 从 AIMVFeedbackPass JSON 筛选当前函数的 missed 诊断
      - history: 最近 3 轮历史记录
      - aimv_level: 当前级别（可能已被 IterationEngine 升级）
    """
    import hashlib

    with open(source_file, "r", encoding="utf-8") as f:
        source_code = f.read()

    # 从诊断中提取 loop_line（取第一个 missed 诊断的 loop_location）
    loop_line = 1
    for d in diagnostics:
        if d.get("function_name") == function_name and d.get("severity") == "missed":
            loc = d.get("loop_location", "")
            # 解析 "file.c:42:5" 格式
            parts = loc.rsplit(":", 2)
            if len(parts) >= 2:
                try:
                    loop_line = int(parts[-2])
                except ValueError:
                    pass
            break

    return {
        "request_id": f"aimv-{hashlib.sha256(function_name.encode()).hexdigest()[:12]}",
        "target": target or {
            "triple": "",
            "cpu": "",
            "features": [],
            "vector_width": 128,
        },
        "function": {
            "name": function_name,
            "signature": "",  # 简化: MVP 不提取完整签名
            "source_code": source_code,
            "source_file": source_file,
            "loop_line": loop_line,
        },
        "diagnostics": [
            d for d in diagnostics
            if d.get("function_name") == function_name and d.get("severity") == "missed"
        ],
        "history": history,
        "aimv_level": aimv_level,
    }
```

---

## 10. Review 模式

### 10.1 AIMV_MODE=review

默认全自动。设置 `AIMV_MODE=review` 环境变量后，每轮 patch 前暂停等待用户确认。

```python
# [AIMV] driver/aimv_driver.py

def prompt_review(diff_text: str, description: str) -> str:
    """AIMV_MODE=review 时，每轮 patch 前等待用户确认。

    返回: 'y' (apply), 'n' (skip), 'r' (rollback all), 'q' (quit)
    """
    import sys
    print(f"\n[AIMV] Suggested change: {description}", file=sys.stderr)
    print(f"[AIMV] Diff:\n{diff_text}", file=sys.stderr)
    print("[AIMV] Apply this change? [y/N/r/q]: ", end="", file=sys.stderr, flush=True)

    response = input().strip().lower()
    return response if response in ("y", "r", "q") else "n"
```

默认跳过 review 是因为编译验证 + 测试套件两层自动保险已可拦截语义错误（SPEC §3.1）。

---

## 11. 单函数迭代主循环

```python
# [AIMV] driver/aimv_driver.py

def process_single_function(
    function_name: str,
    source_file: str,
    initial_diagnostics: list,
    config: DriverConfig,
    builder: BuildOrchestrator,
    mcp: MCPClient,
    engine: IterationEngine,
    sources: SourceManager,
    store: SessionStore,
    session: SessionRecord,
) -> PerFunctionResult:
    """单函数迭代主循环。

    独立轮次计数，独立终止原因。
    成功后变更立即通过 atomic mv 写入原文件。
    """

    func_result = PerFunctionResult(function_name=function_name)

    # 残留影子检测
    sources.warn_stale_shadow(source_file)

    # 首轮诊断用于构造 MCP 请求
    prev_passed_count = 0

    try:
        while True:
            round_num = len(func_result.rounds) + 1
            round_rec = RoundRecord(round_number=round_num)
            func_result.rounds.append(round_rec)

            # ── Step 1: 编译 + AIMV Pass ──
            round_rec.status = IterationStatus.COMPILING
            build = builder.compile_with_aimv(
                source_file=source_file,
                output_file=str(Path(config.output_dir) / f"{function_name}.o"),
                aimv_json_output=str(
                    Path(config.output_dir) / f"aimv-{function_name}-r{round_num}.json"
                ),
            )
            round_rec.build_result = build

            if build.returncode != 0:
                action, reason = engine.decide(
                    current_round=round_num,
                    build_result_ok=False, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=False,
                    mcp_responded=True,
                )
                if action == NextAction.STOP:
                    func_result.termination_reason = TerminationReason.COMPILE_ERROR
                    break
                continue

            # 检查向量化状态
            vstatus = builder.check_vectorization(
                build.aimv_json_path, function_name)
            round_rec.vectorization_status = vstatus

            # 终止判定: 向量化成功
            if vstatus.missed_loops == 0 and vstatus.total_loops > 0:
                func_result.termination_reason = TerminationReason.VECTORIZED
                func_result.vectorized = True
                func_result.rounds_used = round_num
                round_rec.status = IterationStatus.SUCCESS
                break

            # 加载诊断数据
            with open(build.aimv_json_path) as f:
                aimv_json = json.load(f)
            round_rec.diagnostics_json = aimv_json

            # 构造 MCP 请求（格式对应 MCP_DESIGN.md §2.1 AnalyzeRequest）
            # source_code 为当前文件的完整内容（已包含前面函数的变更）
            # build_mcp_request 组装: target (来自 aimv_json 或 config),
            #   function (从源文件提取 name/signature/source_code/loop_line),
            #   diagnostics (从 aimv_json), history (最近 3 轮), aimv_level
            request_body = build_mcp_request(
                function_name=function_name,
                source_file=source_file,
                diagnostics=aimv_json.get("diagnostics", []),
                target=aimv_json.get("target", {}),
                history=build_history(func_result),
                aimv_level=engine.current_level,
                config=config,
            )
            round_rec.mcp_request = request_body

            # ── Step 2: MCP 查询 [无锁] ──
            round_rec.status = IterationStatus.QUERYING
            mcp_resp = mcp.analyze(request_body)
            round_rec.mcp_response = mcp_resp
            round_rec.suggestion_description = (
                mcp_resp.get("suggestions", [{}])[0].get("description")
                if mcp_resp else None
            )

            mcp_responded = mcp_resp is not None
            mcp_had_suggestions = bool(
                mcp_resp and mcp_resp.get("suggestions")
                and not mcp_resp.get("no_action_possible")
            )

            if not mcp_responded or not mcp_had_suggestions:
                action, reason = engine.decide(
                    current_round=round_num,
                    build_result_ok=True, test_result_ok=True,
                    vectorized=False, mcp_had_suggestions=mcp_had_suggestions,
                    mcp_responded=mcp_responded,
                )
                if action == NextAction.ESCALATE_LEVEL:
                    continue
                if action == NextAction.STOP:
                    func_result.termination_reason = TerminationReason.NO_SUGGESTION
                    break
                continue

            # Review 模式检查
            suggestion = mcp_resp["suggestions"][0]
            diff_text = suggestion["diff"]
            round_rec.applied_diff_summary = suggestion.get("description", "")

            if config.aimv_mode == "review":
                response = prompt_review(diff_text, suggestion.get("description", ""))
                if response == "n":
                    continue
                elif response == "r":
                    sources.rollback_all()
                    func_result.termination_reason = TerminationReason.INTERRUPTED
                    break
                elif response == "q":
                    func_result.termination_reason = TerminationReason.INTERRUPTED
                    break

            # 循环检测: 防止 LLM 重复建议同一修改
            if any(p.diff_text.strip() == diff_text.strip()
                   for p in sources._patch_history):
                func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                break

            # ── Step 3: 影子文件 patch [获取锁] ──
            round_rec.status = IterationStatus.PATCHING
            if not sources.acquire_lock(source_file):
                func_result.termination_reason = TerminationReason.COMPILE_ERROR
                break

            try:
                patch = sources.apply_shadow_patch(source_file, diff_text)
                round_rec.patch = patch

                # ── Step 4: 重编译验证 [同一锁区间] ──
                round_rec.status = IterationStatus.VERIFYING
                shadow_file = source_file + ".aimv-tmp"
                verify_json = str(
                    Path(config.output_dir) / f"aimv-{function_name}-verify-r{round_num}.json"
                )
                verify_build = builder.compile_with_aimv(
                    source_file=shadow_file,
                    output_file=str(
                        Path(config.output_dir) / f"{function_name}-verify.o"
                    ),
                    aimv_json_output=verify_json,  # 独立 JSON 路径，避免覆盖首轮 aimv.json
                )
                round_rec.verify_build = verify_build

                if verify_build.returncode != 0:
                    # 编译失败 → 丢弃影子
                    sources.discard_shadow(source_file)
                    action, reason = engine.decide(
                        current_round=round_num,
                        build_result_ok=False, test_result_ok=True,
                        vectorized=False, mcp_had_suggestions=True,
                        mcp_responded=True,
                    )
                    if action == NextAction.STOP:
                        func_result.termination_reason = TerminationReason.COMPILE_ERROR
                        break
                    continue

                # 检查验证编译后的向量化状态
                verify_vstatus = builder.check_vectorization(
                    verify_build.aimv_json_path, function_name)

                # 收益退化检测: passed remark 数量减少 → 回滚
                # 注意: 此检测在 engine.decide() 之外直接处理，
                # 因为需要传入 prev_passed_count 做精确比较。
                # engine.decide() 的 regression 分支 (passed_remark_delta < 0)
                # 在此处不可达——regression 在编译验证阶段就地检测并处理。
                if (prev_passed_count > 0 and
                        verify_vstatus.passed_remark_count < prev_passed_count):
                    sources.discard_shadow(source_file)
                    action, reason = engine.decide(
                        current_round=round_num,
                        build_result_ok=True, test_result_ok=True,
                        vectorized=False, mcp_had_suggestions=True,
                        mcp_responded=True,
                        passed_remark_delta=(
                            verify_vstatus.passed_remark_count - prev_passed_count),
                    )
                    func_result.termination_reason = TerminationReason.NO_IMPROVEMENT
                    break

                # 测试
                test = builder.run_tests(config.test_cmd)
                round_rec.test_result = test

                if test.returncode != 0 or test.failed > 0:
                    sources.discard_shadow(source_file)
                    func_result.termination_reason = TerminationReason.TEST_FAILURE
                    break

                # 全部通过 → atomic mv 替换原文件
                sources.commit_shadow(source_file)
                prev_passed_count = verify_vstatus.passed_remark_count

            finally:
                sources.release_lock(source_file)

            # 持久化（锁外，不阻塞文件 I/O）
            # 注意: session 对象在多函数间共享，此处保存的是整个 session 的当前状态。
            # 对于多函数场景，每个函数完成后都会保存一次（N 函数最多 N 次 save）。
            # 如果 session JSON 较大，可考虑每函数完成后再 save，而非每轮 save。
            store.save(session)

            # 轮次判定
            action, reason = engine.decide(
                current_round=round_num,
                build_result_ok=True, test_result_ok=True,
                vectorized=False, mcp_had_suggestions=True,
                mcp_responded=True,
            )
            if action == NextAction.STOP:
                func_result.termination_reason = TerminationReason.ROUND_LIMIT
                func_result.rounds_used = round_num
                break

    except KeyboardInterrupt:
        func_result.termination_reason = TerminationReason.INTERRUPTED
        sources.rollback_all()

    finally:
        func_result.rounds_used = len(func_result.rounds)
        # 标记最后一轮完成时间（若存在）
        if func_result.rounds:
            func_result.rounds[-1].finished_at = time.time()

    return func_result
```

---

## 12. Session 持久化（session_store.py）

### 12.1 格式

```python
# [AIMV] driver/session_store.py

import json
import os
from pathlib import Path
from dataclasses import asdict
from typing import Optional


class SessionStore:
    """Session JSON 持久化。

    文件路径: <output_dir>/sessions/<session_id>.json

    写入策略:
      - 每轮迭代结束后自动写入 (原子写: 写 tmp + rename)
      - 不依赖外部数据库

    追溯产物（SPEC §3.1）:
    | 产物         | 路径                               | 生成时机         |
    |-------------|-------------------------------------|-----------------|
    | 累积 patch  | <source>.aimv.patch                 | 每轮成功后更新   |
    | Session 记录 | sessions/<session_id>.json          | 每轮更新         |
    | 备份        | backups/<name>.r<N>.bak             | 每次 patch 前    |
    """

    def __init__(self, output_dir: str):
        self.sessions_dir = Path(output_dir) / "sessions"
        self.sessions_dir.mkdir(parents=True, exist_ok=True)

    def save(self, session):
        """原子写入 session JSON。"""
        data = self._serialize(session)
        path = self.sessions_dir / f"{session.session_id}.json"
        tmp_path = path.with_suffix(".tmp")

        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False, default=str)

        os.replace(tmp_path, path)  # 原子 rename

    def load(self, session_id: str) -> Optional[dict]:
        """从磁盘恢复 session。"""
        path = self.sessions_dir / f"{session_id}.json"
        if not path.exists():
            return None
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def list_sessions(self) -> list:
        """列出所有 session 摘要。"""
        sessions = []
        for path in self.sessions_dir.glob("*.json"):
            with open(path) as f:
                data = json.load(f)
            sessions.append({
                "session_id": data.get("session_id", ""),
                "source_file": data.get("source_file", ""),
                "status": data.get("termination_reason", "in_progress"),
                "functions_count": len(data.get("functions", [])),
                "started_at": data.get("started_at"),
            })
        return sorted(sessions, key=lambda s: s.get("started_at") or "", reverse=True)

    @staticmethod
    def _serialize(obj) -> dict:
        """递归 dataclass → JSON 兼容 dict。"""
        if hasattr(obj, "__dataclass_fields__"):
            result = {}
            for field_name in obj.__dataclass_fields__:
                value = getattr(obj, field_name)
                result[field_name] = SessionStore._serialize(value)
            return result
        elif isinstance(obj, list):
            return [SessionStore._serialize(item) for item in obj]
        elif isinstance(obj, dict):
            return {k: SessionStore._serialize(v) for k, v in obj.items()}
        elif hasattr(obj, "value"):  # Enum
            return obj.value
        else:
            return obj
```

---

## 13. stderr 输出格式

输出到 stderr，不干扰编译 stdout。格式严格遵循 SPEC §3.1。

```python
# [AIMV] driver/aimv_driver.py

import sys


def emit_summary(
    results: list,              # List[PerFunctionResult]
    source_file: str,
    session: SessionRecord,
    store: SessionStore,
    config: DriverConfig,
):
    """输出最终汇总到 stderr（SPEC §3.1 格式）。"""

    total = len(results)
    optimized = sum(1 for r in results if r.vectorized)
    skipped = total - optimized  # includes gave_up + round_limit

    if len(results) == 1:
        r = results[0]
        if r.vectorized:
            # 单函数成功
            print(
                f"[AIMV] {r.function_name}: vectorized "
                f"({r.rounds_used} rounds, {session.aimv_level})",
                file=sys.stderr,
            )
            for rr in r.rounds:
                if rr.applied_diff_summary:
                    print(
                        f"[AIMV]   Round {rr.round_number}: {rr.applied_diff_summary}",
                        file=sys.stderr,
                    )
        else:
            # 单函数放弃
            reason_str = _format_termination(r.termination_reason, r.rounds_used)
            print(
                f"[AIMV] {r.function_name}: {reason_str}",
                file=sys.stderr,
            )
            if r.termination_reason in (
                TerminationReason.ROUND_LIMIT,
                TerminationReason.NO_SUGGESTION,
                TerminationReason.NO_IMPROVEMENT,
            ):
                print(
                    "[AIMV]   Source rolled back to original",
                    file=sys.stderr,
                )
    else:
        # 多函数混合结果
        print(
            f"[AIMV] {Path(source_file).name}: "
            f"{total} functions analyzed, "
            f"{optimized} optimized, "
            f"{total - optimized} gave up",
            file=sys.stderr,
        )
        for r in results:
            if r.vectorized:
                print(
                    f"[AIMV]   {r.function_name}: vectorized "
                    f"({r.rounds_used} rounds, {session.aimv_level})",
                    file=sys.stderr,
                )
            elif r.termination_reason == TerminationReason.VECTORIZED:
                print(
                    f"[AIMV]   {r.function_name}: already vectorized (skipped)",
                    file=sys.stderr,
                )
            else:
                reason_str = _format_termination(r.termination_reason, r.rounds_used)
                print(
                    f"[AIMV]   {r.function_name}: {reason_str}",
                    file=sys.stderr,
                )

    # 追溯产物路径
    patch_path = Path(source_file).resolve()
    patch_file = patch_path.parent / f"{patch_path.name}.aimv.patch"
    if patch_file.exists():
        print(f"[AIMV]   Patch: {patch_file}", file=sys.stderr)

    session_path = store.sessions_dir / f"{session.session_id}.json"
    if session_path.exists():
        print(f"[AIMV]   Report: {session_path}", file=sys.stderr)


def _format_termination(reason: TerminationReason, rounds_used: int) -> str:
    """格式化终止原因。"""
    if reason == TerminationReason.ROUND_LIMIT:
        return f"unable to vectorize ({rounds_used} rounds exhausted)"
    elif reason == TerminationReason.NO_SUGGESTION:
        return "unable to vectorize (no suggestions from AI)"
    elif reason == TerminationReason.NO_IMPROVEMENT:
        return "unable to vectorize (regression detected)"
    elif reason == TerminationReason.COMPILE_ERROR:
        return "unable to vectorize (compile error)"
    elif reason == TerminationReason.TEST_FAILURE:
        return "unable to vectorize (test failure)"
    elif reason == TerminationReason.INTERRUPTED:
        return "interrupted by user"
    else:
        return f"unable to vectorize ({reason.value})"
```

**输出示例**（SPEC §3.1 精确格式）:

```
# 单函数成功：
[AIMV] process_task: vectorized (2 rounds, conservative)
[AIMV]   Round 1: added restrict to parameter 'a' (line 1)
[AIMV]   Patch: /home/user/task.c.aimv.patch
[AIMV]   Report: /home/user/aimv-output/session_20260515_143022.json

# 多函数混合结果：
[AIMV] task.c: 3 functions analyzed, 2 optimized, 1 skipped
[AIMV]   process_task: vectorized (2 rounds, conservative)
[AIMV]   filter_data:  vectorized (1 round, moderate)
[AIMV]   init_buf:     already vectorized (skipped)
[AIMV]   Patch: /home/user/task.c.aimv.patch
[AIMV]   Report: /home/user/aimv-output/session_20260515_143022.json

# 放弃场景：
[AIMV] process_task: unable to vectorize (3 rounds exhausted)
[AIMV]   Source rolled back to original
[AIMV]   Report: /home/user/aimv-output/session_20260515_143022.json
```

---

## 14. CLI 接口

### 14.1 主入口：`--from-json`（clang Driver 调用）

```
aimv-driver --from-json=<aimv.json> --source=<source_file>

  --from-json=PATH     从 AIMV JSON 启动分析（clang Driver fork+exec 入口）
  --source=FILE        源文件路径
```

### 14.2 独立模式（手动/CI）

```
aimv-driver [OPTIONS] <source_file>

OPTIONS:
  --function FUNC          目标函数名（默认: 所有失败函数）
  --aimv-level LEVEL       修改激进度 (conservative|moderate|aggressive, 默认 conservative)
  --max-rounds N           最大迭代轮次（默认 5）
  --mcp-url URL            MCP 服务地址（默认 http://localhost:8080）
  --output-dir DIR         输出目录（默认 ./aimv-output）
  --test-cmd CMD           测试命令（默认 ""，仅编译验证）
  --dry-run                仅编译+诊断，不调用 MCP 不修改源码
  --require-review         每轮 patch 前等待用户确认
  --resume SESSION_ID      从 session 恢复
  --list-sessions          列出所有 session
  --verbose                详细日志

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

## 15. 错误处理矩阵

| 场景 | 检测方式 | 处理 |
|------|---------|------|
| 源文件不存在 | 启动时检查 | 立即退出，exit code 2 |
| clang 未安装 | 编译失败 stderr 含 "command not found" | 立即退出，exit code 3 |
| MCP 服务不可达 | 健康检查 + POST 超时 | 3 次重试，仍失败则退出，exit code 4 |
| MCP 返回非 JSON | JSONDecodeError | 记录原始响应到日志，重试 1 次 |
| MCP 返回空建议 | no_action_possible=true | 升 aimv_level 或放弃 |
| MCP 建议 diff 格式错误 | patch 命令失败 | 回滚影子文件，重试（同 round 1 次） |
| 编译失败（patch 引入语法错） | returncode != 0 | 丢弃影子文件，重试（同 round 1 次），再次失败则停止 |
| 测试失败 | returncode != 0 或 failed > 0 | 丢弃影子文件，停止 |
| 收益退化（passed remark 减少） | 比较前后 passed_remark_count | 丢弃影子文件，回滚，停止 |
| 磁盘满 | OSError (ENOSPC) | 停止，保留已完成 session，exit code 5 |
| 用户中断 (Ctrl+C) | KeyboardInterrupt | 回滚所有 patch，写入 session，exit code 130 |
| 进程被 kill (SIGTERM) | signal handler | 回滚所有 patch，写入 session，exit code 143 |
| 残留影子文件 | 启动时 check_stale_shadow() | 输出警告到 stderr，不自动覆盖 |

---

## 16. 依赖

```
aimv/driver/requirements.txt

httpx>=0.27.0
pyyaml>=6.0
jinja2>=3.1                   # 仅用于可选的自定义 prompt 模板
```

零额外依赖 -- 核心逻辑只用 stdlib（subprocess, json, hashlib, fcntl, pathlib, dataclasses, time）。httpx 是唯一的第三方依赖（HTTP 客户端），可选替换为 urllib。

---

*文档版本: 2.0*
*创建日期: 2026-04-29*
*最后更新: 2026-05-17*
