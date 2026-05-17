# AIMV — CI 集成方案

**版本**: 2.0
**日期**: 2026-05-17
**关联文档**: SPEC.md v1.4, PLAN.md v1.1, DRIVER_DESIGN.md, MCP_DESIGN.md

---

## 0. 设计目标

将 AIMV 嵌入 CI/CD 流水线，实现代码合入前的**自动向量化检查与建议**。开发者在提交 MR/PR 时，CI 自动对变更函数运行 AIMV 分析，输出向量化报告并作为 MR 评论或 CI 门禁。

核心原则：
- **非阻塞** — 默认不阻止合入，只做信息性报告
- **增量分析** — 只分析 git diff 中变更的函数，不跑全量
- **可缓存** — MCP 结果 + 编译产物缓存，避免重复计算
- **可配置** — 不同仓库/目录可配置不同的 AIMV 等级和门禁策略
- **配置优先级** — 环境变量 > 配置文件 > 默认值（环境变量覆盖配置文件）

---

## 1. CI 流水线架构

```
┌─────────────────────────────────────────────────────────────────┐
│  MR/PR 事件 (push / pull_request)                               │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 1: 变更检测                                               │
│  · git diff origin/main...HEAD → 变更的 .c/.cpp 文件列表         │
│  · 提取每个文件中新增/修改的函数名（用 clang -ast-dump）            │
│  · 过滤：只保留包含循环的函数（可选预分析）                          │
│  · 输出：[(file, function_name), ...]                           │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 2: AIMV 分析 (可并行)                                     │
│  · 对每个变更文件运行 clang -O2 -faimv -c <file>                  │
│    (clang Driver 自动调用 aimv-driver --from-json)                │
│  · 同一源文件内的函数由 aimv-driver 顺序处理（每函数独立轮次）       │
│  · 不同源文件的分析可并行执行                                      │
│  · 输出：per-function session JSON (PerFunctionResult)           │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 3: 报告生成 (aimv-report)                                 │
│  · aimv-report 读取所有 session JSON 文件                         │
│  · 汇总所有函数的 PerFunctionResult + TerminationReason          │
│  · 生成 Markdown 报告（摘要 + 详情）                               │
│  · 支持输出 GitLab API 兼容的 JSON 格式                           │
│  · 可选：自动提交 AI 建议的 patch 为新 commit                      │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 4: 反馈                                                   │
│  · MR/PR 评论 (GitHub/GitLab API)                                │
│  · CI Artifact (完整 JSON 报告)                                  │
│  · 可选：Slack/钉钉 通知                                         │
│  · 可选：门禁决策 (阻止合入 / 警告 / 放行)                         │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 增量变更检测

### 2.1 识别变更函数

```python
# [AIMV] ci/change_detector.py

"""
变更检测策略：

1. git diff 获取变更文件列表
2. 对每个 .c/.cpp 文件：
   a. 用 clang -ast-dump 解析 AST
   b. 对比 diff 的行号范围，找到被修改的函数定义
   c. 输出 (file, function, start_line, end_line) 元组
3. 过滤：排除不含循环的函数（用 LLVM opt -passes=print<loops> 预检）

注意: Phase 1 (MVP) 仅覆盖依赖分析失败场景（对应 benchmark 表中
dep_fail_alias.c 和 dep_fail_stride.c）。Phase 2 扩展至代价模型和
对齐失败场景。
"""

import subprocess
import json
from pathlib import Path
from typing import List, Tuple


def get_changed_functions(
    base_branch: str = "origin/main",
    target_branch: str = "HEAD",
) -> List[Tuple[str, str, int, int]]:
    """返回变更的函数列表: [(file, function_name, start_line, end_line), ...]"""

    # 1. 获取变更文件和行号范围
    diff_output = subprocess.run(
        ["git", "diff", "--name-only", f"{base_branch}...{target_branch}"],
        capture_output=True, text=True,
    ).stdout

    changed_files = [
        f for f in diff_output.strip().split("\n")
        if f.endswith((".c", ".cpp", ".cxx", ".cc"))
    ]

    if not changed_files:
        return []

    # 2. 对每个文件获取受影响函数
    functions = []
    for file in changed_files:
        if not Path(file).exists():
            continue

        # 获取该文件中变更的行号范围
        hunk_ranges = _get_changed_line_ranges(file, base_branch, target_branch)

        # AST dump 找函数定义
        funcs_in_file = _get_function_definitions(file)
        for func_name, func_start, func_end in funcs_in_file:
            for hunk_start, hunk_end in hunk_ranges:
                if _ranges_overlap(func_start, func_end, hunk_start, hunk_end):
                    functions.append((file, func_name, func_start, func_end))
                    break  # 每个函数只加一次

    return functions


