# [AIMV] MCP Server — Prompt builder: AnalyzeRequest → LLM prompt
import os
from .models import AnalyzeRequest, HistoryRecord

_TEMPLATES_DIR = os.path.join(os.path.dirname(__file__), "templates")


def _load_template(name: str) -> str:
    path = os.path.join(_TEMPLATES_DIR, name)
    with open(path) as f:
        return f.read()


_COST_REJECT_TEMPLATE = _load_template("cost_reject_prompt.txt")
_ALIGN_TEMPLATE = _load_template("align_prompt.txt")

SYSTEM_PROMPT_TEMPLATE = """\
You are an expert compiler engineer specializing in automatic vectorization
of C/C++ code for embedded systems. You analyze LLVM optimization remarks
and suggest source-level fixes to enable loop vectorization.

## Your Task
Given:
1. The current version of the C source code of a function containing a loop
   (note: this source may already contain changes from prior iterations for
   other functions in the same file; line numbers match this version exactly)
2. LLVM diagnostic data explaining WHY vectorization failed
3. LLVM IR snippets showing how the compiler sees the loop
4. Cost model details and memory dependency analysis

You must determine a source-level modification to fix the failure.

## Target Platform
- Triple: {triple}
- CPU: {cpu}
- SIMD: {features} ({vector_width}-bit vectors)

## Rules
1. DO NOT change program semantics. The fix must be behavior-preserving.
2. Prefer minimal changes: add qualifiers (restrict, const, alignas) before
   rewriting loop structure.
3. Suggest ONE change per iteration. The driver will re-run the compiler and
   send you updated diagnostics if the first fix isn't sufficient.
4. If you cannot determine a safe fix, set `no_action_possible: true`.
5. Provide a valid unified diff in the `diff` field. Line numbers in the diff
   must match the current source_code version (not the original file).
6. Only suggest modifications to .c/.cpp source files, never to headers
   (.h/.hpp). Header file changes (static inline functions, macros) are out
   of scope.
7. For {aimv_level} level, you may:
   - conservative: only add qualifiers and attributes
   - moderate: also suggest loop fission, interchange, scalar promotion
   - aggressive: also suggest data structure changes (AoS->SoA)

## Output Format
Respond with a single JSON object matching this exact schema:
{response_schema}

Do not include any text outside the JSON.
"""

RESPONSE_SCHEMA_JSON = """\
{
  "suggestions": [
    {
      "description": "<one-line summary>",
      "reasoning": "<detailed analysis of the failure and why this fix works>",
      "source_file": "<path>",
      "line_start": <int>,
      "line_end": <int>,
      "original": "<original source text>",
      "modified": "<modified source text>",
      "diff": "<unified diff>",
      "estimated_impact": "high|medium|low",
      "safety_concern": "<null or string describing potential risk>"
    }
  ],
  "overall_analysis": "<summary paragraph>",
  "confidence": <float 0.0-1.0>,
  "no_action_possible": false
}
"""

DEPTYPE_SEMANTICS = (
    '"Forward"=safe read-after-write; "Backward"=unsafe write-after-read; '
    '"BackwardVectorizable"=safe with element-wise overlap; '
    '"BackwardVectorizableButPreventsForwarding"=safe but blocks store forwarding; '
    '"ForwardButPreventsForwarding"=safe but blocks store forwarding; '
    '"Unknown"=SCEV cannot determine direction, treat as PossiblyBackward (potentially unsafe); '
    '"IndirectUnsafe"=unsafe through pointer aliasing.'
)


def build_system_prompt(request: AnalyzeRequest) -> str:
    return SYSTEM_PROMPT_TEMPLATE.format(
        triple=request.target.triple,
        cpu=request.target.cpu,
        features=", ".join(request.target.features) if request.target.features else "default",
        vector_width=request.target.vector_width,
        aimv_level=request.aimv_level.value,
        response_schema=RESPONSE_SCHEMA_JSON,
    )


