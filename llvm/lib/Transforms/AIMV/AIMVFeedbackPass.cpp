// [AIMV] AIMVFeedbackPass — Function Pass implementation
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
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

static cl::opt<std::string> AIMVOutputPath(
    "aimv-output", cl::desc("AIMV JSON output path"), cl::Hidden);
static cl::opt<bool> AIMVEnable(
    "aimv-enable", cl::desc("Enable AIMV diagnostics"), cl::Hidden);
static cl::opt<std::string> AIMVTargetFunction(
    "aimv-target-function", cl::desc("Only analyze target function"), cl::Hidden);

// [AIMV] Parse "file.c:42:5" or "file.c:42" into (filename, line, col).
// Returns true on success.
static bool parseSourceLocation(StringRef SrcLoc, std::string &File,
                                unsigned &Line, unsigned &Col) {
  if (SrcLoc.empty() || SrcLoc == "unknown") return false;
  // Find last ':' for column, then second-last for line
  size_t ColPos = SrcLoc.rfind(':');
  if (ColPos == StringRef::npos) return false;
  size_t LinePos = SrcLoc.rfind(':', ColPos - 1);
  if (LinePos == StringRef::npos) return false;
  File = SrcLoc.substr(0, LinePos).str();
  if (File.empty()) return false;
  SrcLoc.substr(LinePos + 1, ColPos - LinePos - 1).getAsInteger(10, Line);
  SrcLoc.substr(ColPos + 1).getAsInteger(10, Col);
  return true;
}

// [AIMV] Parse "file.c:42" (no column) into (filename, line).
static bool parseSourceLocationNoCol(StringRef SrcLoc, std::string &File,
                                     unsigned &Line) {
  if (SrcLoc.empty() || SrcLoc == "unknown") return false;
  size_t ColPos = SrcLoc.rfind(':');
  if (ColPos == StringRef::npos) return false;
  File = SrcLoc.substr(0, ColPos).str();
  if (File.empty()) return false;
  SrcLoc.substr(ColPos + 1).getAsInteger(10, Line);
  return true;
}

// [AIMV] Extract source context (±3 lines around target) from source file.
static std::string extractSourceContext(const std::string &SourceLocation,
                                        std::string &SourceAccuracy) {
  if (SourceLocation.empty() || SourceLocation == "unknown") {
    SourceAccuracy = "approximate";
    return "";
  }

  std::string File;
  unsigned Line = 0, Col = 0;
  bool hasCol = parseSourceLocation(SourceLocation, File, Line, Col);
  if (!hasCol) {
    // Try without column (DISubprogram fallback: "file.c:42")
    if (!parseSourceLocationNoCol(SourceLocation, File, Line)) {
      SourceAccuracy = "approximate";
      return "";
    }
    SourceAccuracy = "approximate";
  }

  if (Line == 0) {
    SourceAccuracy = "approximate";
    return "";
  }

  std::ifstream In(File);
  if (!In.is_open()) {
    SourceAccuracy = "approximate";
    return "";
  }

  unsigned StartLine = (Line > 3) ? Line - 3 : 1;
  unsigned EndLine = Line + 3;
  std::string Result;
  std::string CurrentLine;
  unsigned LineNo = 0;
  while (std::getline(In, CurrentLine) && LineNo < EndLine) {
    LineNo++;
    if (LineNo >= StartLine) {
      Result += std::to_string(LineNo) + ": " + CurrentLine + "\n";
    }
  }

  return Result;
}