def _get_function_definitions(file: str) -> List[Tuple[str, int, int]]:
    """用 clang -ast-dump 获取文件中所有函数定义的位置。"""
    cmd = [
        "clang", "-Xclang", "-ast-dump", "-fsyntax-only",
        file, "--", "-I.", "-I..",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

    # 解析 AST dump 输出: FunctionDecl <line:col, line:col> func_name 'type'
    functions = []
    for line in proc.stderr.split("\n"):  # AST dump 输出到 stderr
        if "FunctionDecl" in line and " definition " in line:
            name = line.split()[-1].strip("'")
            # 提取行号范围
            import re
            m = re.search(r"<line:(\d+):\d+, line:(\d+):\d+>", line)
            if m:
                functions.append((name, int(m.group(1)), int(m.group(2))))
    return functions


def _get_changed_line_ranges(
    file: str, base: str, target: str
) -> List[Tuple[int, int]]:
    """获取文件在 diff 中变更的行号范围。"""
    proc = subprocess.run(
        ["git", "diff", f"{base}...{target}", "--", file],
        capture_output=True, text=True,
    )
    ranges = []
    for line in proc.stdout.split("\n"):
        if line.startswith("@@"):
            # 解析 @@ -old_start,old_count +new_start,new_count @@
            import re
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if m:
                start = int(m.group(1))
                count = int(m.group(2)) if m.group(2) else 1
                ranges.append((start, start + count - 1))
    return ranges


def _ranges_overlap(a1, a2, b1, b2) -> bool:
    return max(a1, b1) <= min(a2, b2)
```

### 2.2 预过滤：排除不含循环的函数

```python
# [AIMV] ci/change_detector.py (续)

def filter_loopy_functions(functions: List[Tuple], cc: str = "clang") -> List[Tuple]:
    """用 opt -passes='print<loops>' 预检，只保留含循环的函数。

    对每个变更文件:
      1. clang -S -emit-llvm → file.ll
      2. opt -passes='print<loops>' -disable-output file.ll 2>&1
      3. 检查输出是否包含目标函数名
      4. 不含循环的函数 → 跳过
    """
    import tempfile, os

    loopy = []
    files_processed = {}  # file → set of (func_name, start, end) tuples that have loops

    for file, func_name, start, end in functions:
        if file not in files_processed:
            # 首次遇到此文件: 编译 + 分析循环
            files_processed[file] = set()

            with tempfile.NamedTemporaryFile(suffix=".ll", delete=False) as tmp:
                ir_path = tmp.name

            try:
                # 编译为 LLVM IR
                subprocess.run(
                    [cc, "-S", "-emit-llvm", "-O0", file, "-o", ir_path],
                    capture_output=True, text=True, timeout=30,
                )

                # 获取该文件中的所有循环
                proc = subprocess.run(
                    ["opt", "-passes=print<loops>", "-disable-output", ir_path],
                    capture_output=True, text=True, timeout=30,
                )
                # 输出中包含 "Loop at depth N containing: ..." 和函数名
                # 收集此文件中所有含有循环的函数名
                for f, fn, fs, fe in functions:
                    if f == file and fn in proc.stderr:
                        files_processed[file].add(fn)
            finally:
                os.unlink(ir_path)

        # 检查此函数是否含有循环
        if func_name in files_processed.get(file, set()):
            loopy.append((file, func_name, start, end))

    return loopy
```

---

## 3. CI Runner 实现

### 3.1 GitHub Actions

```yaml
# .github/workflows/aimv-analysis.yml

name: AIMV Vectorization Analysis

on:
  pull_request:
    types: [opened, synchronize, reopened]
    paths:
      - '**/*.c'
      - '**/*.cpp'
      - '**/*.h'

jobs:
  aimv-analyze:
    runs-on: [self-hosted, arm64]   # 需要实际目标架构的 runner
    timeout-minutes: 120

    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0              # 需要完整 git history 做 diff

      - name: Setup AIMV
        run: |
          pip install aimv-driver
          # 或从源码安装: pip install -e ./aimv/driver

          # 验证 aimv-driver 可用（clang -faimv 会 fork+exec 此工具）
          which aimv-driver || (echo "ERROR: aimv-driver not found in PATH" && exit 1)

      - name: Build LLVM+Clang (with AIMV Pass)
        run: |
          mkdir -p build && cd build
          cmake .. -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DLLVM_ENABLE_PROJECTS="clang" \
            -DLLVM_TARGETS_TO_BUILD="ARM;AArch64"
          ninja clang opt
        # 注: 完整 LLVM 构建耗时较长（30-60 分钟）。
        # 生产 CI 应使用预构建的 Docker 镜像（含已编译的 clang+opt+aimv-driver），
        # 或将此步骤移到独立的构建流水线中，通过 artifact/cache 传递编译产物。

      - name: Build baseline (AOT)
        run: |
          # 使用与 Step 2 相同的构建配置，不重新 cmake（避免覆盖 Ninja 配置）
          # 直接使用 ninja 重新构建 baseline 目标
          cd build
          ninja clang opt

      - name: Detect changed functions
        id: changes
        run: |
          aimv-detect-changes \
            --base origin/${{ github.base_ref }} \
            --target HEAD \
            --output changes.json
          cat changes.json

      - name: Run AIMV analysis
        id: aimv
        env:
          # [AIMV] CI secrets map to config priority chain (SPEC §3.1):
          #   环境变量覆盖默认值，但被 ~/.aimv/config 文件覆盖
          AIMV_MCP_URL: ${{ secrets.AIMV_MCP_URL }}
          AIMV_MCP_API_KEY: ${{ secrets.AIMV_MCP_API_KEY }}
          AIMV_LEVEL: conservative
        run: |
          aimv-run-batch \
            --input changes.json \
            --parallel 4 \
            --output-dir ./aimv-results

      - name: Generate report
        if: always()
        run: |
          aimv-report \
            --input ./aimv-results \
            --format markdown \
            --output aimv-report.md

      - name: Post PR comment
        if: always()
        uses: actions/github-script@v7
        with:
          script: |
            const fs = require('fs');
            const report = fs.readFileSync('aimv-report.md', 'utf8');

            // 更新已有评论或创建新评论
            const { data: comments } = await github.rest.issues.listComments({
              owner: context.repo.owner,
              repo: context.repo.repo,
              issue_number: context.issue.number,
            });

            const botComment = comments.find(c =>
              c.user.type === 'Bot' && c.body.includes('AIMV Vectorization Analysis')
            );

            if (botComment) {
              await github.rest.issues.updateComment({
                owner: context.repo.owner,
                repo: context.repo.repo,
                comment_id: botComment.id,
                body: report,
              });
            } else {
              await github.rest.issues.createComment({
                owner: context.repo.owner,
                repo: context.repo.repo,
                issue_number: context.issue.number,
                body: report,
              });
            }

      - name: Upload artifacts
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: aimv-results
          path: ./aimv-results/
```

### 3.2 GitLab CI

```yaml
# .gitlab-ci.yml

aimv:vectorization-analysis:
  image: registry.example.com/aimv/runner:latest
  tags:
    - arm64
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      changes:
        - "**/*.c"
        - "**/*.cpp"
  timeout: 2h
  variables:
    # [AIMV] 配置优先级: 环境变量 > ~/.aimv/config > 默认值
    # CI 中通过环境变量注入 secrets，环境变量优先于配置文件
    AIMV_MCP_URL: "${AIMV_MCP_URL}"
    AIMV_MCP_API_KEY: "${AIMV_MCP_API_KEY}"
    AIMV_LEVEL: "conservative"

  script:
    # 检出
    - git fetch origin $CI_MERGE_REQUEST_TARGET_BRANCH_NAME

    # 编译基线
    - mkdir build && cd build
    - cmake .. -DCMAKE_BUILD_TYPE=Release
    - make -j$(nproc)

    # 增量分析
    - aimv-detect-changes
        --base origin/$CI_MERGE_REQUEST_TARGET_BRANCH_NAME
        --target HEAD
        --output changes.json

    - aimv-run-batch
        --input changes.json
        --output-dir ./aimv-results

    # 生成报告
    - aimv-report
        --input ./aimv-results
        --format gitlab-api
        --output aimv-report.md

  after_script:
    # 通过 GitLab API 创建 MR note
    - |
      curl --request POST \
        --header "PRIVATE-TOKEN: ${GITLAB_API_TOKEN}" \
        --header "Content-Type: application/json" \
        --data "$(aimv-report --input ./aimv-results --format gitlab-api --output -)" \
        "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/notes"

  artifacts:
    when: always
    paths:
      - aimv-results/
    expire_in: 30 days
```

---

## 4. 批量分析工具（aimv-run-batch）

```python
# [AIMV] ci/aimv_run_batch.py

"""
aimv-run-batch — 批量 AIMV 分析

对变更检测输出的 (file, function) 列表，使用 clang -faimv 驱动完整分析管线。

核心约束（对应 PLAN §2.2 多函数处理策略）:
  - 同一源文件内的函数由 aimv-driver 按顺序处理（每函数独立轮次）
  - 不同源文件的函数可并行分析
  - 全局最大并行度由 --parallel 控制

配置优先级（对应 SPEC §3.1）:
  1. ~/.aimv/config (YAML)
  2. 环境变量: AIMV_MCP_URL, AIMV_LEVEL, AIMV_MAX_ROUNDS, AIMV_TEST_CMD
  3. 默认值: aimv_level=conservative, max_rounds=5, mcp_url=localhost:8080

注意: aimv-run-batch 不直接传递 --mcp-url / --aimv-level 等 flag 给 aimv-driver。
而是通过环境变量注入，让 aimv-driver --from-json 内部按优先级链加载配置。
"""

import asyncio
import json
import os
from pathlib import Path
from collections import defaultdict
from typing import List, Tuple


async def run_batch(
    changes: List[Tuple[str, str, int, int]],
    max_parallel: int = 4,
    output_dir: str = "./aimv-results",
) -> dict:
    """并行批量运行 AIMV。返回汇总字典。"""

    # 按文件分组：同文件函数由 driver 串行处理（per-function sequential model）
    by_file: dict[str, list] = defaultdict(list)
    for file, func, start, end in changes:
        by_file[file].append((func, start, end))

    # 跨文件并行，单文件内 driver 自行串行处理
    semaphore = asyncio.Semaphore(max_parallel)

    async def analyze_file(file: str, funcs: list) -> list:
        async with semaphore:
            result = await _run_file(file, funcs, output_dir)
            return result

    # 启动所有文件的分析任务
    tasks = []
    for file, funcs in by_file.items():
        tasks.append(analyze_file(file, funcs))

    all_results = await asyncio.gather(*tasks)

    # 汇总
    flat_results = [r for file_results in all_results for r in file_results]
    return _summarize(flat_results)


async def _run_file(
    source_file: str,
    funcs: list,
    output_dir: str,
) -> list:
    """对单个源文件运行 clang -faimv。

    clang Driver 会 fork+exec aimv-driver --from-json，
    aimv-driver 内部对文件中所有失败函数顺序处理。
    因此我们只需调用一次 clang -faimv，不需要逐函数调用。

    重编译时使用 -mllvm -aimv-enable（不含 -faimv），防止 fork 链。
    """

    # 在工作副本上运行（避免与正在开发的源码冲突）
    import tempfile, shutil
    work = tempfile.mkdtemp(prefix="aimv-ci-")

    # cp 源文件到工作目录
    src_copy = Path(work) / Path(source_file).name
    shutil.copy2(source_file, src_copy)

    # [AIMV] 通过 clang -faimv 入口运行完整管线
    # clang Driver 自动:
    #   1. 编译 + AIMVFeedbackPass → aimv.json
    #   2. fork aimv-driver --from-json=aimv.json
    #   3. aimv-driver 按函数顺序迭代处理
    # 配置通过环境变量注入（aimv-driver 内部按优先级链加载）
    file_output_dir = f"{output_dir}/{Path(source_file).stem}"
    os.makedirs(file_output_dir, exist_ok=True)

    env = os.environ.copy()
    env["AIMV_OUTPUT_DIR"] = file_output_dir

    cmd = [
        "clang", "-O2", "-faimv",
        "-c", str(src_copy),
        "-o", f"{work}/{Path(source_file).stem}.o",
    ]

    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
    )
    stdout, stderr = await proc.communicate()

    # 从 session JSONs 读取结果
    results = []
    sessions_dir = Path(file_output_dir) / "sessions"
    if sessions_dir.exists():
        for session_path in sorted(sessions_dir.glob("*.json")):
            with open(session_path) as f:
                session_data = json.load(f)

            # 按 PerFunctionResult (PLAN §3.2) 提取每个函数的结果
            for func_result in session_data.get("functions", []):
                results.append({
                    "file": source_file,
                    "function": func_result.get("function_name", "unknown"),
                    "status": func_result.get("termination_reason", "unknown"),
                    "vectorized": func_result.get("vectorized", False),
                    "rounds": len(func_result.get("rounds", [])),
                    "session_id": session_data.get("session_id"),
                })

            # 向后兼容: 单函数 session（无 functions 列表）
            if not session_data.get("functions") and session_data.get("function_name"):
                results.append({
                    "file": source_file,
                    "function": session_data.get("function_name", "unknown"),
                    "status": session_data.get("termination_reason", "unknown"),
                    "vectorized": session_data.get("termination_reason") == "vectorized",
                    "rounds": len(session_data.get("rounds", [])),
                    "session_id": session_data.get("session_id"),
                })

    if not results:
        results.append({
            "file": source_file,
            "function": "(file-level)",
            "status": "error",
            "error": stderr.decode()[:500] if stderr else "no session output",
            "vectorized": False,
            "rounds": 0,
        })

    return results


