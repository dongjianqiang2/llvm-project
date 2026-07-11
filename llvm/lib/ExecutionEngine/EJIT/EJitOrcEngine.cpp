//===-- EJitOrcEngine.cpp - OrcJIT Engine Wrapper -------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitLinkDiagPlugin.h"
#include "llvm/ExecutionEngine/EJIT/EJitLinkOptimizationPlugin.h"
#include "llvm/ExecutionEngine/EJIT/EJitLibcallStubs.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#ifdef EJIT_FREESTANDING
extern "C" uint32_t SRE_TaskDelay(uint32_t tick);
#endif

#ifdef EJIT_FREESTANDING
#include "llvm/ExecutionEngine/EJIT/EJitBareMetal.h"
#else
#include <mutex>
#endif

#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPoolState.h"
#endif

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-orc-engine"

static const GlobalVariable *rootGlobal(Value *V) {
  while (V) {
    V = V->stripPointerCasts();
    if (auto *GV = dyn_cast<GlobalVariable>(V))
      return GV;
    auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      return nullptr;
    V = GEP->getPointerOperand();
  }
  return nullptr;
}

static void collectReferencedExternalDecls(
    Module &M, SmallPtrSetImpl<const Function *> &Funcs,
    SmallPtrSetImpl<const GlobalVariable *> &Globals) {
  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        for (Use &U : I.operands()) {
          Value *Op = U.get();
          if (auto *Callee = dyn_cast<Function>(Op->stripPointerCasts()))
            if (Callee->isDeclaration() && !Callee->isIntrinsic())
              Funcs.insert(Callee);
          if (auto *GV = rootGlobal(Op))
            if (GV->isDeclaration() && !GV->getName().empty())
              Globals.insert(GV);
        }
      }
    }
  }
}

struct EJitOrcEngine::Impl {
#ifdef EJIT_SRE_CODE_POOL
  /// Dedicated 2MiB code pools backing all JIT machine code. Declared before
  /// J so it outlives the LLJIT (and the memory manager the object linking
  /// layer owns, which references it).
  std::unique_ptr<EJitCodePoolManager> codePool;
#endif
  std::unique_ptr<orc::LLJIT> J;
  PeriodArrayRegistry *periodReg = nullptr;
  EJitRuntimeState *runtimeState = nullptr;
  const SpecializationContext *activeCtx = nullptr;
  /// Per-specialization JITDylib pointers so each specialization is
  /// independently compiled and symbols from different specializations
  /// never conflict.
  std::map<uint64_t, orc::JITDylib *> specDylibs;
  /// User-registered symbols (functions + globals) for bare-metal.
  /// Populated via ejit_register_symbol() / addUserSymbol().
  std::map<std::string, void *> userSymbols;
  /// If non-empty, dump JIT-optimized IR to this directory.
  std::string dumpJITDir;
  /// Persistent optimizer — analysis managers are registered once and reused.
  std::unique_ptr<EJitOptimizer> optimizer;
  /// TargetMachine used for the name-filtered ASM diagnostic dump (created
  /// once from the same JITTargetMachineBuilder the JIT compiles with, so the
  /// emitted assembly matches the real JIT output). Null if creation failed.
  std::unique_ptr<TargetMachine> dumpTM;
};

