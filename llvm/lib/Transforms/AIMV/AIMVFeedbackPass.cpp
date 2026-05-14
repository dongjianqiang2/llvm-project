// [AIMV] AIMVFeedbackPass — Function Pass implementation
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <mutex>

using namespace llvm;

static std::mutex JSONWriteMutex;

// [AIMV] Command-line flags (opt: -aimv-output=diag.json -aimv-enable)
static cl::opt<std::string> AIMVOutputPath(
    "aimv-output", cl::desc("AIMV JSON diagnostic output path"), cl::Hidden);
static cl::opt<bool> AIMVEnable(
    "aimv-enable", cl::desc("Enable AIMV diagnostic collection"), cl::Hidden);
static cl::opt<std::string> AIMVTargetFunction(
    "aimv-target-function", cl::desc("Only analyze the specified function"),
    cl::Hidden);

namespace {

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

static json::Object buildDiagnosticJSON(
    const AIMVFeedbackPass::RawDiagnostic &R) {
  json::Object Diag;
  Diag["pass_name"] = R.PassName;
  Diag["remark_id"] = R.RemarkID;
  Diag["remark_text"] = R.RemarkMsg;
  Diag["severity"] = inferSeverity(R.RemarkID);
  Diag["function_name"] = R.FunctionName;
  Diag["loop_location"] = R.SourceLocation;
  Diag["source_context"] = "";
  Diag["ir_snippet"] = "";

  json::Object Cost;
  Cost["scalar_cost"] = R.ScalarCost;
  Cost["vector_cost"] = R.VectorCost;
  Cost["vf"] = R.VF;
  Cost["interleave_count"] = R.IC;
  Diag["cost_model"] = std::move(Cost);

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

  json::Object Mem;
  Mem["num_stores"] = R.NumStores;
  Mem["num_loads"] = R.NumLoads;
  Mem["num_pred_stores"] = R.NumPredStores;
  Mem["max_alignment"] = R.MaxAlignment;
  Mem["stride"] = R.Stride;
  Mem["memory_check_count"] = R.MemCheckCount;
  Mem["memory_check_cost"] = R.MemCheckCost;
  Diag["memory_info"] = std::move(Mem);

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

  if (OutputPath.empty() && !AIMVOutputPath.empty())
    OutputPath = AIMVOutputPath;
  if (!EnabledFlag && AIMVEnable)
    EnabledFlag = true;
  if (TargetFunction.empty() && !AIMVTargetFunction.empty())
    TargetFunction = AIMVTargetFunction;

  if (!M->getContext().getLLVMRemarkStreamer() &&
      OutputPath.empty() && !EnabledFlag)
    return PreservedAnalyses::all();

  static Module *CachedModule = nullptr;
  static std::unique_ptr<std::vector<RawDiagnostic>> CachedDiags;
  if (M != CachedModule) {
    auto parsed = parseDiagnostics(*M);
    CachedDiags = std::make_unique<std::vector<RawDiagnostic>>(std::move(parsed));
    CachedModule = M;
  }

  if (!CachedDiags || CachedDiags->empty())
    return PreservedAnalyses::all();

  std::vector<RawDiagnostic> FuncDiags;
  for (auto &R : *CachedDiags) {
    if (!TargetFunction.empty() && R.FunctionName != TargetFunction)
      continue;
    if (R.FunctionName != F.getName().str())
      continue;
    FuncDiags.push_back(R);
  }

  if (FuncDiags.empty())
    return PreservedAnalyses::all();

  auto &TTI = AM.getResult<TargetIRAnalysis>(F);

  json::Object Target;
  Target["triple"] = M->getTargetTriple().str();
  Target["cpu"] = "";
  Target["features"] = json::Array();
  Target["vector_width"] =
      (int)TTI.getRegisterBitWidth(
               TargetTransformInfo::RGK_FixedWidthVector).getFixedValue();

  json::Array DiagArray;
  for (auto &R : FuncDiags)
    DiagArray.push_back(buildDiagnosticJSON(R));

  json::Object Doc;
  Doc["request_id"] = "aimv-" + M->getModuleIdentifier() + "-" +
      std::to_string(std::hash<std::string>{}(F.getName().str()));
  Doc["target"] = std::move(Target);
  Doc["diagnostics"] = std::move(DiagArray);

  std::string JsonStr;
  {
    raw_string_ostream OS(JsonStr);
    OS << json::Value(std::move(Doc));
  } // OS flushes on destruction

  if (!OutputPath.empty()) {
    std::lock_guard<std::mutex> lock(JSONWriteMutex);
    std::ofstream Out(OutputPath, std::ios::app);
    if (Out.is_open())
      Out << JsonStr << "\n";
  }

  return PreservedAnalyses::all();
}