// [AIMV] Find the Loop* in LoopInfo that best matches a diagnostic.
// Matches by comparing source locations from loop header DebugLoc.
static Loop *findMatchingLoop(LoopInfo &LI, Function &F,
                              const AIMVDiagnostic &R) {
  std::string DiagFile, DiagFunc;
  unsigned DiagLine = 0, DiagCol = 0;
  bool DiagHasCol = parseSourceLocation(R.SourceLocation, DiagFile, DiagLine, DiagCol);

  Loop *BestMatch = nullptr;
  unsigned BestDist = ~0u;

  for (Loop *L : LI) {
    BasicBlock *Header = L->getHeader();
    DebugLoc DL = Header->getFirstNonPHIIt() != Header->end()
                      ? Header->getFirstNonPHIIt()->getDebugLoc()
                      : DebugLoc();
    if (!DL) {
      // Try DISubprogram fallback
      if (DISubprogram *SP = F.getSubprogram()) {
        std::string LoopFile = SP->getFilename().str();
        unsigned LoopLine = SP->getLine();
        if (DiagHasCol && LoopFile == DiagFile) {
          unsigned Dist = (LoopLine > DiagLine) ? LoopLine - DiagLine : DiagLine - LoopLine;
          if (Dist < BestDist) { BestDist = Dist; BestMatch = L; }
        }
      }
      continue;
    }
    DILocation *DIL = DL.get();
    std::string LoopFile = DIL->getFilename().str();
    unsigned LoopLine = DIL->getLine();
    if (DiagHasCol && LoopFile == DiagFile) {
      unsigned Dist = (LoopLine > DiagLine) ? LoopLine - DiagLine : DiagLine - LoopLine;
      if (Dist < BestDist) { BestDist = Dist; BestMatch = L; }
    }
  }

  return BestMatch;
}

// [AIMV] Extract IR snippet from loop body, truncated to ~1000 chars.
static std::string extractIRSnippet(Loop *L) {
  if (!L) return "";

  std::string IR;
  raw_string_ostream OS(IR);
  for (BasicBlock *BB : L->blocks()) {
    BB->print(OS);
  }

  if (IR.size() > 1000) {
    IR = IR.substr(0, 997) + "...";
  }
  return IR;
}

namespace {

static std::string inferSeverity(const std::string &RemarkID) {
  if (RemarkID == "CantReorderMemOps" || RemarkID == "VectorizationNotBeneficial" ||
      RemarkID == "UnsafeDep" || RemarkID == "InterleavingNotBeneficial")
    return "missed";
  if (RemarkID == "LoopVectorized" || RemarkID.find("Passed") != std::string::npos)
    return "passed";
  return "analysis";
}

static json::Object buildDiagnosticJSON(const AIMVDiagnostic &R,
                                         Loop *L) {
  json::Object Diag;
  Diag["pass_name"] = R.PassName;
  Diag["remark_id"] = R.RemarkID;
  Diag["remark_text"] = R.RemarkMsg;
  Diag["severity"] = inferSeverity(R.RemarkID);
  Diag["function_name"] = R.FunctionName;
  Diag["loop_location"] = R.SourceLocation;

  std::string SourceAccuracy = R.SourceAccuracy;
  Diag["source_context"] = extractSourceContext(R.SourceLocation, SourceAccuracy);
  Diag["ir_snippet"] = extractIRSnippet(L);
  if (!SourceAccuracy.empty()) Diag["source_accuracy"] = SourceAccuracy;

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
  Mem["num_stores"] = R.NumStores; Mem["num_loads"] = R.NumLoads;
  Mem["num_pred_stores"] = R.NumPredStores; Mem["max_alignment"] = R.MaxAlignment;
  Mem["stride"] = R.Stride; Mem["memory_check_count"] = R.MemCheckCount;
  Mem["memory_check_cost"] = R.MemCheckCost;
  Diag["memory_info"] = std::move(Mem);

  json::Object Loop;
  Loop["num_blocks"] = R.NumBlocks; Loop["num_instructions"] = R.NumInsts;
  Loop["trip_count"] = R.TripCount; Loop["num_branches"] = R.NumBranches;
  Loop["num_calls"] = R.NumCalls;
  Diag["loop_info"] = std::move(Loop);

  return Diag;
}

} // anonymous namespace

// [AIMV] Module Analysis: parse !aimv.diag once per Module.
// Result is cached by the pass manager for all function passes in the module.
AnalysisKey AIMVDiagnosticAnalysis::Key;

AIMVDiagnosticAnalysis::Result
AIMVDiagnosticAnalysis::run(Module &M, ModuleAnalysisManager &) {
  return AIMVFeedbackPass::parseDiagnostics(M);
}

