# [AIMV] MCP Server — Prompt builder: AnalyzeRequest → LLM prompt
from .models import AnalyzeRequest

SYSTEM_PROMPT_TEMPLATE = """\
You are an expert compiler engineer specializing in automatic vectorization
of C/C++ code for embedded systems. You analyze LLVM optimization remarks
and suggest source-level fixes to enable loop vectorization.

## Your Task
Given:
1. The original C source code of a function containing a loop
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
4. If you cannot determine a safe fix, set no_action_possible: true.
5. Provide a valid unified diff in the diff field.
6. For {aimv_level} level, you may:
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


def build_system_prompt(request: AnalyzeRequest) -> str:
    return SYSTEM_PROMPT_TEMPLATE.format(
        triple=request.target.triple,
        cpu=request.target.cpu,
        features=", ".join(request.target.features),
        vector_width=request.target.vector_width,
        aimv_level=request.aimv_level.value,
        response_schema=RESPONSE_SCHEMA_JSON,
    )


def build_user_prompt(request: AnalyzeRequest) -> str:
    lines = []

    # Function context
    f = request.function
    lines.append(f"## Function Under Analysis\n")
    lines.append(f"**Name:** `{f.name}`")
    lines.append(f"**Signature:** `{f.signature}`")
    lines.append(f"**File:** `{f.source_file}`, line {f.loop_line}\n")
    lines.append("### Source Code\n")
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
        lines.append(f"**Severity:** {diag.severity.value}\n")

        if diag.ir_snippet:
            lines.append("#### LLVM IR\n")
            lines.append("```llvm")
            lines.append(diag.ir_snippet)
            lines.append("```\n")

        if diag.cost_model:
            cm = diag.cost_model
            lines.append("#### Cost Model Analysis\n")
            lines.append(f"| Metric | Value |")
            lines.append(f"|--------|-------|")
            lines.append(f"| Scalar cost | {cm.scalar_cost} |")
            lines.append(f"| Vector cost (VF={cm.vf}) | {cm.vector_cost} |")
            lines.append(f"| Interleave count | {cm.interleave_count} |")
            if cm.scalar_cost < cm.vector_cost:
                lines.append(f"\n**Key insight:** Vector cost ({cm.vector_cost}) > scalar cost ({cm.scalar_cost}). The cost model estimates vectorization is NOT profitable.\n")
        else:
            lines.append("Cost model data not available (legality stage rejection).\n")

        if diag.dependencies:
            lines.append("#### Memory Dependencies\n")
            lines.append("| # | Type | Source | Sink | Alias Result |")
            lines.append("|---|------|--------|------|-------------|")
            for j, dep in enumerate(diag.dependencies):
                lines.append(f"| {j + 1} | `{dep.dep_type}` | `{dep.source_ptr}` | `{dep.sink_ptr}` | {dep.alias_result} |")
            lines.append("")
        else:
            lines.append("No dependency information available.\n")

        if diag.memory_info:
            mi = diag.memory_info
            lines.append("#### Memory Access Pattern\n")
            lines.append(f"| Attribute | Value |")
            lines.append(f"|-----------|-------|")
            lines.append(f"| Stores / Loads | {mi.num_stores} / {mi.num_loads} |")
            lines.append(f"| Max alignment | {mi.max_alignment} bytes |")
            lines.append(f"| Stride | {mi.stride} |")
            lines.append(f"| Memory checks needed | {mi.memory_check_count} (cost: {mi.memory_check_cost}) |")
            lines.append("")

        if diag.loop_info:
            li = diag.loop_info
            trip = li.trip_count if li.trip_count >= 0 else "unknown"
            lines.append("#### Loop Structure\n")
            lines.append(f"| Blocks | Instructions | Trip count | Branches | Calls |")
            lines.append(f"|--------|-------------|------------|----------|-------|")
            lines.append(f"| {li.num_blocks} | {li.num_instructions} | {trip} | {li.num_branches} | {li.num_calls} |")
            lines.append("")

        lines.append("---\n")

    # History
    if request.history:
        lines.append("### Previous Attempts (This Session)\n")
        for item in request.history:
            lines.append(f"**Round {item.get('round', '?')}:**")
            lines.append(f"- Suggestion: {item.get('suggestion_description', 'unknown')}")
            lines.append(f"- Result: {item.get('result', 'unknown')}")
        lines.append("\n**Important:** Do NOT repeat any of the above suggestions. They have been tried and failed.\n")

    return "\n".join(lines)
