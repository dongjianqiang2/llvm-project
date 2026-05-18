// [AIMV] emitAIMVDiagnostic() implementation.
// Writes structured vectorization diagnostics into !aimv.diag Named Metadata.
#include "llvm/Analysis/AIMVDiagnostic.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

void llvm::emitAIMVDiagnostic(
    Module &M, Function &F, Loop &L,
    const LoopAccessInfo *LAI,
    const AIMVCostSnapshot &Cost,
    StringRef RemarkID, StringRef RemarkMsg,
    ScalarEvolution *SE,
    int RtCheckCost,
    int RtCheckCount) {

  LLVMContext &Ctx = M.getContext();

  // Build source_location with fallback chain:
  //   1. DILocation (precise)
  //   2. DISubprogram (function-level)
  //   3. "unknown"
  DebugLoc StartLoc = L.getStartLoc();
  std::string SrcLoc;
  if (StartLoc) {
    DILocation *DIL = StartLoc.get();
    raw_string_ostream(SrcLoc) << DIL->getFilename() << ":"
                               << DIL->getLine() << ":" << DIL->getColumn();
  } else if (DISubprogram *SP = F.getSubprogram()) {
    raw_string_ostream(SrcLoc) << SP->getFilename() << ":" << SP->getLine();
  } else {
    SrcLoc = "unknown";
  }

  // --- cost_data (4 x i32) from pre-computed snapshot ---
  SmallVector<Metadata *> CostOps;
  CostOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Cost.ScalarCost)));
  CostOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Cost.VectorCost)));
  CostOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Cost.VF)));
  CostOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Cost.IC)));

  // --- dep_data (count + DepEntries) ---
  SmallVector<Metadata *> DepOps;
  if (LAI) {
    const MemoryDepChecker &DepChecker = LAI->getDepChecker();
    const auto *Deps = DepChecker.getDependences();
    unsigned DepCount = 0;
    SmallVector<Metadata *> DepEntries;
    if (Deps) {
      for (auto &Dep : *Deps) {
        if (Dep.Type == MemoryDepChecker::Dependence::NoDep) continue;
        const char *DepTypeStr =
            MemoryDepChecker::Dependence::DepName[Dep.Type];
        SmallVector<Metadata *> DepEntry;
        DepEntry.push_back(MDString::get(Ctx, DepTypeStr));
        auto getInstDesc = [](Instruction *I) -> std::string {
          if (!I) return "null";
          if (I->hasName()) return I->getName().str();
          return I->getOpcodeName();
        };
        DepEntry.push_back(MDString::get(Ctx,
            getInstDesc(Dep.getSource(DepChecker))));
        DepEntry.push_back(MDString::get(Ctx,
            getInstDesc(Dep.getDestination(DepChecker))));
        using SafetyStatus = MemoryDepChecker::VectorizationSafetyStatus;
        const char *SafetyStr;
        switch (MemoryDepChecker::Dependence::isSafeForVectorization(Dep.Type)) {
        case SafetyStatus::Safe:
          SafetyStr = "safe for vectorization"; break;
        case SafetyStatus::PossiblySafeWithRtChecks:
          SafetyStr = "possibly safe with runtime checks"; break;
        case SafetyStatus::Unsafe:
          SafetyStr = "unsafe: prevents vectorization"; break;
        }
        DepEntry.push_back(MDString::get(Ctx, SafetyStr));
        DepEntries.push_back(MDNode::get(Ctx, DepEntry));
        DepCount++;
      }
    }
    DepOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), DepCount)));
    for (auto *E : DepEntries) DepOps.push_back(E);
  } else {
    DepOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), 0)));
  }

  // --- memory_data (7 values) ---
  SmallVector<Metadata *> MemOps;
  if (LAI) {
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), LAI->getNumStores())));
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), LAI->getNumLoads())));
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), 0))); // num_pred_stores
    unsigned MaxAlign = 0;
    for (BasicBlock *BB : L.blocks())
      for (Instruction &I : *BB)
        if (auto *LI = dyn_cast<LoadInst>(&I))
          MaxAlign = std::max(MaxAlign, (unsigned)LI->getAlign().value());
        else if (auto *SI = dyn_cast<StoreInst>(&I))
          MaxAlign = std::max(MaxAlign, (unsigned)SI->getAlign().value());
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), MaxAlign)));
    bool Stride1 = (RtCheckCount == 0) &&
                   (LAI->getNumStores() + LAI->getNumLoads() > 0);
    MemOps.push_back(MDString::get(Ctx, Stride1 ? "stride=1" : "non-constant"));
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), RtCheckCount)));
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), RtCheckCost)));
  } else {
    for (int i = 0; i < 7; i++)
      MemOps.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), -1)));
  }

  // --- loop_info (loop_name + 5 x i32) ---
  SmallVector<Metadata *> LoopOps;
  LoopOps.push_back(MDString::get(Ctx, L.getName()));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), L.getNumBlocks())));
  unsigned NumInsts = 0, NumBranches = 0, NumCalls = 0;
  for (BasicBlock *BB : L.blocks()) {
    NumInsts += BB->size();
    for (Instruction &I : *BB) {
      if (isa<BranchInst>(&I) || isa<SwitchInst>(&I)) NumBranches++;
      if (isa<CallBase>(&I)) NumCalls++;
    }
  }
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumInsts)));
  int TripCount = SE ? (int)SE->getSmallConstantTripCount(&L) : -1;
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), TripCount)));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumBranches)));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumCalls)));

  // Assemble 9-operand MDNode
  MDNode *DiagNode = MDNode::get(Ctx, {
      MDString::get(Ctx, "LoopVectorize"),   // [0] PassName
      MDString::get(Ctx, RemarkID),          // [1] RemarkID
      MDString::get(Ctx, F.getName()),       // [2] FunctionName
      MDString::get(Ctx, SrcLoc),            // [3] SourceLocation
      MDString::get(Ctx, RemarkMsg),         // [4] RemarkMsg
      MDNode::get(Ctx, CostOps),             // [5] cost_data
      MDNode::get(Ctx, DepOps),              // [6] dep_data
      MDNode::get(Ctx, MemOps),              // [7] memory_data
      MDNode::get(Ctx, LoopOps)              // [8] loop_info
  });

  NamedMDNode *NMD = M.getOrInsertNamedMetadata("aimv.diag");
  NMD->addOperand(DiagNode);
}