def _summarize(results: list) -> dict:
    """汇总批量结果。"""
    total = len(results)
    vectorized = sum(1 for r in results if r.get("vectorized"))
    round_limit = sum(1 for r in results if r.get("status") == "round_limit")
    no_suggestion = sum(1 for r in results if r.get("status") == "no_suggestion")
    compile_error = sum(1 for r in results if r.get("status") == "compile_error")
    test_failure = sum(1 for r in results if r.get("status") == "test_failure")
    errors = sum(1 for r in results if r.get("status") == "error")

    return {
        "summary": {
            "total_functions": total,
            "vectorized": vectorized,
            "round_limit": round_limit,
            "no_suggestion": no_suggestion,
            "compile_error": compile_error,
            "test_failure": test_failure,
            "errors": errors,
            "success_rate": vectorized / max(total, 1),
        },
        "details": results,
    }
```

---

## 5. 报告工具（aimv-report）

### 5.1 工具说明

`aimv-report` 是独立的报告生成 CLI，读取 aimv-run-batch 产出的 session JSON 文件，汇总 PerFunctionResult 和 TerminationReason（PLAN §3.2），输出 Markdown 或 GitLab API 格式报告。

```python
# [AIMV] ci/aimv_report.py

"""
aimv-report — 从 session JSON 生成向量化分析报告

用法:
  aimv-report --input ./aimv-results --format markdown --output report.md
  aimv-report --input ./aimv-results --format gitlab-api --output -   (stdout)
"""

