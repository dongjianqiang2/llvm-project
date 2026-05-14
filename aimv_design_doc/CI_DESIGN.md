# AIMV — CI 集成方案

**版本**: 1.0
**日期**: 2026-04-29
**关联文档**: SPEC.md, PLAN.md, DRIVER_DESIGN.md

---

## 0. 设计目标

将 AIMV 嵌入 CI/CD 流水线，实现代码合入前的**自动向量化检查与建议**。开发者在提交 MR/PR 时，CI 自动对变更函数运行 AIMV 分析，输出向量化报告并作为 MR 评论或 CI 门禁。

核心原则：
- **非阻塞** — 默认不阻止合入，只做信息性报告
- **增量分析** — 只分析 git diff 中变更的函数，不跑全量
- **可缓存** — MCP 结果 + 编译产物缓存，避免重复计算
- **可配置** — 不同仓库/目录可配置不同的 AIMV 等级和门禁策略

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
│  · 提取每个文件中新增/修改的函数名（用 clang AST dump 或 ctags）    │
│  · 过滤：只保留包含循环的函数（可选预分析）                          │
│  · 输出：[(file, function_name), ...]                           │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 2: AIMV 分析 (可并行)                                     │
│  · 对每个 (file, function) 运行 aimv-driver                      │
│  · 每轮尝试的 patch 写入临时分支，验证编译+测试                      │
│  · 输出：per-function session JSON                              │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  Stage 3: 报告生成                                               │
│  · 汇总所有函数的分析结果                                          │
│  · 生成 Markdown 报告（摘要 + 详情）                               │
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
   a. 用 clang-check -ast-dump 解析 AST
   b. 对比 diff 的行号范围，找到被修改的函数定义
   c. 输出 (file, function, start_line, end_line) 元组