namespace llvm {
namespace ejit {

// Mutex type for the dump store. On SRE/freestanding std::mutex is
// unavailable and BareMetalMutex is a no-op, so use a real CAS spinlock (built
// on the __atomic wrappers in EJitAtomic.h). gDumpStore is per-core (each core
// has its own process image, so there is no cross-core race on it), but a
// same-core overlap between the worker capture and a producer print must still
// be guarded. Hosted builds keep std::mutex. The spinlock has a trivial
// default constructor, so a static instance is zero-initialized (unlocked)
// with no dynamic initializer — important for freestanding.
#ifdef EJIT_FREESTANDING
namespace {
class DumpSpinLock {
public:
  void lock() {
    uint32_t expected = 0;
    while (!flag_.compareExchange(expected, 1u))
      expected = 0;
  }
  void unlock() { flag_.storeRelease(0u); }

private:
  EJitAtomicU32 flag_;
};
} // namespace
using DumpMutexType = DumpSpinLock;
#else
using DumpMutexType = std::mutex;
#endif

#ifndef EJIT_DUMP_PRINT_THROTTLE_LINES
#define EJIT_DUMP_PRINT_THROTTLE_LINES 16
#endif

#ifndef EJIT_DUMP_PRINT_THROTTLE_TICKS
#define EJIT_DUMP_PRINT_THROTTLE_TICKS 50
#endif

static void throttleDumpPrint(unsigned printedLines) {
#if defined(EJIT_FREESTANDING) && EJIT_DUMP_PRINT_THROTTLE_LINES > 0 &&        \
    EJIT_DUMP_PRINT_THROTTLE_TICKS > 0
  if (printedLines != 0 &&
      (printedLines % EJIT_DUMP_PRINT_THROTTLE_LINES) == 0)
    (void)SRE_TaskDelay(EJIT_DUMP_PRINT_THROTTLE_TICKS);
#else
  (void)printedLines;
#endif
}

// Process-wide function-name filter and payload store. The mutex protects both
// because the shell may update/print while the worker captures.
static DumpMutexType gDumpMutex;
#ifdef EJIT_SRE_SHARED_TASKPOOL
EJitSharedTaskPoolState *gDumpSharedState = nullptr;
#endif
static std::string gDumpFuncFilter;

void setDumpFuncFilter(const std::string &name) {
  {
    std::lock_guard<DumpMutexType> lock(gDumpMutex);
    gDumpFuncFilter = name;
    EJIT_DIAG_DEBUG("set_dump_filter value=%s &filter=%p",
                    gDumpFuncFilter.empty() ? "(off)" : gDumpFuncFilter.c_str(),
                    (void *)&gDumpFuncFilter);
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (gDumpSharedState) {
    EJitSharedDumpState &D = gDumpSharedState->dump;
    uint32_t expected = 0;
    while (!D.lock.compareExchange(expected, 1))
      expected = 0;
    uint32_t len = 0;
    if (!name.empty()) {
      while (len + 1 < kEJitSharedDumpNameBytes && len < name.size()) {
        D.filterName[len] = name[len];
        ++len;
      }
    }
    D.filterName[len] = 0;
    D.filterLen = len;
    D.hasDump.storeRelease(0);
    D.status.storeRelease(0);
    D.resultNameLen = 0;
    D.irSize = 0;
    D.asmSize = 0;
    D.keyHi = 0;
    D.keyLo = 0;
    D.workerCore = kEJitInvalidCoreId;
    D.resultName[0] = 0;
    D.filterEnabled.storeRelease(len ? 1u : 0u);
    D.lock.storeRelease(0);
    EJIT_DIAG_DEBUG("set_dump_filter shared enabled=%u len=%u &shared=%p",
              len ? 1u : 0u, len, (void *)gDumpSharedState);
  }
#endif
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
void setDumpSharedState(EJitSharedTaskPoolState *state) {
  gDumpSharedState = state;
  EJIT_DIAG_DEBUG("set_dump_shared_state state=%p", (void *)state);
  // If a filter was set before the shared state was bound — e.g. ejit_dump_func
  // called during the init_array phase, before ejit_init —
  // propagate it into the now-bound shared state. Otherwise the owner worker
  // (possibly a different core) sees an empty shared filter and never captures,
  // even though the producer thinks dump is armed.
  std::string filter;
  {
    std::lock_guard<DumpMutexType> lock(gDumpMutex);
    filter = gDumpFuncFilter;
  }
  if (gDumpSharedState && !filter.empty())
    setDumpFuncFilter(filter);
}

static void sharedDumpLock(EJitSharedDumpState &D) {
  uint32_t expected = 0;
  while (!D.lock.compareExchange(expected, 1))
    expected = 0;
}

static void sharedDumpUnlock(EJitSharedDumpState &D) {
  D.lock.storeRelease(0);
}

static bool getSharedDumpFilter(std::string &out) {
  if (!gDumpSharedState)
    return false;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  sharedDumpLock(D);
  bool enabled = D.filterEnabled.loadAcquire() != 0;
  if (enabled) {
    uint32_t len = D.filterLen;
    if (len >= kEJitSharedDumpNameBytes)
      len = kEJitSharedDumpNameBytes - 1;
    out.assign(D.filterName, D.filterName + len);
  }
  sharedDumpUnlock(D);
  return enabled;
}
#else
void setDumpSharedState(EJitSharedTaskPoolState * /*state*/) {}
#endif

static bool getActiveDumpFilter(std::string &out) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (getSharedDumpFilter(out))
    return true;
#endif
  std::lock_guard<DumpMutexType> lock(gDumpMutex);
  if (gDumpFuncFilter.empty())
    return false;
  out = gDumpFuncFilter;
  return true;
}

/// Saved IR+ASM for a captured specialization (latest per function name).
struct DumpEntry {
  uint64_t cacheKey = 0;
  std::string IR;
  std::string ASM;
};

// Process-wide store of captured IR+ASM, filled by the IR transform layer
// (worker thread) when the filter matches, read by ejit_print_dumped() (user
// thread). Guarded by gDumpMutex. These are ordinary process statics, not part
// of the shared taskpool state; cross-core visibility depends on the worker
// running in the same process image (addresses are logged to diagnose this).
static std::map<std::string, DumpEntry> gDumpStore;

static void dumpBytesSafe(const char *label, const char *data, size_t n) {
  EJIT_DIAG("=== %s begin size=%u ===", label, (unsigned)n);
  size_t i = 0;
  unsigned lineNo = 0;
  unsigned printedLines = 0;
  while (i < n) {
    size_t lineEnd = i;
    while (lineEnd < n && data[lineEnd] != '\n')
      ++lineEnd;
    size_t pos = i;
    if (pos == lineEnd) {
      EJIT_DIAG("%s:%u: ", label, lineNo);
    } else {
      while (pos < lineEnd) {
        size_t chunk = lineEnd - pos;
        if (chunk > 180)
          chunk = 180;
        EJIT_DIAG("%s:%u: %.*s", label, lineNo, (int)chunk, data + pos);
        pos += chunk;
      }
    }
    throttleDumpPrint(++printedLines);
    ++lineNo;
    i = lineEnd + 1;
  }
  (void)lineNo;
  EJIT_DIAG("=== %s end lines=%u ===", label, lineNo);
}

/// Emit a multi-line blob (IR or ASM) through EJIT_DIAG. SRE-safe: no lambda,
/// no Twine, no raw_fd_ostream. Splits on '\n' and chunks each line to a small
/// fixed width so a single EJIT_DIAG/SRE_printf call stays bounded.
static void dumpLinesSafe(const char *label, const std::string &s) {
  dumpBytesSafe(label, s.data(), s.size());
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
static uint32_t copyDumpBytes(char *dst, uint32_t cap, const char *src,
                              size_t size, bool &truncated) {
  if (cap == 0)
    return 0;
  uint32_t n = 0;
  while (n + 1 < cap && n < size) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
  truncated = size >= cap;
  return n;
}

static void captureSharedDumpMetadata(const std::string &fnName,
                                      uint64_t cacheKey, size_t irSize,
                                      size_t asmSize) {
  if (!gDumpSharedState)
    return;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  uint32_t core = EJitCoreId::current();
  sharedDumpLock(D);
  bool nameTrunc = false;
  D.hasDump.storeRelease(0);
  D.resultNameLen = copyDumpBytes(D.resultName, kEJitSharedDumpNameBytes,
                                  fnName.data(), fnName.size(), nameTrunc);
  D.irSize = irSize > 0xffffffffu ? 0xffffffffu : (uint32_t)irSize;
  D.asmSize = asmSize > 0xffffffffu ? 0xffffffffu : (uint32_t)asmSize;
  D.keyHi = (uint32_t)(cacheKey >> 32);
  D.keyLo = (uint32_t)(cacheKey & 0xffffffffu);
  D.workerCore = core;
  D.status.storeRelease(nameTrunc ? 4u : 0u);
  D.hasDump.storeRelease(1);
  sharedDumpUnlock(D);
  EJIT_DIAG_DEBUG(
      "capture shared metadata func=%s core=%u ir=%u asm=%u &shared=%p",
      fnName.c_str(), core, (unsigned)irSize, (unsigned)asmSize,
      (void *)gDumpSharedState);
}

static bool printSharedDumpHint(const char *name) {
  if (!gDumpSharedState)
    return false;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  sharedDumpLock(D);
  if (D.hasDump.loadAcquire() == 0) {
    sharedDumpUnlock(D);
    return false;
  }
  bool hasName = name && name[0];
  bool match = true;
  if (hasName) {
    uint32_t i = 0;
    while (i < D.resultNameLen && name[i] && name[i] == D.resultName[i])
      ++i;
    match = i == D.resultNameLen && name[i] == 0;
  }
  if (!match) {
    sharedDumpUnlock(D);
    return false;
  }
  uint32_t workerCore = D.workerCore;
  uint32_t irSize = D.irSize;
  uint32_t asmSize = D.asmSize;
  uint32_t keyHi = D.keyHi;
  uint32_t keyLo = D.keyLo;
  char stored[kEJitSharedDumpNameBytes];
  uint32_t n = D.resultNameLen;
  if (n >= kEJitSharedDumpNameBytes)
    n = kEJitSharedDumpNameBytes - 1;
  for (uint32_t i = 0; i < n; ++i)
    stored[i] = D.resultName[i];
  stored[n] = 0;
  sharedDumpUnlock(D);
  (void)irSize;
  (void)asmSize;
  (void)keyHi;
  (void)keyLo;
  (void)stored;
  if (workerCore == kEJitInvalidCoreId)
    EJIT_DIAG("print_dumped: dump for \"%s\" is worker-local; run "
              "ejit_print_dumped(\"%s\") on the worker core. ir_size=%u "
              "asm_size=%u key_hi=0x%08x key_lo=0x%08x",
              stored, stored, irSize, asmSize, keyHi, keyLo);
  else
    EJIT_DIAG("print_dumped: dump for \"%s\" is stored on worker core %u; "
              "run ejit_print_dumped(\"%s\") on that core. ir_size=%u "
              "asm_size=%u key_hi=0x%08x key_lo=0x%08x",
              stored, workerCore, stored, irSize, asmSize, keyHi, keyLo);
  return true;
}
#endif

/// Called from the IR transform layer when the filter matches: save the
/// post-optimization IR and emitted ASM for later selective printing.
static void captureDump(const std::string &fnName, uint64_t cacheKey,
                        std::string IR, std::string ASM) {
  EJIT_DIAG_DEBUG("capture enter func=%s ir_size=%u asm_size=%u &store=%p",
            fnName.c_str(), (unsigned)IR.size(), (unsigned)ASM.size(),
            (void *)&gDumpStore);
  std::lock_guard<DumpMutexType> lock(gDumpMutex);
  EJIT_DIAG_DEBUG("capture store_size before=%u", (unsigned)gDumpStore.size());
  gDumpStore[fnName] = DumpEntry{cacheKey, std::move(IR), std::move(ASM)};
  EJIT_DIAG_DEBUG("capture store_size after=%u", (unsigned)gDumpStore.size());
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Publish only small metadata. Full text remains in the worker-local map.
  const DumpEntry &E = gDumpStore[fnName];
  captureSharedDumpMetadata(fnName, cacheKey, E.IR.size(), E.ASM.size());
#endif
}

/// Print one stored entry: header (name, key hi/lo, IR/ASM sizes) followed by
/// the IR and ASM bodies. SRE-safe: no Twine, no lambda, no temporary label.
static void printOneDumpSafe(const char *requestedName,
                             const std::string &storedName,
                             const DumpEntry &e) {
  uint32_t keyHi = (uint32_t)(e.cacheKey >> 32);
  uint32_t keyLo = (uint32_t)(e.cacheKey & 0xffffffffu);
  (void)keyHi;
  (void)keyLo;
  EJIT_DIAG("print_dumped hit requested=%s stored=%s key_hi=0x%08x "
            "key_lo=0x%08x ir_size=%u asm_size=%u",
            requestedName ? requestedName : "(list)", storedName.c_str(), keyHi,
            keyLo, (unsigned)e.IR.size(), (unsigned)e.ASM.size());
  if (!e.IR.empty())
    dumpLinesSafe("dump IR", e.IR);
  if (!e.ASM.empty())
    dumpLinesSafe("dump ASM", e.ASM);
}

/// Print saved IR+ASM through EJIT_DIAG, one line per IR/ASM line. A null/empty
/// name prints all payloads available on this core.
void printDumped(const char *name) {
  EJIT_DIAG_DEBUG("print_dumped enter name=%s &filter=%p &store=%p",
                  (name && name[0]) ? name : "(all)", (void *)&gDumpFuncFilter,
                  (void *)&gDumpStore);
  bool hasName = name && name[0];
  // The complete payloads are worker-local. A specific name prints one entry;
  // an empty name prints every entry captured by this core.
  {
    std::lock_guard<DumpMutexType> lock(gDumpMutex);
    if (hasName) {
      auto it = gDumpStore.find(name);
      if (it != gDumpStore.end()) {
        printOneDumpSafe(name, it->first, it->second);
        return;
      }
    } else if (!gDumpStore.empty()) {
      EJIT_DIAG("print_dumped saved entries=%u", (unsigned)gDumpStore.size());
      for (auto &kv : gDumpStore)
        printOneDumpSafe(nullptr, kv.first, kv.second);
      return;
    }
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // A non-worker core cannot read the worker-private payload. Shared state
  // carries only enough metadata to direct the caller to the owning core.
  if (printSharedDumpHint(name))
    return;
#endif
  if (hasName)
    EJIT_DIAG_DEBUG("print_dumped miss name=%s store_size=%u", name,
                    (unsigned)gDumpStore.size());
  else
    EJIT_DIAG("print_dumped: nothing saved");
}

} // namespace ejit
} // namespace llvm

EJitOrcEngine::EJitOrcEngine() : P(std::make_unique<Impl>()) {}
EJitOrcEngine::~EJitOrcEngine() = default;

Expected<std::unique_ptr<EJitOrcEngine>>
EJitOrcEngine::Create(const Config &config,
                      PeriodArrayRegistry &periodReg,
                      EJitRuntimeState &runtimeState) {
  EJIT_DIAG_VERBOSE("create: opt=%d dump=%s",
                    static_cast<int>(config.optLevel),
                    config.dumpJITDir.empty() ? "(off)" : config.dumpJITDir.c_str());
  auto engine = std::unique_ptr<EJitOrcEngine>(new EJitOrcEngine());
  engine->P->periodReg = &periodReg;
  engine->P->runtimeState = &runtimeState;
  engine->P->dumpJITDir = config.dumpJITDir;

  // Bare-metal / cross-compiled: use compile-time target triple.
  // Native host: auto-detect via detectHost().
#if defined(EJIT_DEFAULT_TRIPLE) || defined(EJIT_FREESTANDING)
  #ifdef EJIT_DEFAULT_TRIPLE
    Expected<orc::JITTargetMachineBuilder> JTMBOrErr(
        orc::JITTargetMachineBuilder(Triple(EJIT_DEFAULT_TRIPLE)));
  #else
    #error EJIT_FREESTANDING requires EJIT_DEFAULT_TRIPLE to be set
  #endif
#else
  auto JTMBOrErr = orc::JITTargetMachineBuilder::detectHost();
#endif
  if (!JTMBOrErr) {
    EJIT_DIAG("create FAIL: target machine builder error");
    return JTMBOrErr.takeError();
  }

  // Use Small code model so data accesses use ADRP+LDR (2 insns/global, ±4GB
  // PC-relative) instead of movz/movk absolute (5 insns/global). The JIT slab
  // is ~1.5-2.2GB from .text (within ADRP's ±4GB range), confirmed by
  // compileFailed=0 with dso_local=true. Function calls are always BL on
  // AArch64 (unaffected by code model); JITLink auto-stubs out-of-range BL.
  // With Large, the 3 extra movz/movk instructions per global access eaten
  // the specialization savings (fewer BBs / folded branches). Small makes
  // the per-global cost match AOT (ADRP+LDR), so specialization gains show.
  JTMBOrErr->setCodeModel(CodeModel::Small);

  // Build a TargetMachine (same options the JIT compiles with) for the
  // name-filtered ASM diagnostic dump. Failure is non-fatal — the dump is
  // simply unavailable.
  if (auto TMOrErr = JTMBOrErr->createTargetMachine())
    engine->P->dumpTM = std::move(*TMOrErr);
  else
    consumeError(TMOrErr.takeError());

  orc::LLJITBuilder Builder;
  Builder.setJITTargetMachineBuilder(*JTMBOrErr);
  Builder.setNumCompileThreads(0);
// Bare-metal: skip host process symbol search (avoids dlopen/dlsym),
// and skip ORC runtime injection / EH frames / atexit / global ctors.
#ifdef EJIT_FREESTANDING
  Builder.setLinkProcessSymbolsByDefault(false);
  Builder.setPlatformSetUp(orc::setUpInactivePlatform);
#endif

#ifdef EJIT_SRE_CODE_POOL
  // Route JIT machine-code memory through EmbeddedJIT's own 2MiB pools instead
  // of the default JITLink mmap/mprotect path. The pool manager is owned by the
  // engine (so it outlives the LLJIT); the object linking layer owns a memory
  // manager that references it. Pages are kept RW here and sealed to RX later,
  // at lookup time, by the pool manager's enable_ex sealing.
  engine->P->codePool = makeSreCodePoolManager();
  {
    EJitCodePoolManager *Pool = engine->P->codePool.get();
    Builder.setObjectLinkingLayerCreator(
        [Pool](orc::ExecutionSession &ES)
            -> Expected<std::unique_ptr<orc::ObjectLayer>> {
          // Page size only affects per-segment layout padding; we never apply
          // per-segment protections (sealing is done per 2MiB pool), so a
          // conservative 4KiB is sufficient and portable.
          constexpr size_t JitPageSize = 4096;
          return std::make_unique<orc::ObjectLinkingLayer>(
              ES, std::make_unique<EJitCodePoolMemoryManager>(*Pool,
                                                              JitPageSize));
        });
  }
#endif

  auto J = Builder.create();
  if (!J) {
    EJIT_DIAG("create FAIL: LLJIT builder error");
    return J.takeError();
  }

  engine->P->J = std::move(*J);

  // Retarget standard AArch64 pointer-jump stubs to their final destination
  // when the resolved B/BL displacement fits the architectural range. This
  // plugin must run before the diagnostic plugin so PostFixup reporting sees
  // the actual direct/stubbed result.
  if (auto *OLL = dyn_cast<orc::ObjectLinkingLayer>(
          &engine->P->J->getObjLinkingLayer()))
    OLL->addPlugin(std::make_shared<EJitLinkOptimizationPlugin>());

  // Attach the JITLink branch-relocation diagnostic plugin. It appends a
  // PostFixup pass that audits every AArch64 branch relocation and reports
  // which ones remain bridged through a $__STUBS PointerJumpStub + $__GOT
  // (because the resolved target is out of +-128MB or the chain is not safe to
  // relax) instead of a direct BL. INFO emits one summary per graph; VERBOSE
  // additionally emits each relocation. Output uses SRE_printf on bare-metal.
  if (auto *OLL = dyn_cast<orc::ObjectLinkingLayer>(
          &engine->P->J->getObjLinkingLayer()))
    OLL->addPlugin(std::make_shared<EJitLinkDiagPlugin>());

  // Override the default error reporter (logErrorsToStdErr → errs() →
  // raw_fd_ostream) with a bare-metal-safe version using EJIT_DIAG.  On
  // SRE / bare-metal the default reporter crashes because raw_fd_ostream
  // internally calls POSIX I/O (open / write / isatty) whose GOT/PLT
  // entries may be unmapped.  EJIT_DIAG uses SRE_printf / std::printf
  // which are always available on the target.
  engine->P->J->getExecutionSession().setErrorReporter(
      [](Error Err) {
        EJIT_DIAG("JIT error: %s", toString(std::move(Err)).c_str());
      });

  // Create persistent optimizer — analysis managers are registered once here
  // and reused across compilations (cleared between runs).
  engine->P->optimizer = std::make_unique<EJitOptimizer>(periodReg);

  // Register all known global variable addresses from the PeriodArrayRegistry
  // so that external global references in any loaded bitcode module resolve
  // to the AOT process's memory. Deduplicate: the constructor may run twice
  // (PASS1 + PASS2 both add to global_ctors), causing duplicate entries.
  {
    auto &JD = engine->P->J->getMainJITDylib();
    orc::SymbolMap symMap;
    for (auto &kv : periodReg.getStaticVars())
      symMap[engine->P->J->mangleAndIntern(kv.varName)] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(kv.varAddr),
                                 JITSymbolFlags::Exported);
    if (!symMap.empty()) {
      size_t n = symMap.size();
      (void)n;
      if (auto Err = JD.define(orc::absoluteSymbols(std::move(symMap))))
        EJIT_DIAG("create: define %zu static var(s) FAILED: %s", n,
                  toString(std::move(Err)).c_str());
    }
  }
  EJIT_DIAG_VERBOSE("create: static vars registered=%zu",
                    periodReg.getStaticVars().size());

  // Set up IR transform layer: runs the specialization pipeline during
  // JIT compilation (parameter substitution → InstCombine → StructFieldPass
  // → core optimization pipeline).
  engine->P->J->getIRTransformLayer().setTransform(
      [engine = engine.get()](
          orc::ThreadSafeModule TSM,
          const orc::MaterializationResponsibility &R)
          -> Expected<orc::ThreadSafeModule> {
        TSM.withModuleDo([engine](Module &M) {
          LLVM_DEBUG(dbgs() << "ejit-orc-engine: JIT transform on "
                            << M.getName() << "\n");
          const SpecializationContext *ctx = engine->P->activeCtx;
          if (!ctx)
            return;

          // Clear stale analysis results from previous compilations
          // (each compilation uses a fresh Module with new IR unit pointers).
          engine->P->optimizer->clearAnalyses();

          // Dump pre-optimization IR (before the JIT pipeline runs).
          if (!engine->P->dumpJITDir.empty()) {
            std::string prePath = engine->P->dumpJITDir + "/" +
                                  ctx->fnName + "_" +
                                  std::to_string(ctx->cacheKey) + "_pre.ll";
            std::error_code EC;
            llvm::raw_fd_ostream preOS(prePath, EC);
            if (!EC)
              M.print(preOS, nullptr);
          }

          engine->P->optimizer->runPipeline(M, *ctx);

          // Dump post-optimization IR.
          if (!engine->P->dumpJITDir.empty()) {
            std::string path = engine->P->dumpJITDir + "/" +
                               ctx->fnName + "_" +
                               std::to_string(ctx->cacheKey) + "_opt.ll";
            std::error_code EC;
            llvm::raw_fd_ostream OS(path, EC);
            if (!EC)
              M.print(OS, nullptr);
          }

          // Name-filtered IR+ASM capture for later selective printing. Filter
          // set via ejit_dump_func(); captured entries printed on demand via
          // ejit_print_dumped(). Bare-metal-safe (strings only, no
          // raw_fd_ostream). Captures the post-optimization IR and the emitted
          // assembly (from the same TargetMachine the JIT compiles with).
          // Capture is exact-name only. The local gDumpStore keeps one dynamic
          // IR/ASM payload per captured function name (overwritten on
          // re-compile); the shared dump table keeps cross-core visible dynamic
          // payloads for recent captures.
          {
            std::string DumpFilter;
            bool hasFilter = getActiveDumpFilter(DumpFilter);
            bool match =
                hasFilter && (DumpFilter == "*" || ctx->fnName == DumpFilter);
            EJIT_DIAG_DEBUG("dump check filter=%s fn=%s key_hi=0x%08x "
                            "key_lo=0x%08x match=%d &filter=%p",
                            hasFilter ? DumpFilter.c_str() : "(off)",
                            ctx->fnName.c_str(), (uint32_t)(ctx->cacheKey >> 32),
                            (uint32_t)(ctx->cacheKey & 0xffffffffu), match ? 1 : 0,
                            (void *)&gDumpFuncFilter);
            if (match) {
              // IR capture always runs first so it succeeds even if the ASM
              // diagnostic path is disabled or fails.
              std::string IR;
              raw_string_ostream IOS(IR);
              M.print(IOS, nullptr);
              IOS.flush();

              std::string Asm;
              // Textual ASM emit goes through addPassesToEmitFile ->
              // addAsmPrinter -> createMCStreamer(AssemblyFile). Under
              // EJIT_TRIM_LLVM_BACKEND that path is compile-time removed and
              // createMCStreamer returns "textual assembly output unavailable";
              // addAsmPrinter reports it via MCContext::reportError ->
              // llvm::errs() (raw_fd_ostream fd 2), whose constructor is
              // unmapped on bare-metal/SRE and crashes. So under trim we skip
              // ASM (IR is still captured — M.print to a string stream is
              // SRE-safe). To get ASM on target, build with EJIT_DUMP_ASM=ON
              // (re-enables the textual asm backend under trim). The ASM emit's
              // InstPrinter needs snprintf/vsnprintf: link a libc that provides
              // them, OR ejit_test/stubs/ejit_sre_format_stubs.cpp if the SRE
              // libc lacks them (not both — strong-symbol conflict). The success
              // path of the emit does not call errs(), so once the path is
              // compiled in it is SRE-safe.
#if !defined(EJIT_TRIM_LLVM_BACKEND) || defined(EJIT_DUMP_ASM)
              if (engine->P->dumpTM) {
                EJIT_DIAG_DEBUG("dump asm begin fn=%s", ctx->fnName.c_str());
                SmallVector<char, 0> AsmBuf;
                raw_svector_ostream AOS(AsmBuf);
                legacy::PassManager PM;
                if (!engine->P->dumpTM->addPassesToEmitFile(
                        PM, AOS, /*DwoOut=*/nullptr,
                        CodeGenFileType::AssemblyFile)) {
                  EJIT_DIAG_DEBUG("dump asm PM.run begin fn=%s", ctx->fnName.c_str());
                  // Clone M before running codegen so this diagnostic ASM emit
                  // path cannot perturb the live module handed back to the JIT
                  // for real compilation (codegen is IR-read-only in theory,
                  // but target passes are not guaranteed to never touch IR).
                  // The clone is local to this diagnostic path and discarded.
                  std::unique_ptr<Module> MClone = CloneModule(M);
                  PM.run(*MClone);
                  EJIT_DIAG_DEBUG("dump asm PM.run end fn=%s", ctx->fnName.c_str());
                  Asm.assign(AsmBuf.begin(), AsmBuf.end());
                  EJIT_DIAG_DEBUG("dump asm size=%u fn=%s", (unsigned)Asm.size(),
                            ctx->fnName.c_str());
                } else {
                  EJIT_DIAG_DEBUG("dump asm addPassesToEmitFile failed fn=%s",
                            ctx->fnName.c_str());
                }
              }
#else
              EJIT_DIAG_DEBUG("dump asm skipped (EJIT_TRIM_LLVM_BACKEND, "
                              "EJIT_DUMP_ASM off) fn=%s; IR captured",
                              ctx->fnName.c_str());
#endif
              captureDump(ctx->fnName, ctx->cacheKey, std::move(IR),
                          std::move(Asm));
            }
          }
        });
        return std::move(TSM);
      });

  EJIT_DIAG("create OK: LLJIT ready");
  return engine;
}

Error EJitOrcEngine::loadBitcodeModule(StringRef bitcodeData,
                                       uint64_t cacheKey,
                                       const std::string &origFnName) {
  EJIT_DIAG_VERBOSE("loadBitcode key=0x%016lx func=%s size=%zu", cacheKey,
                    origFnName.c_str(), bitcodeData.size());
  auto Ctx = std::make_unique<LLVMContext>();
  auto Buf = MemoryBuffer::getMemBuffer(
      bitcodeData, ("spec_" + std::to_string(cacheKey) + ".bc"));
  auto ModuleOrErr = parseBitcodeFile(Buf->getMemBufferRef(), *Ctx);
  if (!ModuleOrErr) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: parse bitcode error", cacheKey);
    return ModuleOrErr.takeError();
  }