def build_user_prompt(request: AnalyzeRequest) -> str:
    lines = []

    # Function context
    f = request.function
    lines.append("## Function Under Analysis\n")
    lines.append(f"**Name:** `{f.name}`")
    lines.append(f"**Signature:** `{f.signature}`")
    lines.append(f"**File:** `{f.source_file}`, line {f.loop_line}\n")
    lines.append("### Source Code (current version with prior changes applied)\n")
    lines.append("```c")
    lines.append(f.source_code)
    lines.append("```\n")
    lines.append("---\n")

    # Diagnostics
    for i, diag in enumerate(request.diagnostics):
        lines.append(f"### Vectorization Failure #{i + 1}\n")
        lines.append(f"**Pass:** {diag.pass_name}")
        lines.append(f"**Remark ID:** {diag.remark_id}")
        lines.append(f"**Message:** {diag.remark_text}")
        lines.append(f"**Severity:** {diag.severity.value}")

        # source_accuracy warning (MCP_DESIGN §4.2)
        if diag.source_accuracy == "approximate":
            lines.append(
                "**WARNING:** Source location is approximate — line numbers may be off "
                "by +/-5 lines due to IR optimization passes reordering instructions. "
                "Pay extra attention when generating diffs."
            )
        lines.append("")

        if diag.ir_snippet:
            lines.append("#### LLVM IR (optimized, surrounding the loop)\n")
            lines.append("```llvm")
            lines.append(diag.ir_snippet)
            lines.append("```\n")

        # Cost model (MCP_DESIGN §4.2: VF=0 conditional rendering)
        if diag.cost_model:
            cm = diag.cost_model
            lines.append("#### Cost Model Analysis\n")
            lines.append("| Metric | Value |")
            lines.append("|--------|-------|")
            lines.append(f"| Scalar cost | {cm.scalar_cost} |")
            if cm.vf == 0:
                vf_label = "not determined, legality rejection"
            else:
                vf_label = str(cm.vf)
            lines.append(f"| Vector cost (VF={vf_label}) | {cm.vector_cost} |")
            lines.append(f"| Interleave count | {cm.interleave_count} |")
            denom = max(cm.scalar_cost, 1)
            lines.append(f"| Cost ratio | {cm.vector_cost / denom:.1f}x |")
            if cm.scalar_cost < cm.vector_cost:
                lines.append(
                    f"\n**Key insight:** Vector cost ({cm.vector_cost}) > scalar cost "
                    f"({cm.scalar_cost}). The cost model estimates vectorization is NOT profitable.\n"
                )
        else:
            lines.append("Cost model data not available (legality stage rejection).\n")

        # Dependencies (MCP_DESIGN §4.2: dep_type semantics)
        if diag.dependencies:
            lines.append("#### Memory Dependencies\n")
            lines.append(f"**DepType semantics:** {DEPTYPE_SEMANTICS}")
            lines.append("| # | Type | Source | Sink | Alias Result |")
            lines.append("|---|------|--------|------|-------------|")
            for j, dep in enumerate(diag.dependencies):
                lines.append(
                    f"| {j + 1} | `{dep.dep_type}` | `{dep.source_ptr}` | "
                    f"`{dep.sink_ptr}` | {dep.alias_result} |"
                )
            lines.append("")
        else:
            lines.append("No dependency information available.\n")

        # Memory info
        if diag.memory_info:
            mi = diag.memory_info
            lines.append("#### Memory Access Pattern\n")
            lines.append("| Attribute | Value |")
            lines.append("|-----------|-------|")
            lines.append(f"| Stores / Loads | {mi.num_stores} / {mi.num_loads} |")
            lines.append(f"| Max alignment | {mi.max_alignment} bytes |")
            lines.append(f"| Stride | {mi.stride} |")
            check_count = mi.memory_check_count if mi.memory_check_count is not None else "N/A"
            check_cost = mi.memory_check_cost if mi.memory_check_cost is not None else "N/A"
            lines.append(f"| Memory checks needed | {check_count} (cost: {check_cost}) |")
            lines.append("")

        # Loop structure
        if diag.loop_info:
            li = diag.loop_info
            trip = li.trip_count if li.trip_count >= 0 else "unknown"
            lines.append("#### Loop Structure\n")
            lines.append("| Blocks | Instructions | Trip count | Branches | Calls |")
            lines.append("|--------|-------------|------------|----------|-------|")
            lines.append(f"| {li.num_blocks} | {li.num_instructions} | {trip} | {li.num_branches} | {li.num_calls} |")
            lines.append("")

        lines.append("---\n")

    # History (MCP_DESIGN §4.2: structured HistoryRecord)
    if request.history:
        lines.append(f"### Previous Attempts (last {len(request.history)} round(s))\n")
        for item in request.history:
            # Handle both HistoryRecord objects and plain dicts
            if isinstance(item, HistoryRecord):
                round_num = item.round
                summary = item.diagnosis_summary
                applied = item.suggestion_applied
                outcome = item.outcome
            else:
                round_num = item.get("round", "?")
                summary = item.get("diagnosis_summary", "N/A")
                applied = item.get("suggestion_applied", "N/A")
                outcome = item.get("outcome", "N/A")
            lines.append(f"Round {round_num}:")
            lines.append(f"- Diagnosis: {summary}")
            lines.append(f"- Applied: {applied}")
            lines.append(f"- Outcome: {outcome}")
        lines.append(
            "\n**Important:** Do NOT repeat any of the above suggestions. "
            "They have been tried and failed or did not fully resolve the issue.\n"
        )

    # Scenario-specific prompt augmentation (T5.1, T5.2)
    _append_scenario_hints(request, lines)

    return "\n".join(lines)