import json
import sys
from pathlib import Path
from typing import List, Optional
from datetime import datetime


# [AIMV] TerminationReason → 人类可读标签（对应 PLAN §3.2）
TERMINATION_LABELS = {
    "vectorized":       ("向量化成功", "heavy_check_mark"),
    "round_limit":      ("达到轮次上限", "warning"),
    "no_improvement":   ("收益不增", "warning"),
    "no_suggestion":    ("AI 无可用建议", "information_source"),
    "compile_error":    ("编译失败", "x"),
    "test_failure":     ("测试失败", "x"),
    "interrupted":      ("中断", "grey_exclamation"),
}


def collect_results(input_dir: str) -> List[dict]:
    """扫描 input_dir 下所有 session JSON，提取 PerFunctionResult。"""
    results = []
    sessions_dir = Path(input_dir)

    # 递归查找 session JSON
    for session_path in sorted(sessions_dir.rglob("*.json")):
        if session_path.name.endswith(".tmp"):
            continue
        try:
            with open(session_path) as f:
                session_data = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue

        session_id = session_data.get("session_id", session_path.stem)

        # 新格式: SessionRecord.functions 列表 (PLAN §3.2)
        for func_result in session_data.get("functions", []):
            results.append({
                "file": session_data.get("source_file", ""),
                "function": func_result.get("function_name", "unknown"),
                "termination_reason": func_result.get("termination_reason"),
                "vectorized": func_result.get("vectorized", False),
                "rounds": len(func_result.get("rounds", [])),
                "session_id": session_id,
            })

        # 向后兼容: 单函数 session（无 functions 列表）
        if not session_data.get("functions") and session_data.get("function_name"):
            reason = session_data.get("termination_reason")
            results.append({
                "file": session_data.get("source_file", ""),
                "function": session_data.get("function_name"),
                "termination_reason": reason,
                "vectorized": reason == "vectorized",
                "rounds": len(session_data.get("rounds", [])),
                "session_id": session_id,
            })

    return results