  Triple TT((*ModuleOrErr)->getTargetTriple());
  if (TT.isAArch64() && TT.isOSBinFormatELF()) {
    // External-symbol access from JIT specializations. The JIT slab is
    // SRE_MemAlloc'd ~2-3GB from the main binary's .text/.data - beyond the
    // AArch64 BL reach (±128MB) but within ADRP reach (±4GB). Treat the two
    // kinds differently rather than clearing dso_local on both:
    //   * Functions: clear dso_local so calls route through JITLink PLT stubs
    //     (PointerJumpStub = ADRP x16; LDR x16,[x16]; BR x16), which bridge the
    //     ±128MB BL gap via a GOT entry. A direct BL to a 2-3GB-distant target
    //     would overflow.
    //   * Globals: keep dso_local so data access is ADRP+LDR (direct, ±4GB
    //     reaches the slab). Clearing them forced a per-global GOT load (62 GOT
    //     loads / ~1700c on DlschCcScheduler vs 0 in AOT) for no reach benefit,
    //     since ADRP already reaches. So globals are NOT cleared.
    for (Function &F : (*ModuleOrErr)->functions()) {
      if (F.isDeclaration() && !F.isIntrinsic())
        F.setDSOLocal(false);
    }
  }

  // ejit_entry functions may have internal linkage (e.g. declared `static` in
  // source). ORC's IR layer excludes local-linkage symbols from the JITDylib
  // symbol table (Layer.cpp: hasLocalLinkage() skip), so a static entry is
  // invisible to lookup ("symbol not found", no materialization). The function
  // being compiled (origFnName) is the JIT lookup target — force it to
  // external linkage so ORC registers and can materialize it. Spec JITDylibs
  // are isolated, so this cannot collide with other specializations.
  if (Function *EntryF = (*ModuleOrErr)->getFunction(origFnName))
    if (!EntryF->isDeclaration() && EntryF->hasLocalLinkage())
      EntryF->setLinkage(GlobalValue::ExternalLinkage);