def _append_scenario_hints(request: AnalyzeRequest, lines: list):
    """Append scenario-specific prompt sections based on diagnostic content."""
    has_cost_reject = False
    has_align_unknown = False

    for diag in request.diagnostics:
        # T5.1: cost model rejection
        if diag.remark_id == "VectorizationNotBeneficial":
            has_cost_reject = True
        if diag.cost_model and diag.cost_model.scalar_cost > 0 and diag.cost_model.vector_cost > diag.cost_model.scalar_cost:
            has_cost_reject = True

        # T5.2: alignment unknown
        if diag.remark_id in ("CantReorderMemOps", "UnsafeDep"):
            if diag.memory_info and diag.memory_info.max_alignment <= 1:
                has_align_unknown = True
        if diag.memory_info and diag.memory_info.max_alignment == 0:
            has_align_unknown = True

    if has_cost_reject:
        lines.append("## Cost Model Rejection Guidance\n")
        for diag in request.diagnostics:
            if diag.cost_model:
                cm = diag.cost_model
                lines.append(_COST_REJECT_TEMPLATE.format(
                    scalar_cost=cm.scalar_cost,
                    vector_cost=cm.vector_cost,
                    vf=cm.vf or "N/A",
                    ratio=f"{cm.vector_cost / max(cm.scalar_cost, 1):.1f}",
                    interleave_count=cm.interleave_count,
                ))
                break
        lines.append("")

    if has_align_unknown:
        lines.append("## Memory Alignment Guidance\n")
        for diag in request.diagnostics:
            if diag.memory_info:
                mi = diag.memory_info
                lines.append(_ALIGN_TEMPLATE.format(
                    max_alignment=mi.max_alignment,
                    stride=mi.stride,
                    num_stores=mi.num_stores,
                    num_loads=mi.num_loads,
                    memory_check_count=mi.memory_check_count if mi.memory_check_count is not None else "N/A",
                    memory_check_cost=mi.memory_check_cost if mi.memory_check_cost is not None else "N/A",
                ))
                break
        lines.append("")

    return "\n".join(lines)