def generate_summary(results: List[dict]) -> dict:
    """汇总统计数据。"""
    total = len(results)
    vectorized = sum(1 for r in results if r.get("vectorized"))
    failed = sum(1 for r in results if r.get("termination_reason")
                 in ("compile_error", "test_failure"))
    gave_up = sum(1 for r in results if r.get("termination_reason")
                  in ("round_limit", "no_improvement"))
    no_suggestion = sum(1 for r in results if r.get("termination_reason")
                        == "no_suggestion")
    errors = total - vectorized - failed - gave_up - no_suggestion

    return {
        "total_functions": total,
        "vectorized": vectorized,
        "failed": failed,
        "gave_up": gave_up,
        "no_suggestion": no_suggestion,
        "errors": max(errors, 0),
        "success_rate": vectorized / max(total, 1),
    }


def render_markdown(summary: dict, results: List[dict]) -> str:
    """渲染 Markdown 报告。"""
    lines = [
        "<!-- AIMV Vectorization Analysis -->",
        "",
        "## AIMV 向量化分析报告",
        "",
        "| 指标 | 值 |",
        "|------|----|",
        f"| 分析函数数 | {summary['total_functions']} |",
        f"| 成功向量化 | {summary['vectorized']} |",
        f"| AI 无法建议 | {summary['no_suggestion']} |",
        f"| 达到轮次上限 | {summary['gave_up']} |",
        f"| 编译/测试失败 | {summary['failed']} |",
        f"| 成功率 | {summary['success_rate']:.0%} |",
        "",
        "### 详情",
        "",
    ]

    for item in results:
        reason = item.get("termination_reason") or "unknown"
        label, icon = TERMINATION_LABELS.get(reason, (reason, "grey_question"))
        func = item.get("function", "unknown")
        src = item.get("file", "")
        rounds = item.get("rounds", 0)
        vec = item.get("vectorized", False)

        lines.append("<details>")
        status_marker = "成功" if vec else label
        lines.append(f"<summary><b>{func}</b> ({src}) -- {status_marker}</summary>")
        lines.append("")
        lines.append(f"- **状态**: {reason}")
        lines.append(f"- **迭代轮次**: {rounds}")
        if item.get("session_id"):
            lines.append(f"- **Session**: `{item['session_id']}`")
        lines.append("")
        lines.append("</details>")
        lines.append("")

    lines.append("---")
    lines.append(f"*分析时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}*")

    return "\n".join(lines)


def render_gitlab_api(summary: dict, results: List[dict]) -> str:
    """渲染 GitLab MR note API 兼容的 JSON。"""
    body = render_markdown(summary, results)
    return json.dumps({"body": body}, ensure_ascii=False)


def main():
    """CLI 入口。"""
    import argparse
    parser = argparse.ArgumentParser(description="AIMV report generator")
    parser.add_argument("--input", required=True, help="Directory containing session JSONs")
    parser.add_argument("--format", choices=["markdown", "gitlab-api"],
                        default="markdown", help="Output format")
    parser.add_argument("--output", default="-",
                        help="Output file path ('-' for stdout)")
    args = parser.parse_args()

    results = collect_results(args.input)
    summary = generate_summary(results)

    if args.format == "markdown":
        content = render_markdown(summary, results)
    elif args.format == "gitlab-api":
        content = render_gitlab_api(summary, results)
    else:
        content = render_markdown(summary, results)

    if args.output == "-":
        sys.stdout.write(content)
    else:
        Path(args.output).write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
