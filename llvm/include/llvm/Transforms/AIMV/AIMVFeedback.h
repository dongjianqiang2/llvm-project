// [AIMV] AIMVFeedbackPass — Function Pass for collecting vectorization diagnostics
// See aimv_design_doc/LLVM_DESIGN.md for full design.
#ifndef LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H
#define LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H

#include "llvm/IR/PassManager.h"
#include <string>
#include <vector>

namespace llvm {

/// [AIMV] Raw diagnostic record parsed from !aimv.diag Named Metadata.
/// Shared between AIMVDiagnosticAnalysis (Module Analysis) and
/// AIMVFeedbackPass (Function Pass).
struct AIMVDiagnostic {
  std::string PassName;        // "LoopVectorize" | "SLPVectorize"
  std::string RemarkID;        // "CantReorderMemOps" | ...
  std::string FunctionName;
  std::string SourceLocation;  // "file.c:42:5"
  std::string SourceAccuracy;  // "approximate" when !dbg degraded, else empty
  std::string RemarkMsg;
  int ScalarCost = -1, VectorCost = -1, VF = 0, IC = 0;
  struct DepEntry {
    std::string Type;  // LLVM Dependence::DepName[]: "Backward"|"Forward"|...
    std::string Source, Sink, AliasResult;
  };
  std::vector<DepEntry> Dependencies;
  int NumStores = 0, NumLoads = 0, NumPredStores = 0;
  int MaxAlignment = 0, MemCheckCount = 0, MemCheckCost = 0;
  std::string Stride = "unknown";
  int NumBlocks = 0, NumInsts = 0, TripCount = -1;
  int NumBranches = 0, NumCalls = 0;
};

/// [AIMV] Module Analysis that parses !aimv.diag Named Metadata once per Module.
/// Result is cached by the pass manager — subsequent function passes
/// in the same module reuse the parsed diagnostics.
class AIMVDiagnosticAnalysis : public AnalysisInfoMixin<AIMVDiagnosticAnalysis> {
  friend AnalysisInfoMixin<AIMVDiagnosticAnalysis>;
  static AnalysisKey Key;

public:
  using Result = std::vector<AIMVDiagnostic>;
  Result run(Module &M, ModuleAnalysisManager &);
  static StringRef name() { return "aimv-diagnostic"; }
};

/// [AIMV] AIMVFeedbackPass — collect vectorization diagnostics and export JSON
///
/// Type: Function Pass.
/// Activation: -mllvm -aimv-enable (via clang -faimv).
/// Input: reads !aimv.diag via AIMVDiagnosticAnalysis, filters by FunctionName.
/// Output: appends JSON to --aimv-output=<path>.
class AIMVFeedbackPass : public PassInfoMixin<AIMVFeedbackPass> {
public:
  void setOutputPath(const std::string &Path) { OutputPath = Path; }
  void setTargetFunction(const std::string &FuncName) { TargetFunction = FuncName; }
  void setEnabled(bool V = true) { EnabledFlag = V; }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static StringRef name() { return "aimv-feedback"; }

  // Compatibility alias for existing callers
  using RawDiagnostic = AIMVDiagnostic;

  static std::vector<AIMVDiagnostic> parseDiagnostics(Module &M);

private:
  std::string OutputPath, TargetFunction;
  bool EnabledFlag = false;
};

} // namespace llvm
#endif