3. 过滤：排除不含循环的函数（用 LLVM opt -passes=print<loops> 预检）
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
    """用 clang-check 获取文件中所有函数定义的位置。"""
    cmd = [
        "clang-check", "-ast-dump", file, "--",
        "-fsyntax-only",
        "-I.", "-I..",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

    # 解析 AST dump: FunctionDecl <line:col, line:col> func_name 'type'
    functions = []
    for line in proc.stderr.split("\n"):  # AST dump 输出到 stderr
        if "FunctionDecl" in line and " definition " in line:
            name = line.split()[-1].strip("'")
            # 提取行号范围（简化，实际需要更健壮的解析）
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
    files_processed = set()

    for file, func_name, start, end in functions:
        if file in files_processed:
            continue
        files_processed.add(file)

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
            for func, fn, fs, fe in functions:
                if func not in [f[0] for f in loopy] and fn in proc.stderr:
                    loopy.append((func, fn, fs, fe))
        finally:
            os.unlink(ir_path)

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

      - name: Build baseline (AOT)
        run: |
          cd build
          cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
          make -j$(nproc)

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
          AIMV_MCP_URL: ${{ secrets.AIMV_MCP_URL }}
          AIMV_MCP_API_KEY: ${{ secrets.AIMV_MCP_API_KEY }}
        run: |
          aimv-run-batch \
            --input changes.json \
            --mcp-url "$AIMV_MCP_URL" \
            --aimv-level moderate \
            --max-rounds 3 \
            --test-cmd "make -C build test" \
            --parallel 4 \
            --output-dir ./aimv-results

      - name: Generate report
        if: always()
        run: |
          aimv-report generate \
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
    AIMV_MCP_URL: "${AIMV_MCP_URL}"
    AIMV_MCP_API_KEY: "${AIMV_MCP_API_KEY}"

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
        --mcp-url "$AIMV_MCP_URL"
        --max-rounds 3
        --test-cmd "make -C build test"
        --output-dir ./aimv-results

    # 生成报告
    - aimv-report generate
        --input ./aimv-results
        --format gitlab
        --output aimv-report.md

  after_script:
    # 通过 GitLab API 创建 MR note
    - |
      curl --request POST \
        --header "PRIVATE-TOKEN: ${GITLAB_API_TOKEN}" \
        --header "Content-Type: application/json" \
        --data "$(aimv-report generate --input ./aimv-results --format gitlab-api --output -)" \
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

对变更检测输出的 (file, function) 列表，并行运行 aimv-driver。
考虑以下约束:
  - 同一源文件的函数串行分析（避免 patch 冲突）
  - 不同源文件的函数并行分析
  - 全局最大并行度由 --parallel 控制
"""

import asyncio
import json
from pathlib import Path
from collections import defaultdict


async def run_batch(
    changes: List[Tuple[str, str, int, int]],
    mcp_url: str,
    aimv_level: str = "moderate",
    max_rounds: int = 3,
    test_cmd: str = "make test",
    max_parallel: int = 4,
    output_dir: str = "./aimv-results",
) -> dict:
    """并行批量运行 AIMV。返回汇总字典。"""

    # 按文件分组：同文件函数串行队列
    by_file: dict[str, list] = defaultdict(list)
    for file, func, start, end in changes:
        by_file[file].append((func, start, end))

    # 跨文件并行，单文件内串行
    semaphore = asyncio.Semaphore(max_parallel)

    async def analyze_file(file: str, funcs: list) -> list:
        results = []
        for func_name, start, end in funcs:
            async with semaphore:
                result = await _run_single(
                    file, func_name, mcp_url, aimv_level,
                    max_rounds, test_cmd, output_dir
                )
                results.append(result)
        return results

    # 启动所有文件的分析任务
    tasks = []
    for file, funcs in by_file.items():
        tasks.append(analyze_file(file, funcs))

    all_results = await asyncio.gather(*tasks)

    # 汇总
    flat_results = [r for file_results in all_results for r in file_results]
    return _summarize(flat_results)


async def _run_single(
    source_file: str,
    function_name: str,
    mcp_url: str,
    aimv_level: str,
    max_rounds: int,
    test_cmd: str,
    output_dir: str,
) -> dict:
    """运行单次 aimv-driver 分析。"""

    # 在工作副本上运行（避免与正在开发的源码冲突）
    import tempfile, shutil
    work = tempfile.mkdtemp(prefix="aimv-ci-")

    # cp 源文件到工作目录
    src_copy = Path(work) / Path(source_file).name
    shutil.copy2(source_file, src_copy)

    # 调用 aimv-driver
    cmd = [
        "aimv-driver",
        "--function", function_name,
        "--mcp-url", mcp_url,
        "--aimv-level", aimv_level,
        "--max-rounds", str(max_rounds),
        "--test-cmd", test_cmd,
        "--output-dir", f"{output_dir}/{function_name}",
        "--json-log",
        str(src_copy),
    ]

    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await proc.communicate()

    # 读取 session JSON 获取结果摘要
    import glob as glob_mod
    sessions = list(Path(f"{output_dir}/{function_name}/sessions").glob("*.json"))
    if sessions:
        with open(sessions[0]) as f:
            session_data = json.load(f)
        return {
            "file": source_file,
            "function": function_name,
            "status": session_data.get("termination_reason", "unknown"),
            "rounds": len(session_data.get("rounds", [])),
            "final_patch": session_data.get("final_patch_path"),
            "perf_improvement_pct": session_data.get("overall_perf_improvement_pct"),
            "session_id": session_data.get("session_id"),
        }
    else:
        return {
            "file": source_file,
            "function": function_name,
            "status": "error",
            "error": stderr.decode()[:500] if stderr else "unknown",
            "rounds": 0,
            "final_patch": None,
            "perf_improvement_pct": None,
        }


def _summarize(results: list) -> dict:
    """汇总批量结果。"""
    total = len(results)
    succeeded = sum(1 for r in results if r.get("status") == "vectorized")
    gave_up = sum(1 for r in results if r.get("status") == "gave_up")
    no_suggestion = sum(1 for r in results if r.get("status") == "no_suggestion")
    errors = sum(1 for r in results if r.get("status") == "error")

    patches_ready = [r for r in results if r.get("final_patch")]
    perf_improvements = [r for r in results if r.get("perf_improvement_pct")]

    return {
        "summary": {
            "total_functions": total,
            "vectorized": succeeded,
            "gave_up": gave_up,
            "no_suggestion": no_suggestion,
            "errors": errors,
            "success_rate": succeeded / max(total, 1),
            "patches_ready": len(patches_ready),
        },
        "details": results,
    }
```

---

## 5. MR 报告格式

### 5.1 Markdown 报告模板

```markdown
<!-- AIMV Vectorization Analysis -->

## AIMV 向量化分析报告

| 指标 | 值 |
|------|----|
| 分析函数数 | {{ summary.total_functions }} |
| 成功向量化 | {{ summary.vectorized }} |
| AI 无法建议 | {{ summary.no_suggestion }} |
| 分析失败 | {{ summary.errors }} |
| 可应用 patch | {{ summary.patches_ready }} |
| 成功率 | {{ "%.0f"|format(summary.success_rate * 100) }}% |

### 详情

{% for item in details %}
<details>
<summary>
  <b>{{ item.function }}</b> ({{ item.file }})
  {% if item.status == "vectorized" %} ✅ 向量化成功
  {% elif item.status == "gave_up" %} ⚠️ 无法完成
  {% elif item.status == "no_suggestion" %} ℹ️ AI 无可用建议
  {% else %} ❌ 错误
  {% endif %}
</summary>

- **状态**: {{ item.status }}
- **迭代轮次**: {{ item.rounds }}
{% if item.perf_improvement_pct %}
- **性能提升**: {{ "%.1f"|format(item.perf_improvement_pct) }}%
{% endif %}
{% if item.final_patch %}
- **建议 patch**: [查看]({{ artifact_url }}/{{ item.session_id }})
{% endif %}
{% if item.error %}
- **错误信息**: `{{ item.error }}`
{% endif %}

</details>
{% endfor %}

---
*分析时间: {{ timestamp }} | 总耗时: {{ elapsed }}s | MCP 后端: {{ mcp_backend }}*
```

### 5.2 MR 评论截图示意

```
┌─────────────────────────────────────────────────────────┐
│ 🤖 AIMV Bot commented • 3 minutes ago                  │
│                                                         │
│ ## AIMV 向量化分析报告                                   │
│                                                         │
│ | 指标 | 值 |                                           │
│ |------|----|                                           │
│ | 分析函数数 | 5 |                                       │
│ | 成功向量化 | 2 |          ← 绿色                      │
│ | AI 无法建议 | 1 |         ← 灰色                      │
│ | 分析失败 | 0 |                                         │
│ | 可应用 patch | 2 |                                    │
│ | 成功率 | 40% |                                        │
│                                                         │
│ ### 详情                                                │
│                                                         │
│ <details>                                               │
│   ✅ process_task (src/task.c) — 向量化成功              │
│   性能提升: 35.2%                                       │
│   [查看 patch]                                          │
│ </details>                                              │
│                                                         │
│ <details>                                               │
│   ✅ filter_samples (src/dsp.c) — 向量化成功             │
│   性能提升: 18.7%                                       │
│   [查看 patch]                                          │
│ </details>                                              │
│                                                         │
│ <details>                                               │
│   ⚠️ compute_matrix (src/math.c) — 无法完成              │
│   轮次: 5/5 (达到上限)                                  │
│   原因: 代价模型持续拒绝，建议人工 review                 │
│ </details>                                              │
│                                                         │
│ <details>                                               │
│   ℹ️ init_buffers (src/init.c) — AI 无可用建议           │
│   原因: 循环内包含 NEON 不支持的 div 指令                │
│ </details>                                              │
└─────────────────────────────────────────────────────────┘
```

---

## 6. 门禁策略

### 6.1 三级门禁

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
    # 性能退化阈值
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

### 6.2 门禁决策逻辑

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
            if baseline_func and baseline_func.get("perf_improvement_pct"):
                delta = (item.get("perf_improvement_pct", 0) -
                         baseline_func["perf_improvement_pct"])
                threshold = config["regression"]["perf_degradation_threshold_pct"]
                if delta < -threshold:
                    return {
                        "allow": False,
                        "reason": f"performance regression in {func_name}: {delta:.1f}%",
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
                vec_count = sum(1 for d in matching if d["status"] == "vectorized")
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

## 7. 缓存策略

### 7.1 多层缓存

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

### 7.2 Baseline 管理

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
        run: |
          aimv-run-batch \
            --input all-functions.json \
            --mcp-url "$AIMV_MCP_URL" \
            --output-dir ./aimv-baseline
      - name: Upload baseline artifact
        uses: actions/upload-artifact@v4
        with:
          name: aimv-baseline
          path: ./aimv-baseline/
```

---

## 8. 通知集成

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
    if summary["summary"]["patches_ready"] == 0:
        return  # 无可用 patch，不发通知

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
                        f"*{summary['summary']['patches_ready']}* 个函数的向量化 patch 可应用\n"
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

## 9. AIMV-CLI 工具集

CI 集成提供一组独立 CLI 工具：

| 工具 | 用途 |
|------|------|
| `aimv-detect-changes` | 检测 git diff 中变更的函数列表 |
| `aimv-run-batch` | 批量并行运行 aimv-driver |
| `aimv-report` | 汇总 session JSON，生成 Markdown/GitLab API 格式报告 |
| `aimv-gate` | 根据配置执行门禁决策 |
| `aimv-baseline` | 管理 baseline 快照的上传/下载/比较 |

---

## 10. 目录结构补充

```
aimv/
├── ci/
│   ├── aimv_detect_changes.py       # 变更检测
│   ├── aimv_run_batch.py            # 批量分析
│   ├── aimv_report.py              # 报告生成
│   ├── aimv_gate.py                # 门禁决策
│   ├── aimv_baseline.py            # Baseline 管理
│   ├── change_detector.py          # git diff + AST 解析
│   ├── notifications.py            # 通知渠道
│   ├── gate_config.yaml            # 门禁配置模板
│   └── templates/
│       ├── report_markdown.j2      # MR 评论模板
│       └── report_gitlab.j2        # GitLab MR note 模板
```

---

*文档版本: 1.0*
*创建日期: 2026-04-29*