```

### 5.2 MR 报告模板（Markdown 输出示例）

```
<!-- AIMV Vectorization Analysis -->

## AIMV 向量化分析报告

| 指标 | 值 |
|------|----|
| 分析函数数 | 5 |
| 成功向量化 | 2 |
| AI 无法建议 | 1 |
| 达到轮次上限 | 1 |
| 编译/测试失败 | 0 |
| 成功率 | 40% |

### 详情

<details>
<summary><b>process_task</b> (src/task.c) -- 向量化成功</summary>

- **状态**: vectorized
- **迭代轮次**: 2
- **Session**: `aimv-a1b2c3d4e5f6`

</details>

<details>
<summary><b>filter_samples</b> (src/dsp.c) -- 向量化成功</summary>

- **状态**: vectorized
- **迭代轮次**: 1
- **Session**: `aimv-f7g8h9i0j1k2`

</details>

<details>
<summary><b>compute_matrix</b> (src/math.c) -- 达到轮次上限</summary>

- **状态**: round_limit
- **迭代轮次**: 5
- **Session**: `aimv-l3m4n5o6p7q8`

</details>

<details>
<summary><b>init_buffers</b> (src/init.c) -- AI 无可用建议</summary>

- **状态**: no_suggestion
- **迭代轮次**: 0
- **Session**: `aimv-r9s0t1u2v3w4`

</details>

---
*分析时间: 2026-05-17 14:30:22*
```

---

## 6. 验证 Benchmark 集

### 6.1 Benchmark 表（与 SPEC §8.2 对齐）

从 `aimv/benchmarks/` 中选取至少 5 个已知有向量化失败场景的 C 文件：

| Benchmark | 失败类型 | 目标 | 阶段 |
|-----------|---------|------|------|
| `dep_fail_alias.c` | 别名分析失败 (CantReorderMemOps) | restrict 修复后向量化 | MVP |
| `dep_fail_stride.c` | 跨迭代 RAW 依赖 | loop fission 或 restrict | MVP |
| `cost_reject.c` | 代价模型拒绝 | pragma 或结构调整 | Phase 2 |
| `align_unknown.c` | 对齐未知 | alignas 修复 | Phase 2 |
| `multi_fail.c` | 混合失败（依赖+代价） | 多轮迭代 | Phase 2 |

### 6.2 测量方法（与 SPEC §8.1 对齐）

**工具**: Linux `perf stat`（PMU 计数器: cycles, instructions, branches, branch-misses）或 `clock_gettime()` 高精度计时。

**策略**: 预热 3 次后取 10 次运行的**中位数**（排除首轮冷启动和末轮噪声）。

**基线**: 优化前原始代码在 `-O2` 下的中位数执行时间。

**合格阈值**: 中位数执行时间缩短 >= **5%** 且所有测试无回归。

```bash
# [AIMV] benchmarks/perf_runner.sh
# 标准化性能测量脚本

#!/bin/bash
set -euo pipefail

BENCHMARK="$1"
WARMUP=3
RUNS=10

# 编译基线版本 (-O2, 不含 AIMV)
clang -O2 -o "${BENCHMARK}.baseline" "${BENCHMARK}"

# 编译 AIMV 优化版本 (通过 aimv 修改后的源码)
clang -O2 -o "${BENCHMARK}.aimv" "${BENCHMARK}.aimv.c"

# 预热
for i in $(seq 1 $WARMUP); do
    ./"${BENCHMARK}.baseline" > /dev/null 2>&1 || true
    ./"${BENCHMARK}.aimv" > /dev/null 2>&1 || true
done

# 采集执行时间（中位数策略）
baseline_times=()
aimv_times=()

for i in $(seq 1 $RUNS); do
    t_base=$(perf stat -e cycles ./"${BENCHMARK}.baseline" 2>&1 \
             | grep "cycles" | awk '{print $1}' | tr -d ',')
    t_aimv=$(perf stat -e cycles ./"${BENCHMARK}.aimv" 2>&1 \
             | grep "cycles" | awk '{print $1}' | tr -d ',')

    baseline_times+=("$t_base")
    aimv_times+=("$t_aimv")
done

# 计算中位数（动态计算中间位置，而非硬编码 NR==6）
median_base=$(printf '%s\n' "${baseline_times[@]}" | sort -n | awk '{a[NR]=$0} END{print a[int(NR/2)+1]}')
median_aimv=$(printf '%s\n' "${aimv_times[@]}" | sort -n | awk '{a[NR]=$0} END{print a[int(NR/2)+1]}')

# 计算改善百分比
improvement=$(echo "scale=2; ($median_base - $median_aimv) / $median_base * 100" | bc)

echo "Baseline median (cycles): $median_base"
echo "AIMV median (cycles):     $median_aimv"
echo "Improvement:              ${improvement}%"

# 阈值判定: >= 5% 改善为合格
if (( $(echo "$improvement >= 5.0" | bc -l) )); then
    echo "PASS: improvement >= 5%"
    exit 0
else
    echo "FAIL: improvement < 5%"
    exit 1
