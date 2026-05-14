// [AIMV] AIMVFeedbackPass — Function Pass for collecting vectorization diagnostics
// See aimv_design_doc/LLVM_DESIGN.md §3 for full design.
#ifndef LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H
#define LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H

#include "llvm/IR/PassManager.h"
#include <string>
#include <vector>

namespace llvm {

/// [AIMV] AIMVFeedbackPass — collect vectorization diagnostics and export JSON
///
/// Type: Function Pass (processes !aimv.diag diagnostics for each function)
/// Timing: runs after LoopVectorize + SLPVectorize
/// Input: reads !aimv.diag Named Metadata via F.getParent(), filters by FunctionName
/// Output: appends JSON to --aimv-output=<path>
class AIMVFeedbackPass : public PassInfoMixin<AIMVFeedbackPass> {
public:
  /// [AIMV] Set JSON output file path
  void setOutputPath(const std::string &Path) { OutputPath = Path; }

  /// [AIMV] Set target function filter (empty = all functions)
  void setTargetFunction(const std::string &FuncName) {
    TargetFunction = FuncName;
  }

  /// [AIMV] Explicit enable (run even without remark streamer)
  void setEnabled(bool V = true) { EnabledFlag = V; }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static StringRef name() { return "aimv-feedback"; }

  /// [AIMV] Parsed diagnostic record from !aimv.diag Named Metadata
  struct RawDiagnostic {
    std::string PassName;        // "LoopVectorize" | "SLPVectorize"
    std::string RemarkID;        // "CantReorderMemOps" | ...
    std::string FunctionName;
    std::string SourceLocation;  // "file.c:42:5"
    std::string RemarkMsg;
    // Cost
    int ScalarCost = -1;
    int VectorCost = -1;
    int VF = 0;
    int IC = 0;
    // Dependencies
    struct DepEntry {
      std::string Type;  // LLVM Dependence::DepName[]: "Backward"|"Forward"|...
      std::string Source;
      std::string Sink;
      std::string AliasResult;
    };
    std::vector<DepEntry> Dependencies;
    // Memory
    int NumStores = 0, NumLoads = 0, NumPredStores = 0;
    int MaxAlignment = 0;
    std::string Stride = "unknown";
    int MemCheckCount = 0, MemCheckCost = 0;
    // Loop
    int NumBlocks = 0, NumInsts = 0, TripCount = -1;
    int NumBranches = 0, NumCalls = 0;
  };

  /// [AIMV] Parse !aimv.diag Named Metadata into RawDiagnostic vector
  /// @return empty vector if no diagnostics found
  static std::vector<RawDiagnostic> parseDiagnostics(Module &M);

private:
  std::string OutputPath;
  std::string TargetFunction;
  bool EnabledFlag = false;  // set by -aimv-enable
};

} // namespace llvm

#endif