  // Collect global variable addresses from the registry for symbols
  // that appear as external declarations in the bitcode module.
  orc::SymbolMap globalSymbols;
  for (GlobalVariable &GV : (*ModuleOrErr)->globals()) {
    if (!GV.isDeclaration() || GV.getName().empty())
      continue;
    void *addr = nullptr;
    if (const auto *info = P->periodReg->getArrayInfo(GV.getName().str()))
      addr = info->baseAddr;
    else
      addr = P->periodReg->getStaticVarAddr(GV.getName().str());
    if (!addr)
      continue;
    globalSymbols[P->J->mangleAndIntern(GV.getName())] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(addr),
                               JITSymbolFlags::Exported);
  }

  // Each specialization gets its own JITDylib so that symbols from
  // different specializations (same TU bitcode loaded multiple times)
  // never conflict. Remove any stale JD from a previous compilation
  // of the same cacheKey (e.g., after ejit_clear_cache).
  auto it = P->specDylibs.find(cacheKey);
  if (it != P->specDylibs.end()) {
    if (auto Err = P->J->getExecutionSession().removeJITDylib(*it->second))
      EJIT_DIAG("loadBitcode key=0x%016lx: remove stale JD FAILED: %s",
                cacheKey, toString(std::move(Err)).c_str());
    P->specDylibs.erase(it);
  }