fi
```

### 6.3 CI 中的 Benchmark 运行

```yaml
# .github/workflows/aimv-benchmark.yml

name: AIMV Benchmark Validation

on:
  push:
    branches: [main]
    paths: ['aimv/benchmarks/**']

jobs:
  benchmark:
    runs-on: [self-hosted, arm64]
    steps:
      - uses: actions/checkout@v4

      - name: Run benchmarks
        run: |
          for bench in aimv/benchmarks/dep_fail_alias.c aimv/benchmarks/dep_fail_stride.c; do
            echo "=== Benchmark: $(basename $bench) ==="
            ./aimv/benchmarks/perf_runner.sh "$bench"
          done

      - name: Analyze results
        run: |
          python3 aimv/benchmarks/analyze_results.py ./aimv-results
```

---

## 7. 门禁策略

### 7.1 三级门禁

```yaml
# aimv-gate-config.yaml

gate:
  # Level 0: 纯信息报告 (默认)
  #   - 不阻止合入
  #   - 仅生成 MR 评论 + artifact
  mode: "report"

  # Level 1: 回归门禁
  #   - 如果新代码导致已有向量化的函数退化，阻止合入
  # mode: "regression"
  regression:
    # 依赖 baseline: 从 main 分支的 aimv session JSON 获取
    baseline_artifact: "aimv-baseline/latest"
    # 性能退化阈值（与 SPEC §8.1 测量方法一致: 中位数, >= 5% 改善）
    perf_degradation_threshold_pct: 5.0
    # 已有向量化覆盖率的退化阈值
    coverage_degradation_threshold_pct: 2.0

  # Level 2: 强制向量化
  #   - 特定目录/文件必须达到最低向量化覆盖率
  # mode: "enforce"
  enforce:
    rules:
      - path_pattern: "src/dsp/**/*.c"
        min_vectorization_rate: 0.6       # 60% 循环必须向量化
      - path_pattern: "src/control/**/*.c"
        min_vectorization_rate: 0.0       # 控制代码不强求
```

**注意**: Phase 1 (MVP) 仅覆盖依赖分析失败场景（benchmark 表中 `dep_fail_alias.c` 和 `dep_fail_stride.c`）。门禁的回归检测和强制向量化在 Phase 1 仅对依赖分析维度有效。Phase 2 扩展至代价模型和对齐失败维度后，门禁覆盖范围相应扩大。

### 7.2 门禁决策逻辑

```python
# [AIMV] ci/gate.py

def evaluate_gate(summary: dict, config: dict, baseline: Optional[dict] = None) -> dict:
    """根据门禁配置评估是否放行。

    返回: {"allow": bool, "reason": str, "details": dict}
    """

    mode = config.get("mode", "report")

    if mode == "report":
        return {"allow": True, "reason": "report-only mode, always allow"}

    if mode == "regression" and baseline:
        # 检查性能退化
        for item in summary["details"]:
            func_name = item["function"]
            baseline_func = _find_baseline(baseline, func_name)
            if baseline_func and baseline_func.get("vectorized"):
                # 基线已向量化但当前版本未向量化 → 退化
                if not item.get("vectorized"):
                    return {
                        "allow": False,
                        "reason": f"vectorization regression in {func_name}: was vectorized, now is not",
                    }

        return {"allow": True, "reason": "no regressions detected"}

    if mode == "enforce":
        for rule in config["enforce"]["rules"]:
            pattern = rule["path_pattern"]
            min_rate = rule["min_vectorization_rate"]

            # 筛选匹配路径的函数
            matching = [d for d in summary["details"]
                       if _path_matches(d["file"], pattern)]

            if matching:
                vec_count = sum(1 for d in matching if d.get("vectorized"))
                rate = vec_count / len(matching)
                if rate < min_rate:
                    return {
                        "allow": False,
                        "reason": (
                            f"vectorization rate {rate:.0%} below minimum "
                            f"{min_rate:.0%} for {pattern}"
                        ),
                    }

        return {"allow": True, "reason": "all enforce rules satisfied"}

    return {"allow": True, "reason": "unknown mode, allowing"}
```

---

## 8. 缓存策略

### 8.1 多层缓存

```
请求层
  │
  ├── L1: MCP 诊断缓存 (见 MCP_DESIGN.md §7)
  │     相同诊断指纹 → 直接返回缓存的 AI 建议
  │     TTL: 24h
  │
  ├── L2: CI 编译缓存
  │     同一 commit SHA 的编译产物不重复构建
  │     使用 ccache / sccache
  │
  └── L3: Baseline 快照缓存
        main 分支的 aimv 分析结果定期保存为 baseline artifact
        用于回归检测的比较基准
        TTL: 保留最近 10 次 main 构建
```

### 8.2 Baseline 管理

```yaml
# .github/workflows/aimv-baseline.yml (main 分支合并后触发)

name: AIMV Baseline Update

on:
  push:
    branches: [main]
    paths: ['**/*.c', '**/*.cpp']

