// [BiSheng] AIMVFeedbackPass — Function Pass implementation
// See aimv_design_doc/LLVM_DESIGN.md §3 for full design.
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
#include "llvm/Support/CommandLine.h"

// [BiSheng] Command-line flags for AIMV
// For use with opt: opt -passes=aimv-feedback -aimv-output=diag.json
static cl::opt<std::string> AIMVOutputPath(
    "aimv-output", cl::desc("AIMV JSON diagnostic output path"), cl::Hidden);
static cl::opt<bool> AIMVEnable(
    "aimv-enable", cl::desc("Enable AIMV diagnostic collection"), cl::Hidden);
static cl::opt<std::string> AIMVTargetFunction(
    "aimv-target-function", cl::desc("Only analyze the specified function"),
    cl::Hidden);
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <mutex>

using namespace llvm;

static std::mutex JSONWriteMutex;

namespace {

/// Infer severity from RemarkID
static std::string inferSeverity(const std::string &RemarkID) {
  if (RemarkID == "CantReorderMemOps" ||
      RemarkID == "VectorizationNotBeneficial" ||
      RemarkID == "UnsafeDep" ||
      RemarkID == "InterleavingNotBeneficial")
    return "missed";
  if (RemarkID == "LoopVectorized" ||
      RemarkID.find("Passed") != std::string::npos)
    return "passed";
  return "analysis";
}

/// Build a JSON object from a RawDiagnostic
static json::Object buildDiagnosticJSON(
    const AIMVFeedbackPass::RawDiagnostic &R, Function &F) {
  json::Object Diag;
  Diag["pass_name"] = R.PassName;
  Diag["remark_id"] = R.RemarkID;
  Diag["remark_text"] = R.RemarkMsg;
  Diag["severity"] = inferSeverity(R.RemarkID);
  Diag["function_name"] = R.FunctionName;
  Diag["loop_location"] = R.SourceLocation;

  // source_context: extract from IR debug info if available
  Diag["source_context"] = "";
  Diag["ir_snippet"] = "";

  // cost_model
  json::Object Cost;
  Cost["scalar_cost"] = R.ScalarCost;
  Cost["vector_cost"] = R.VectorCost;
  Cost["vf"] = R.VF;
  Cost["interleave_count"] = R.IC;
  Diag["cost_model"] = std::move(Cost);

  // dependencies
  json::Array Deps;
  for (auto &D : R.Dependencies) {
    json::Object Dep;
    Dep["dep_type"] = D.Type;
    Dep["source_ptr"] = D.Source;
    Dep["sink_ptr"] = D.Sink;
    Dep["alias_result"] = D.AliasResult;
    Deps.push_back(std::move(Dep));
  }
  Diag["dependencies"] = std::move(Deps);

  // memory_info
  json::Object Mem;
  Mem["num_stores"] = R.NumStores;
  Mem["num_loads"] = R.NumLoads;
  Mem["num_pred_stores"] = R.NumPredStores;
  Mem["max_alignment"] = R.MaxAlignment;
  Mem["stride"] = R.Stride;
  Mem["memory_check_count"] = R.MemCheckCount;
  Mem["memory_check_cost"] = R.MemCheckCost;
  Diag["memory_info"] = std::move(Mem);

  // loop_info
  json::Object Loop;
  Loop["num_blocks"] = R.NumBlocks;
  Loop["num_instructions"] = R.NumInsts;
  Loop["trip_count"] = R.TripCount;
  Loop["num_branches"] = R.NumBranches;
  Loop["num_calls"] = R.NumCalls;
  Diag["loop_info"] = std::move(Loop);

  return Diag;
}

} // anonymous namespace

PreservedAnalyses AIMVFeedbackPass::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();

  // Populate pass parameters from cl::opt (for opt-based testing)
  // In clang-based usage, BackendUtil.cpp calls setOutputPath/setEnabled directly.
  if (OutputPath.empty() && !AIMVOutputPath.empty())
    OutputPath = AIMVOutputPath;
  if (!EnabledFlag && AIMVEnable)
    EnabledFlag = true;
  if (TargetFunction.empty() && !AIMVTargetFunction.empty())
    TargetFunction = AIMVTargetFunction;

  // Activation check: remark streamer, OutputPath, or explicit enable
  if (!M->getContext().getLLVMRemarkStreamer() &&
      OutputPath.empty() && !EnabledFlag)
    return PreservedAnalyses::all();

  // Parse !aimv.diag (with caching for multi-function modules)
  // Static cache: reuse parsed diagnostics across functions in same pipeline run
  static Module *CachedModule = nullptr;
  static std::unique_ptr<std::vector<RawDiagnostic>> CachedDiags;
  if (M != CachedModule) {
    auto parsed = parseDiagnostics(*M);
    CachedDiags = std::make_unique<std::vector<RawDiagnostic>>(std::move(parsed));
    CachedModule = M;
  }

  if (!CachedDiags || CachedDiags->empty())
    return PreservedAnalyses::all();

  // Filter diagnostics for the current function
  std::vector<RawDiagnostic> FuncDiags;
  for (auto &R : *CachedDiags) {
    if (!TargetFunction.empty() && R.FunctionName != TargetFunction)
      continue;
    if (R.FunctionName != F.getName().str())
      continue;
    FuncDiags.push_back(R);
  }

  if (FuncDiags.empty() && !OutputPath.empty()) {
    // Explicit output requested but no diagnostics for this function — skip
    return PreservedAnalyses::all();
  }

  // Get TTI
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);

  // Build target info
  json::Object Target;
  Target["triple"] = M->getTargetTriple();
  Target["cpu"] = M->getTargetCPU();
  Target["features"] = json::Array();
  Target["vector_width"] =
      (int)TTI.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector)
          .value_or(128);

  // Build diagnostics array
  json::Array DiagArray;
  for (auto &R : FuncDiags)
    DiagArray.push_back(buildDiagnosticJSON(R, F));

  // Build JSON document
  json::Object Doc;
  Doc["request_id"] = "aimv-" + M->getModuleIdentifier() + "-" +
                       std::to_string(std::hash<std::string>{}(F.getName().str()));
  Doc["target"] = std::move(Target);
  Doc["diagnostics"] = std::move(DiagArray);

  // Serialize and write to OutputPath
  std::string JsonStr;
  raw_string_ostream OS(JsonStr);
  json::OStream JOS(OS, 2);
  JOS.value(json::Value(std::move(Doc)));
  OS.flush();

  {
    std::lock_guard<std::mutex> lock(JSONWriteMutex);
    std::ofstream Out(OutputPath, std::ios::app);
    if (Out.is_open()) {
      Out << JsonStr;
      // Multiple functions: separate with newline for JSON Lines
      Out << "\n";
    }
  }

  return PreservedAnalyses::all();
}