  auto JDOrErr = P->J->createJITDylib("spec_" + std::to_string(cacheKey));
  if (!JDOrErr) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: create JITDylib error", cacheKey);
    return JDOrErr.takeError();
  }

  // Resolve undefined function symbols from user-registered table.
  // Required for bare-metal where dynamic lookup (dlsym) is unavailable.
  // Throttle diagnostics: tally unresolved externals and emit a single
  // summary line per load (below) instead of one line per symbol; the
  // individual names are listed at DEBUG for regression triage.
  size_t unresolvedFuncs = 0;
  size_t unresolvedGlobals = 0;
  SmallVector<std::string, 16> unresolvedNames;
  static constexpr size_t kMaxUnresolvedNames = 32;
  SmallPtrSet<const Function *, 16> ReferencedExternalFuncs;
  SmallPtrSet<const GlobalVariable *, 16> ReferencedExternalGlobals;
  collectReferencedExternalDecls(**ModuleOrErr, ReferencedExternalFuncs,
                                 ReferencedExternalGlobals);
  for (Function &F : (*ModuleOrErr)->functions()) {
    if (!F.isDeclaration() || F.getName().empty())
      continue;
    std::string name = F.getName().str();
    if (globalSymbols.count(P->J->mangleAndIntern(name)))
      continue;
    auto it = P->userSymbols.find(name);
    if (it == P->userSymbols.end()) {
      if (!F.isIntrinsic() && ReferencedExternalFuncs.contains(&F)) {
        ++unresolvedFuncs;
        if (unresolvedNames.size() < kMaxUnresolvedNames)
          unresolvedNames.push_back("f:" + name);
      }
      continue;
    }
    globalSymbols[P->J->mangleAndIntern(name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(it->second),
                               JITSymbolFlags::Exported);
  }
  for (GlobalVariable &GV : (*ModuleOrErr)->globals()) {
    if (!GV.isDeclaration() || GV.getName().empty())
      continue;
    std::string name = GV.getName().str();
    if (globalSymbols.count(P->J->mangleAndIntern(name)))
      continue;
    auto it = P->userSymbols.find(name);
    if (it == P->userSymbols.end()) {
      if (ReferencedExternalGlobals.contains(&GV)) {
        ++unresolvedGlobals;
        if (unresolvedNames.size() < kMaxUnresolvedNames)
          unresolvedNames.push_back("g:" + name);
      }
      continue;
    }
    globalSymbols[P->J->mangleAndIntern(name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(it->second),
                               JITSymbolFlags::Exported);
  }
  if (unresolvedFuncs || unresolvedGlobals)
    EJIT_DIAG("loadBitcode: %zu unresolved external(s) not registered "
              "(%zu funcs, %zu globals)",
              unresolvedFuncs + unresolvedGlobals, unresolvedFuncs,
              unresolvedGlobals);
  EJIT_DIAG_DEBUG("loadBitcode: %zu unresolved name(s) listed (of %zu total):",
                  unresolvedNames.size(),
                  unresolvedFuncs + unresolvedGlobals);
  for (const std::string &n : unresolvedNames)
    EJIT_DIAG_DEBUG("  %s", n.c_str());

  // Provide codegen-synthesized runtime symbols (memset/memcpy/memmove/memcmp
  // and the stack-protector guard/fail) that the AOT symbol collector cannot
  // register because they are never present as IR declarations — the JIT
  // back-end lowers the llvm.mem* intrinsics and -fstack-protector attributes
  // into references to them. On freestanding targets process-symbol lookup is
  // disabled, so without these every JIT compilation fails at link time.
  // They go into the spec JITDylib (which is isolated: it does not link back
  // to the main JITDylib) so each specialization resolves them locally.
  for (const LibcallSymbol &LCS : getLibcallSymbols())
    globalSymbols[P->J->mangleAndIntern(LCS.name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(LCS.addr),
                               JITSymbolFlags::Exported);

  // Define all collected symbols in the spec JITDylib before loading the
  // IR module so the JIT linker can resolve external references.
  if (!globalSymbols.empty()) {
    size_t nGlobals = globalSymbols.size();
    (void)nGlobals;
    if (auto Err = JDOrErr->define(
            orc::absoluteSymbols(std::move(globalSymbols))))
      EJIT_DIAG("loadBitcode key=0x%016lx: define %zu global(s) FAILED: %s",
                cacheKey, nGlobals, toString(std::move(Err)).c_str());
  }

  if (auto Err = P->J->addIRModule(*JDOrErr,
      orc::ThreadSafeModule(std::move(*ModuleOrErr), std::move(Ctx)))) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: add IR module error", cacheKey);
    return Err;
  }

  P->specDylibs[cacheKey] = &*JDOrErr;
  EJIT_DIAG_VERBOSE("loadBitcode OK key=0x%016lx func=%s", cacheKey,
                    origFnName.c_str());
  return Error::success();
}

Expected<void *> EJitOrcEngine::lookup(uint64_t cacheKey,
                                       const std::string &name) {
  auto it = P->specDylibs.find(cacheKey);
  if (it == P->specDylibs.end()) {
    EJIT_DIAG("lookup FAIL key=0x%016lx name=%s: no spec JITDylib", cacheKey,
              name.c_str());
    return make_error<StringError>(
        "No specialization JITDylib found for: " + std::to_string(cacheKey),
        inconvertibleErrorCode());
  }

  auto addr = P->J->lookup(*it->second, name);
  if (!addr) {
    EJIT_DIAG("lookup FAIL key=0x%016lx name=%s: symbol not found", cacheKey,
              name.c_str());
    return addr.takeError();
  }
  void *Ptr = reinterpret_cast<void *>(addr->getValue());

#ifdef EJIT_SRE_CODE_POOL
  // Legacy whole-pool seal: flip the 2MiB pool that contains the resolved
  // function to RX before it is handed back. This is the only point a JIT pool
  // transitions RW->RX in whole-pool mode. Idempotent: a pool already sealed
  // (e.g. on allocation rollover) is not re-flipped, so repeated lookups of the
  // same function do not re-invoke enable_ex. Only pool-backed code is sealed;
  // an address resolved outside the pools (e.g. a process/absolute symbol) is
  // left untouched. If sealing fails we must not return a callable pointer.
  //
  // In 4K page-seal mode the seal already happened per-page at finalize (in the
  // code-pool memory manager), so nothing is done here.
  if (P->codePool && !P->codePool->usesPageSeal() &&
      P->codePool->contains(Ptr)) {
    if (auto Err = P->codePool->sealPoolContaining(Ptr)) {
      EJIT_DIAG("lookup FAIL key=0x%016lx ptr=%p: seal pool error", cacheKey,
                Ptr);
      return std::move(Err);
    }
  }
#endif

  EJIT_DIAG_VERBOSE("lookup OK key=0x%016lx name=%s ptr=%p", cacheKey,
                    name.c_str(), Ptr);
  return Ptr;
}

void EJitOrcEngine::setActiveContext(const SpecializationContext *ctx) {
  P->activeCtx = ctx;
}

const SpecializationContext *EJitOrcEngine::getActiveContext() const {
  return P->activeCtx;
}

ArrayRef<std::string> EJitOrcEngine::getLastCounterNames() const {
  if (P->optimizer)
    return P->optimizer->getLastCounterNames();
  return {};
}


void EJitOrcEngine::addUserSymbol(const std::string &name, void *addr) {
  P->userSymbols[name] = addr;
}

#ifdef EJIT_SRE_CODE_POOL
EJitCodePoolManager::Stats EJitOrcEngine::getCodePoolStats() const {
  if (P->codePool)
    return P->codePool->getStats();
  return EJitCodePoolManager::Stats{};
}

bool EJitOrcEngine::findCodeRange(const void *FnPtr,
                                  EJitCompiledCodeInfo &Out) const {
  if (!P->codePool) {
    EJIT_DIAG("findCodeRange FAIL: no code pool (fnPtr=%p)", FnPtr);
    return false;
  }
  return P->codePool->findRange(FnPtr, Out);
}
#endif