PreservedAnalyses AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M) return PreservedAnalyses::all();

  // Populate from cl::opt
  if (OutputPath.empty() && !AIMVOutputPath.empty()) OutputPath = AIMVOutputPath;
  if (!EnabledFlag && AIMVEnable) EnabledFlag = true;
  if (TargetFunction.empty() && !AIMVTargetFunction.empty()) TargetFunction = AIMVTargetFunction;

  // Activation: EnabledFlag + OutputPath must both be set
  if (!EnabledFlag || OutputPath.empty())
    return PreservedAnalyses::all();

  // Get diagnostics via ModuleAnalysisManager proxy when available
  // (proper pipeline), or parse directly when running standalone.
  const std::vector<AIMVDiagnostic> *DiagsPtr = nullptr;
  std::vector<AIMVDiagnostic> StandaloneDiags;
  auto *Proxy =
      AM.getCachedResult<ModuleAnalysisManagerFunctionProxy>(F);
  if (Proxy) {
    DiagsPtr = Proxy->getCachedResult<AIMVDiagnosticAnalysis>(*M);
  }
  if (!DiagsPtr) {
    // Standalone mode: parse directly (no wrapping FPM to pre-run the analysis).
    // Track !aimv.diag operand count to invalidate cache when prior passes
    // (SLP, LoopUnroll, etc.) add diagnostics between function invocations.
    static Module *CachedModule = nullptr;
    static std::unique_ptr<std::vector<AIMVDiagnostic>> CachedStandalone;
    static unsigned CachedDiagCount = 0;
    NamedMDNode *NMD = M->getNamedMetadata("aimv.diag");
    unsigned CurrentCount = NMD ? NMD->getNumOperands() : 0;
    if (M != CachedModule || CurrentCount != CachedDiagCount) {
      CachedStandalone = std::make_unique<std::vector<AIMVDiagnostic>>(
          parseDiagnostics(*M));
      CachedModule = M;
      CachedDiagCount = CurrentCount;
    }
    DiagsPtr = CachedStandalone.get();
  }

  if (!DiagsPtr || DiagsPtr->empty())
    return PreservedAnalyses::all();

  // Filter for current function
  std::vector<AIMVDiagnostic> FuncDiags;
  for (auto &R : *DiagsPtr) {
    if (!TargetFunction.empty() && R.FunctionName != TargetFunction) continue;
    if (R.FunctionName != F.getName().str()) continue;
    FuncDiags.push_back(R);
  }
  if (FuncDiags.empty()) return PreservedAnalyses::all();

  // Build JSON
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  auto &LI = AM.getResult<LoopAnalysis>(F);
  // [AIMV] Extract target CPU and features from function attributes.
  // These are set by clang -mcpu= / -mattr= / --target= flags.
  std::string TargetCPU, TargetFeatures;
  if (auto A = F.getFnAttribute("target-cpu"); A.isValid())
    TargetCPU = A.getValueAsString().str();
  if (auto A = F.getFnAttribute("target-features"); A.isValid())
    TargetFeatures = A.getValueAsString().str();

  json::Object Target;
  Target["triple"] = M->getTargetTriple().str();
  Target["cpu"] = TargetCPU;
  json::Array FeaturesArray;
  // target-features is a comma-separated string like "+neon,-fp-armv8"
  if (!TargetFeatures.empty()) {
    SmallVector<StringRef, 8> FeatureList;
    StringRef(TargetFeatures).split(FeatureList, ',');
    for (auto &Ftr : FeatureList)
      FeaturesArray.push_back(Ftr.trim().str());
  }
  Target["features"] = std::move(FeaturesArray);
  Target["vector_width"] = (int)TTI.getRegisterBitWidth(
      TargetTransformInfo::RGK_FixedWidthVector).getFixedValue();

  json::Array DiagArray;
  for (auto &R : FuncDiags) {
    Loop *L = findMatchingLoop(LI, F, R);
    DiagArray.push_back(buildDiagnosticJSON(R, L));
  }

  json::Object Doc;
  Doc["request_id"] = "aimv-" + M->getModuleIdentifier() + "-" +
      std::to_string(std::hash<std::string>{}(F.getName().str()));
  Doc["target"] = std::move(Target);
  Doc["diagnostics"] = std::move(DiagArray);

  std::string JsonStr;
  { raw_string_ostream OS(JsonStr); OS << json::Value(std::move(Doc)); }

  {
    std::lock_guard<std::mutex> lock(JSONWriteMutex);
    std::ofstream Out(OutputPath, std::ios::app);
    if (Out.is_open()) Out << JsonStr << "\n";
  }

  return PreservedAnalyses::all();
}