jobs:
  update-baseline:
    runs-on: [self-hosted, arm64]
    steps:
      - uses: actions/checkout@v4
      - name: Run full AIMV analysis on main
        env:
          AIMV_LEVEL: conservative
        run: |
          # [AIMV] 全量分析 main 分支的向量化状态
          # 使用 clang -faimv 驱动（aimv-driver --from-json 模式）
          find . -name '*.c' -path '*/src/*' | while read f; do
            clang -O2 -faimv -c "$f" -o /dev/null || true
          done
      - name: Upload baseline artifact
        uses: actions/upload-artifact@v4
        with:
          name: aimv-baseline
          path: ./aimv-results/
```

---

## 9. 通知集成

```python
# [AIMV] ci/notifications.py

"""
通知渠道:
  - MR 评论 (主要渠道，GitHub/GitLab API)
  - Slack Webhook
  - 钉钉机器人
  - 邮件 (仅严重回归时)
"""


def notify_slack(webhook_url: str, summary: dict):
    """Slack 通知模板。仅通知高价值事件。"""
    vec_count = summary["summary"].get("vectorized", 0)
    if vec_count == 0:
        return  # 无向量化成功，不发通知

    payload = {
        "blocks": [
            {
                "type": "header",
                "text": {"type": "plain_text", "text": "AIMV: 向量化建议就绪"}
            },
            {
                "type": "section",
                "text": {
                    "type": "mrkdwn",
                    "text": (
                        f"*{vec_count}* 个函数的向量化 patch 可应用\n"
                        f"成功率: {summary['summary']['success_rate']:.0%}\n"
                        f"<{summary['report_url']}|查看完整报告>"
                    )
                }
            }
        ]
    }

    import httpx
    httpx.post(webhook_url, json=payload)
```

---

## 10. 配置优先级

### 10.1 配置链（与 DRIVER_DESIGN §8.1 对齐）

```
环境变量（最高优先级）
    │
    ├── AIMV_MCP_URL         MCP 服务地址
    ├── AIMV_MCP_API_KEY     MCP API 密钥（CI secrets 注入）
    ├── AIMV_LEVEL           修改激进度
    ├── AIMV_MAX_ROUNDS      最大迭代轮次
    ├── AIMV_TEST_CMD        测试命令
    └── AIMV_MODE            运行模式 (auto|review|dry-run|off)
            │
            │  环境变量未设置时回退到 ~/.aimv/config:
            │
    ~/.aimv/config (YAML)
            │
            │  配置文件未设置时使用默认值:
            │
            ├── mcp_url       = http://localhost:8080
            ├── aimv_level    = conservative
            ├── max_rounds    = 5
            └── test_cmd      = ""  (空=仅编译验证)
```

### 10.2 CI Secrets 映射

```yaml
# GitHub Actions 示例
env:
  AIMV_MCP_URL: ${{ secrets.AIMV_MCP_URL }}         # MCP 服务地址
  AIMV_MCP_API_KEY: ${{ secrets.AIMV_MCP_API_KEY }} # API 密钥

# GitLab CI 示例
variables:
  AIMV_MCP_URL: "${AIMV_MCP_URL}"
  AIMV_MCP_API_KEY: "${AIMV_MCP_API_KEY}"
```

`aimv-driver --from-json` 内部按优先级链加载：环境变量 **始终覆盖** `~/.aimv/config` 中的同名字段（与 DRIVER_DESIGN §8.1 config.py 实现一致）。CI 中通过 secrets 注入的环境变量优先级最高，即使 `~/.aimv/config` 中存在同名字段也会被覆盖。两者均无则报错。

CI 中 `aimv_level` 默认为 `conservative`（非 moderate），因为 CI 是全自动场景，保守修改是安全基线。若项目需更激进优化，可通过环境变量 `AIMV_LEVEL=moderate` 覆盖。

---

## 11. AIMV-CLI 工具集

CI 集成提供一组独立 CLI 工具：

| 工具 | 用途 |
|------|------|
| `aimv-detect-changes` | 检测 git diff 中变更的函数列表（使用 `clang -ast-dump`） |
| `aimv-run-batch` | 批量并行运行 AIMV（跨文件并行，同文件串行；使用 `clang -faimv` 驱动） |
| `aimv-report` | 读取 session JSON，汇总 PerFunctionResult + TerminationReason，生成 Markdown/GitLab API 格式报告 |
| `aimv-gate` | 根据配置执行门禁决策 |
| `aimv-baseline` | 管理 baseline 快照的上传/下载/比较 |

---

## 12. 目录结构补充

```
aimv/
├── ci/
│   ├── aimv_detect_changes.py       # 变更检测
│   ├── aimv_run_batch.py            # 批量分析
│   ├── aimv_report.py               # 报告生成（aimv-report CLI）
│   ├── aimv_gate.py                 # 门禁决策
│   ├── aimv_baseline.py             # Baseline 管理
│   ├── change_detector.py           # git diff + clang -ast-dump 解析
│   ├── notifications.py             # 通知渠道
│   ├── gate_config.yaml             # 门禁配置模板
│   └── templates/
│       ├── report_markdown.j2       # MR 评论模板
│       └── report_gitlab.j2         # GitLab MR note 模板
```

---

*文档版本: 2.0*
*创建日期: 2026-04-29*
*最后更新: 2026-05-17*
