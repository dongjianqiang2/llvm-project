//===-- EJit.cpp - EmbeddedJIT Main C++ API -------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/Config/Targets.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLogger.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;
using namespace llvm::ejit;

// Registry entries: each translation unit's PASS1/PASS2 output places its
// entries into the ".ejit_bitcode" / ".ejit_period" sections as private
// symbols. The linker concatenates all such input sections across every TU.
// The leading-dot section names are not valid C identifiers, so the linker
// does NOT auto-synthesize the __start_/__stop_ bounds — a linker script must
// define these four symbols and bracket the sections (see the example
// llvm/lib/ExecutionEngine/EJIT/ejit_registry.ld).  This replaces the old
// single external __ejit_registry_*[] arrays, which produced duplicate-symbol
// link errors across multiple TUs.
//
// Hosted builds (!EJIT_FREESTANDING): the bounds are declared weak so a
// program with no ejit_entry functions — or no linker script — resolves them
// to null and walks an empty range (no link error). Freestanding builds
// (EJIT_FREESTANDING): the bounds are strong, because the weak attribute
// creates a GOT relocation that is undesirable on bare-metal; a linker script
// MUST therefore define them.
extern "C" {
#ifndef EJIT_FREESTANDING
extern const ejit_reg_entry_t __start_ejit_bitcode[] __attribute__((weak));
extern const ejit_reg_entry_t __stop_ejit_bitcode[] __attribute__((weak));
extern const ejit_reg_entry_t __start_ejit_period[] __attribute__((weak));
extern const ejit_reg_entry_t __stop_ejit_period[] __attribute__((weak));
#else
extern const ejit_reg_entry_t __start_ejit_bitcode[];
extern const ejit_reg_entry_t __stop_ejit_bitcode[];
extern const ejit_reg_entry_t __start_ejit_period[];
extern const ejit_reg_entry_t __stop_ejit_period[];
#endif
}

namespace {

void initializeEJitTargets() {
#ifdef EJIT_TRIM_LLVM_BACKEND
#ifdef EJIT_DEFAULT_TRIPLE
  Triple TT(EJIT_DEFAULT_TRIPLE);
#if LLVM_HAS_AARCH64_TARGET
  if (TT.isAArch64()) {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    return;
  }
#endif
#if LLVM_HAS_X86_TARGET
  if (TT.isX86()) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    return;
  }
#endif
#endif
#ifndef EJIT_FREESTANDING
  if (!InitializeNativeTarget()) {
    InitializeNativeTargetAsmPrinter();
    return;
  }
#endif
#endif

  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();
}

} // namespace

EJit::EJit(const Config &config) : config_(config) {
  EJIT_DIAG_VERBOSE("constructing: mode=%d opt=%d maxCache=%zu maxEntries=%u",
                    (int)config.compileMode, (int)config.optLevel,
                    config.maxCacheSize, (unsigned)config.maxCacheEntries);

  // Create all runtime components
  runtimeState_ = std::make_unique<EJitRuntimeState>();
  moduleLoader_ = std::make_unique<EJitModuleLoader>();

#ifndef EJIT_FREESTANDING
  if (config.enableLogger)
    logger_ = std::make_unique<EJitLogger>();
#endif

#ifndef EJIT_FREESTANDING
  EJitLogger *logger = logger_.get();
#else
  EJitLogger *logger = nullptr;
#endif
  compileDriver_ = std::make_unique<EJitCompileDriver>(
      config_, *runtimeState_, *moduleLoader_, logger);

  // Consume registration data from the staging store (constructor path).
  StoredData data = EJitRegistrationStore::instance().consume();

  // A constructor-phase callback (ejit_register_funcindex / _lifecycle) that
  // hit a capacity limit recorded an error before this instance existed; fold
  // it in so ejit_init fails rather than building a half-registered taskpool.
  if (RegistrationError ctorErr =
          EJitRegistrationStore::instance().consumeError();
      !ctorErr.ok())
    recordInitError(ctorErr.code, ctorErr.message, ctorErr.funcName);

  // Bare-metal / test fallback: if forced by config or no constructor data,
  // walk the static registry tables generated by PASS1/PASS2.
  if (config_.forceStaticRegistry || data.empty()) {
    if (config_.forceStaticRegistry)
      data = StoredData(); // discard any constructor data
    SmallVector<SymbolEntry, 8> tableSymbols;
    PeriodArrayRegistry &reg = runtimeState_->getRegistry();
    // Walk the linker-provided [start, stop) ranges. Each section holds a
    // concatenation of per-TU entry arrays; there is no sentinel.
    auto walkRange = [&](const ejit_reg_entry_t *Begin,
                         const ejit_reg_entry_t *End) {
      for (const ejit_reg_entry_t *e = Begin; e < End; ++e) {
        switch (e->type) {
        case EJIT_REG_BITCODE:
          if (!moduleLoader_->registerBitcode(
                  e->name1, static_cast<const uint8_t *>(e->ptr), e->size))
            recordInitError(EJIT_ERR_INVALID_PARAM,
                            "bitcode registration rejected (null/zero payload, "
                            "funcIndex capacity, or conflicting re-register)",
                            e->name1 ? e->name1 : "");
          break;
        case EJIT_REG_PERIOD_ARRAY:
          reg.registerArray(e->name1, e->name2, const_cast<void *>(e->ptr),
                            e->size);
          break;
        case EJIT_REG_STATIC_VAR:
          reg.registerStaticVar(e->name1, const_cast<void *>(e->ptr));
          break;
        case EJIT_REG_SYMBOL:
          tableSymbols.push_back({e->name1, const_cast<void *>(e->ptr)});
          break;
        case EJIT_REG_LIFECYCLE: {
          // Fill the wrapper's per-lifecycle dimType global with the slot
          // assigned by name. A null fixup pointer or an exhausted lifecycle
          // capacity is a hard init failure.
          if (!e->name1 || !e->ptr) {
            recordInitError(EJIT_ERR_INVALID_PARAM,
                            "lifecycle registration has a null fixup pointer",
                            e->name1 ? e->name1 : "");
            break;
          }
          uint32_t slot =
              EJitLifecycleRegistry::instance().resolveAssign(e->name1);
          *const_cast<uint32_t *>(static_cast<const uint32_t *>(e->ptr)) = slot;
          if (slot == kEJitInvalidDimType)
            recordInitError(EJIT_ERR_CACHE_FULL,
                            "lifecycle (dimType) capacity exhausted", e->name1);
          break;
        }
        case EJIT_REG_FUNCINDEX: {
          // Fill the wrapper's per-function dense-funcIndex global with the
          // index assigned by name. A null fixup pointer or an exhausted
          // funcIndex capacity is a hard init failure.
          if (!e->name1 || !e->ptr) {
            recordInitError(EJIT_ERR_INVALID_PARAM,
                            "funcIndex registration has a null fixup pointer",
                            e->name1 ? e->name1 : "");
            break;
          }
          uint32_t idx = EJitFuncRegistry::instance().resolveAssign(e->name1);
          *const_cast<uint32_t *>(static_cast<const uint32_t *>(e->ptr)) = idx;
          if (idx == kEJitInvalidFuncIndex)
            recordInitError(EJIT_ERR_CACHE_FULL,
                            "funcIndex capacity exhausted for function",
                            e->name1);
          break;
        }
        default:
          break;
        }
      }
    };
    walkRange(__start_ejit_bitcode, __stop_ejit_bitcode);
    walkRange(__start_ejit_period, __stop_ejit_period);
    for (auto &sym : tableSymbols)
      data.userSymbols.push_back(sym);
  } else {
    // Populate bitcode tracker (constructor path)
    for (auto &be : data.bitcodes)
      if (!moduleLoader_->registerBitcode(be.funcName, be.data, be.size))
        recordInitError(EJIT_ERR_INVALID_PARAM,
                        "bitcode registration rejected (null/zero payload, "
                        "funcIndex capacity, or conflicting re-register)",
                        be.funcName);

    // Populate period registry (constructor path)
    PeriodArrayRegistry &reg = runtimeState_->getRegistry();
    for (auto &pa : data.periodArrays)
      reg.registerArray(pa.periodName, pa.varName, pa.baseAddr, pa.arraySize);

    for (auto &sv : data.staticVars)
      reg.registerStaticVar(sv.varName, sv.varAddr);
  }

  // Create sync JIT engine (target must be initialized first).
  // Use InitializeAll* instead of InitializeNative* so that cross-compiled
  // builds (e.g. AArch64 target built on x86 host) also work correctly.
  initializeEJitTargets();
  EJIT_DIAG(
      "registered: bitcodes=%zu periodArrays=%zu staticVars=%zu symbols=%zu",
      data.bitcodes.size(), data.periodArrays.size(), data.staticVars.size(),
      data.userSymbols.size());
  auto engine = EJitOrcEngine::Create(config, runtimeState_->getRegistry(),
                                      *runtimeState_);
  bool engineReady = false;
  if (engine) {
    // Forward auto-registered user symbols to the engine.
    for (auto &sym : data.userSymbols)
      (*engine)->addUserSymbol(sym.name, sym.addr);
    compileDriver_->setJitEngine(std::move(*engine));
    engineReady = true;
    EJIT_DIAG("OrcJIT engine created successfully");
  } else {
    EJIT_DIAG("FAILED to create OrcJIT engine");
#ifndef EJIT_FREESTANDING
    std::string errStr;
    llvm::handleAllErrors(
        engine.takeError(),
        [&](const llvm::ErrorInfoBase &E) { errStr = E.message(); });
    if (logger_)
      logger_->log(EJIT_ERR_COMPILE_FAILED,
                   "Failed to create OrcJIT engine: " + errStr, "", "");
#else
    consumeError(engine.takeError());
#endif
  }

#ifdef EJIT_SRE_TASKPOOL
  // Deferred worker start (spec §3.4 single async worker): the taskpool worker
  // is created stopped (autoStartWorker=false) and only starts HERE, after all
  // registration data is consumed, the funcIndex/lifecycle fixup is done, and
  // there is no init failure. Registration is frozen first so the running
  // worker never races a lock-free registry write. Sync maps to taskpool Off
  // and does not need a worker. Async requires both a ready engine and a
  // successfully started worker; otherwise init fails instead of accepting
  // requests that can never be consumed.
  if (!initFailed_) {
    regPhase_ = RegistrationPhase::Frozen;
    if (config_.compileMode == CompileMode::Async) {
      EJIT_DIAG_VERBOSE("taskpool async init: engineReady=%u",
                        static_cast<unsigned>(engineReady));
      if (!engineReady)
        recordInitError(EJIT_ERR_COMPILE_FAILED,
                        "Async mode requires a ready ORC engine", "");
#ifdef EJIT_SRE_SHARED_TASKPOOL
      // Cross-core shared taskpool: run owner election and (if elected) start
      // the ONE shared worker. A clean failure (owner worker-start failed / ABI
      // mismatch) fails init rather than accepting requests no worker consumes.
      else if (!compileDriver_->startSharedTaskPool())
        recordInitError(EJIT_ERR_COMPILE_FAILED,
                        "shared taskpool init/election failed", "");
#else
      else if (!compileDriver_->startTaskPoolWorker())
        recordInitError(EJIT_ERR_COMPILE_FAILED,
                        "taskpool worker failed to start", "");
#endif
      else {
        EJIT_DIAG("taskpool async init complete: worker running");
        // PGO opt-in: arm the Tier-2 auto-trigger when Config::enablePgo is set.
        if (config_.enablePgo) {
          constexpr uint32_t kDefaultPgoThreshold = 64;
#ifdef EJIT_SRE_SHARED_TASKPOOL
          compileDriver_->sharedTaskPool()->setPgoEnabled(
              true, kDefaultPgoThreshold,
              config_.pgoMaxConcurrentProfiles);
#else
          compileDriver_->taskPool()->setPgoEnabled(true, kDefaultPgoThreshold);
#endif
        }
      }
    } else {
      EJIT_DIAG_VERBOSE("taskpool sync init complete: worker remains stopped");
    }
  }
#else
  (void)engineReady;
#endif
}

EJit::~EJit() {
  // Destroy in reverse order (compile driver holds references to other
  // components)
  compileDriver_.reset();
  moduleLoader_.reset();
#ifndef EJIT_FREESTANDING
  logger_.reset();
#endif
  runtimeState_.reset();
}

void EJit::recordInitError(int code, const std::string &message,
                           const std::string &funcName) {
  // Keep the first failure so the earliest root cause is reported.
  if (initFailed_)
    return;
  initFailed_ = true;
  initError_.code = code;
  initError_.message = message;
  initError_.funcName = funcName;
  // Any init failure drives the registration phase to Failed so it is never
  // left in a stale Frozen state (registrationFrozen() rejects both Frozen and
  // Failed; only Open permits registration).
  regPhase_ = RegistrationPhase::Failed;
  EJIT_DIAG("init error: code=%d %s (%s)", code, message.c_str(),
            funcName.c_str());
}

bool EJit::activate(const std::string &periodName, uint8_t cellIdx) {
#ifdef EJIT_SRE_TASKPOOL
  // Taskpool build: a registered lifecycle updates the time-window activation
  // state AND the SwitchController (kept consistent; setEnabled bumps the
  // version only when the enabled bit actually flips). A name that is a
  // registered period but not a JIT lifecycle still drives the legacy
  // time-window state only. A name that is neither is unknown -> clean error,
  // no state change.
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(periodName);
  if (dt != kEJitInvalidDimType) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
    // Cross-core: activation is a SHARED fact. Write the shared enabled/version
    // (visible to the owner worker, which compiles on a different core). The
    // owner-private runtimeState_ is NOT the JIT gate's source of truth here.
    // The shared SwitchController defaults to INACTIVE (enabled=0), so the
    // producer MUST call ejit_activate before the owner will compile a given
    // period instance. setEnabled(true) flips 0->1 + bumps version on first
    // activate; it is a no-op while already active, until a deactivate flips
    // the bit back.
    if (EJitSharedTaskPool *sp = sharedTaskPool())
      sp->setInstanceEnabled(dt, cellIdx, /*enabled=*/true);
#else
    runtimeState_->activate(periodName, cellIdx);
    if (EJitTaskPool *tp = taskPool())
      tp->switchController().setEnabled(dt, cellIdx, /*wantOn=*/true);
#endif
    return true;
  }
  if (!runtimeState_->getRegistry().getArrays(periodName)) {
    EJIT_DIAG("activate reject(%s,%u): unknown period (not a lifecycle/array)",
              periodName.c_str(), cellIdx);
    return false;
  }
  runtimeState_->activate(periodName, cellIdx);
  return true;
#else
  runtimeState_->activate(periodName, cellIdx);
  return true;
#endif
}

bool EJit::deactivate(const std::string &periodName, uint8_t cellIdx) {
#ifdef EJIT_SRE_TASKPOOL
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(periodName);
  if (dt != kEJitInvalidDimType) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
    // Cross-core: deactivation writes the shared enabled/version. The version
    // bump drives the three-layer lazy invalidation (producer early-reject,
    // worker checkpoint discard, cache-lookup version-mismatch miss) — see
    // shared compileOrGet / runCompile / cacheLookup. invalidateByPeriod below
    // still drains the per-instance LRU; the shared POD cache is invalidated by
    // version alone.
    if (EJitSharedTaskPool *sp = sharedTaskPool())
      sp->setInstanceEnabled(dt, cellIdx, /*enabled=*/false);
#else
    runtimeState_->deactivate(periodName, cellIdx);
    if (EJitTaskPool *tp = taskPool())
      tp->switchController().setEnabled(dt, cellIdx, /*wantOn=*/false);
#endif
    return true;
  }
  if (!runtimeState_->getRegistry().getArrays(periodName)) {
    EJIT_DIAG("deactivate reject(%s,%u): unknown period (not a lifecycle/array)",
              periodName.c_str(), cellIdx);
    return false;
  }
  runtimeState_->deactivate(periodName, cellIdx);
  return true;
#else
  runtimeState_->deactivate(periodName, cellIdx);
  return true;
#endif
}

bool EJit::activateAll(const std::string &periodName) {
#ifdef EJIT_SRE_TASKPOOL
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(periodName);
  if (dt != kEJitInvalidDimType) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
    if (EJitSharedTaskPool *sp = sharedTaskPool())
      for (uint32_t i = 0; i < EJitSwitchController::MAX_INSTANCES; ++i)
        sp->setInstanceEnabled(dt, i, /*enabled=*/true);
#else
    runtimeState_->activateAll(periodName);
    if (EJitTaskPool *tp = taskPool())
      for (uint32_t i = 0; i < EJitSwitchController::MAX_INSTANCES; ++i)
        tp->switchController().setEnabled(dt, i, /*wantOn=*/true);
#endif
    return true;
  }
  if (!runtimeState_->getRegistry().getArrays(periodName)) {
    EJIT_DIAG("activateAll reject(%s): unknown period", periodName.c_str());
    return false;
  }
  runtimeState_->activateAll(periodName);
  return true;
#else
  runtimeState_->activateAll(periodName);
  return true;
#endif
}

bool EJit::deactivateAll(const std::string &periodName) {
#ifdef EJIT_SRE_TASKPOOL
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(periodName);
  if (dt != kEJitInvalidDimType) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
    if (EJitSharedTaskPool *sp = sharedTaskPool())
      for (uint32_t i = 0; i < EJitSwitchController::MAX_INSTANCES; ++i)
        sp->setInstanceEnabled(dt, i, /*enabled=*/false);
#else
    runtimeState_->deactivateAll(periodName);
    if (EJitTaskPool *tp = taskPool())
      for (uint32_t i = 0; i < EJitSwitchController::MAX_INSTANCES; ++i)
        tp->switchController().setEnabled(dt, i, /*wantOn=*/false);
#endif
    return true;
  }
  if (!runtimeState_->getRegistry().getArrays(periodName)) {
    EJIT_DIAG("deactivateAll reject(%s): unknown period", periodName.c_str());
    return false;
  }
  runtimeState_->deactivateAll(periodName);
  return true;
#else
  runtimeState_->deactivateAll(periodName);
  return true;
#endif
}

bool EJit::isActive(const std::string &periodName, uint8_t cellIdx) const {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Cross-core: a registered lifecycle's activation lives in the shared
  // enabled bit (the producer's ejit_activate writes it). Query that, not the
  // owner-private runtimeState_, so ejit_is_active returns the same fact the
  // producer sees and the worker gates on. A non-lifecycle period name (a
  // plain period array without a dimType) still consults the private state.
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(periodName);
  if (dt != kEJitInvalidDimType) {
    if (const EJitSharedTaskPool *sp = sharedTaskPool())
      return sp->isInstanceActive(dt, cellIdx);
  }
#endif
  return runtimeState_->isActive(periodName, cellIdx);
}

void EJit::clearCache() { /* Legacy LRU cache retired */ }

void EJit::invalidateByPeriod(const std::string &periodName, uint8_t cellIdx) {
  (void)periodName; (void)cellIdx;
}

void EJit::invalidateAllByPeriod(const std::string &periodName) {
  (void)periodName;
}

void EJit::registerSymbol(const std::string &name, void *addr) {
  if (compileDriver_)
    compileDriver_->registerSymbol(name, addr);
}

bool EJit::registerBitcode(const std::string &funcName, const uint8_t *data,
                           size_t size) {
  // Post-init runtime registration. The bool propagates to the caller (the C
  // ABI records a failure into the registration-error sink); construction-time
  // registration validates separately in the constructor (recordInitError).
  if (!moduleLoader_) {
    EJIT_DIAG("registerBitcode reject func=%s: no module loader", funcName.c_str());
    return false;
  }
#ifdef EJIT_SRE_TASKPOOL
  // Frozen after init: the worker reads the loader/registries lock-free, so no
  // runtime registration is allowed (nothing is mutated on rejection).
  if (registrationFrozen()) {
    EJIT_DIAG("registerBitcode reject func=%s: registration frozen after init",
              funcName.c_str());
    return false;
  }
#endif
  return moduleLoader_->registerBitcode(funcName, data, size);
}

bool EJit::registerPeriodArray(const std::string &periodName,
                               const std::string &varName, void *baseAddr,
                               uint64_t arraySize) {
  if (!runtimeState_) {
    EJIT_DIAG("registerPeriodArray reject period=%s: no runtime state",
              periodName.c_str());
    return false;
  }
#ifdef EJIT_SRE_TASKPOOL
  if (registrationFrozen()) {
    EJIT_DIAG("registerPeriodArray reject period=%s: registration frozen",
              periodName.c_str());
    return false;
  }
#endif
  runtimeState_->getRegistry().registerArray(periodName, varName, baseAddr,
                                             arraySize);
  return true;
}

bool EJit::registerStaticVar(const std::string &varName, void *varAddr) {
  if (!runtimeState_) {
    EJIT_DIAG("registerStaticVar reject var=%s: no runtime state",
              varName.c_str());
    return false;
  }
#ifdef EJIT_SRE_TASKPOOL
  if (registrationFrozen()) {
    EJIT_DIAG("registerStaticVar reject var=%s: registration frozen",
              varName.c_str());
    return false;
  }
#endif
  runtimeState_->getRegistry().registerStaticVar(varName, varAddr);
  return true;
}

bool EJit::setCompileMode(CompileMode mode) {
  EJitTaskPool *tp = taskPool();
  if (!tp)
    return false;

  if (mode == CompileMode::Async) {
    // Do not expose Async until both the compiler engine and consumer exist.
    // Failure preserves the old mode, so callers cannot enqueue permanent
    // pending work into a worker-less taskpool.
    if (!compileDriver_->hasJitEngine()) {
      EJIT_DIAG("compile mode switch rejected: async without engine");
      return false;
    }
#ifndef EJIT_SRE_SHARED_TASKPOOL
    // Private taskpool: the worker is local to this instance, so a runtime
    // switch to Async must start it here. In a shared build the single worker
    // is cross-core and owner-controlled (started by owner election during
    // init), so a mode flip must NOT start a per-instance worker.
    if (!tp->isWorkerRunning() && !compileDriver_->startTaskPoolWorker()) {
      EJIT_DIAG("compile mode switch rejected: worker start failed");
      return false;
    }
#endif
    tp->switchController().setMode(EJitCompileMode::Async);
    EJIT_DIAG("compile mode switched to async");
  } else if (mode == CompileMode::Off) {
    tp->switchController().setMode(EJitCompileMode::Off);
    EJIT_DIAG("compile mode switched to off (no JIT)");
  } else {
    tp->switchController().setMode(EJitCompileMode::Sync);
#ifndef EJIT_SRE_SHARED_TASKPOOL
    // Private taskpool: stop this instance's local worker. In a shared build
    // the single worker is owner-controlled and shared across cores, so a mode
    // flip is a control flag only and must NOT stop the shared worker.
    compileDriver_->stopTaskPoolWorker();
    EJIT_DIAG("compile mode switched to sync; taskpool worker stopped");
#else
    EJIT_DIAG("compile mode switched to sync");
#endif
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Compile mode is CROSS-CORE SHARED runtime state. Publish it to the shared
  // blob with release semantics so every core's compileOrGet() observes the new
  // mode; engine/worker ownership stays owner-controlled (a mode flip never
  // starts/stops the shared worker or re-runs owner election).
  if (EJitSharedTaskPool *sp = sharedTaskPool())
    sp->setSharedMode(mode == CompileMode::Async   ? EJitCompileMode::Async
                     : mode == CompileMode::Sync   ? EJitCompileMode::Sync
                                                   : EJitCompileMode::Off);
#endif
  config_.compileMode = mode;
  return true;
}

CompileMode EJit::getCompileMode() const {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // In a shared build the cross-core shared state is the source of truth for the
  // runtime compile mode, so a peer/other core observes the owner's last switch
  // rather than this instance's stale local config_.
  if (compileDriver_) {
    EJitSharedTaskPool *sp = compileDriver_->sharedTaskPool();
    EJitCompileMode m = sp->getSharedMode();
    return m == EJitCompileMode::Async ? CompileMode::Async
         : m == EJitCompileMode::Sync ? CompileMode::Sync
                                      : CompileMode::Off;
  }
#endif
  return config_.compileMode;
}

void EJit::setOptimizationLevel(OptimizationLevel level) {
  config_.optLevel = level;
}

OptimizationLevel EJit::getOptimizationLevel() const {
  return config_.optLevel;
}

const EJitError *EJit::getLastError() const {
#ifdef EJIT_FREESTANDING
  return nullptr;
#else
  if (!logger_)
    return nullptr;
  // Copy into stable storage so the caller gets a snapshot that won't be
  // overwritten by concurrent log() calls on other threads.
  static thread_local EJitError lastErr;
  if (logger_->copyLastError(lastErr))
    return &lastErr;
  return nullptr;
#endif
}

#ifdef EJIT_SRE_TASKPOOL
EJitTaskPool *EJit::taskPool() {
  return compileDriver_ ? compileDriver_->taskPool() : nullptr;
}
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
EJitSharedTaskPool *EJit::sharedTaskPool() {
  return compileDriver_ ? compileDriver_->sharedTaskPool() : nullptr;
}
const EJitSharedTaskPool *EJit::sharedTaskPool() const {
  return compileDriver_ ? compileDriver_->sharedTaskPool() : nullptr;
}
#endif

void EJit::printRegistry() const {
  const PeriodArrayRegistry &reg = runtimeState_->getRegistry();
  EJIT_DIAG("registry: funcIndexes=%u lifecycles=%u",
            EJitFuncRegistry::instance().count(),
            EJitLifecycleRegistry::instance().count());

  EJIT_DIAG("registry: bitcodes (%u):", EJitFuncRegistry::instance().count());
  for (uint32_t idx = 0; idx < EJitFuncRegistry::instance().count(); ++idx) {
    const std::string &name = moduleLoader_->getFuncNameByFuncIdx(idx);
    if (name.empty())
      continue;
    auto bc = moduleLoader_->getBitcodeByFuncIdx(idx);
    size_t sz = bc ? bc->size() : 0;
    if (!bc)
      consumeError(bc.takeError());
    EJIT_DIAG("  idx=%u name=%s size=%zu", idx, name.c_str(), sz);
  }

  EJIT_DIAG("registry: period arrays:");
  for (const auto &kv : reg.arraysByPeriod())
    for (const auto &info : kv.second)
      EJIT_DIAG("  period=%s var=%s base=%p size=%zu", kv.first.c_str(),
                info.varName.c_str(), info.baseAddr, info.arraySize);

  EJIT_DIAG("registry: static vars (%zu):", reg.getStaticVars().size());
  for (const auto &sv : reg.getStaticVars())
    EJIT_DIAG("  var=%s addr=%p", sv.varName.c_str(), sv.varAddr);
}

void EJit::printFuncMeta(const std::string &funcName) {
  uint32_t idx = EJitFuncRegistry::instance().lookup(funcName);
  if (idx == kEJitInvalidFuncIndex) {
    EJIT_DIAG("func_meta: name=%s not registered", funcName.c_str());
    return;
  }
  auto bc = moduleLoader_->getBitcodeByFuncIdx(idx);
  if (!bc) {
    EJIT_DIAG("func_meta: name=%s funcIdx=%u bitcode lookup error",
              funcName.c_str(), idx);
    consumeError(bc.takeError());
    return;
  }
  auto Ctx = std::make_unique<LLVMContext>();
  auto Buf = MemoryBuffer::getMemBuffer(*bc, "meta_" + std::to_string(idx) + ".bc");
  auto MOrErr = parseBitcodeFile(Buf->getMemBufferRef(), *Ctx);
  if (!MOrErr) {
    EJIT_DIAG("func_meta: name=%s funcIdx=%u parse bitcode error",
              funcName.c_str(), idx);
    consumeError(MOrErr.takeError());
    return;
  }
  Function *F = (*MOrErr)->getFunction(funcName);
  EJIT_DIAG("func_meta name=%s funcIdx=%u found=%d", funcName.c_str(), idx,
            F && !F->isDeclaration() ? 1 : 0);
  if (!F || F->isDeclaration()) {
    EJIT_DIAG("  (no definition in bitcode)");
    return;
  }
  MDNode *MD = F->getMetadata(MD_EJIT_METADATA);
  if (!MD) {
    EJIT_DIAG("  (no !ejit.metadata)");
    return;
  }
  EJIT_DIAG("  ejit_entry=%d",
            hasMDStringEntry(MD, TAG_EJIT_ENTRY) ? 1 : 0);
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() == 0)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    if (!Tag)
      continue;
    StringRef tag = Tag->getString();
    // Render the remaining operands: MDString as their string, ConstantInt as
    // its integer value, anything else as a placeholder.
    std::string rest;
    raw_string_ostream OS(rest);
    for (unsigned i = 1; i < Sub->getNumOperands(); ++i) {
      if (i > 1)
        OS << ",";
      if (auto *S = dyn_cast<MDString>(Sub->getOperand(i)))
        OS << S->getString();
      else if (auto *C = dyn_cast<ConstantAsMetadata>(Sub->getOperand(i)))
        if (auto *CI = dyn_cast<ConstantInt>(C->getValue()))
          OS << CI->getZExtValue();
        else
          OS << "?";
      else
        OS << "?";
    }
    OS.flush();
    EJIT_DIAG("  %s %s", tag.str().c_str(), rest.c_str());
  }
}

bool EJit::getCodePoolStats(ejit_code_pool_stats_t *out) const {
  if (!out)
    return false;
#if defined(EJIT_SRE_SHARED_TASKPOOL) && defined(EJIT_SRE_CODE_POOL)
  // Shared build: read the owner-published mirror so every core (owner and
  // non-owner) sees the SAME code-pool stats. The real pools are owner-private,
  // so a non-owner's per-core manager is empty (pools=0) — the mirror is the
  // only cross-core-consistent source.
  if (const EJitSharedTaskPool *sp = sharedTaskPool()) {
    EJitCodePoolStatsOut s{};
    if (sp->readCodePoolStats(&s)) {
      out->poolCount = s.poolCount;
      out->sealedCount = s.sealedCount;
      out->activeCount = s.activeCount;
      out->usedBytes = s.usedBytes;
      out->reservedBytes = s.reservedBytes;
      out->wastedBytes = s.wastedBytes;
      out->sealInvocations = s.sealInvocations;
      out->splitInvocations = s.splitInvocations;
      out->finalizedRangeCount = s.finalizedRangeCount;
      return true;
    }
  }
#endif
#ifdef EJIT_SRE_CODE_POOL
  if (!compileDriver_)
    return false;
  EJitOrcEngine *engine = compileDriver_->getJitEngine();
  if (!engine)
    return false;
  EJitCodePoolManager::Stats s = engine->getCodePoolStats();
  out->poolCount = s.poolCount;
  out->sealedCount = s.sealedCount;
  out->activeCount = s.activeCount;
  out->usedBytes = s.usedBytes;
  out->reservedBytes = s.reservedBytes;
  out->wastedBytes = s.wastedBytes;
  out->sealInvocations = s.sealInvocations;
  out->splitInvocations = s.splitInvocations;
  out->finalizedRangeCount = s.finalizedRangeCount;
  return true;
#else
  return false;
#endif
}

void EJit::printCodePoolStats() const {
#ifdef EJIT_SRE_CODE_POOL
  ejit_code_pool_stats_t s{};
  if (!getCodePoolStats(&s)) {
    EJIT_DIAG("code pool: not available (no engine)");
    return;
  }
  EJIT_DIAG("code pool: pools=%llu sealed=%llu active=%llu",
            (unsigned long long)s.poolCount, (unsigned long long)s.sealedCount,
            (unsigned long long)s.activeCount);
  EJIT_DIAG("  bytes used=%llu reserved=%llu wasted=%llu",
            (unsigned long long)s.usedBytes,
            (unsigned long long)s.reservedBytes,
            (unsigned long long)s.wastedBytes);
  EJIT_DIAG("  sealInvocations=%llu splitInvocations=%llu finalizedRanges=%llu",
            (unsigned long long)s.sealInvocations,
            (unsigned long long)s.splitInvocations,
            (unsigned long long)s.finalizedRangeCount);
#else
  EJIT_DIAG("code pool: EJIT_SRE_CODE_POOL not enabled");
#endif
}

void EJit::printActive() const {
  const PeriodArrayRegistry &reg = runtimeState_->getRegistry();
  EJIT_DIAG("active periods:");
  // Query the same isActive() path the JIT gate uses (shared SwitchController
  // in shared-taskpool builds, per-instance arrayStates_ otherwise), so the
  // printed view matches the compile decision.
  for (const auto &kv : reg.arraysByPeriod()) {
    const std::string &period = kv.first;
    uint32_t activeCells = 0;
    for (uint32_t cell = 0; cell < kEJitMaxInstances; ++cell) {
      if (isActive(period, static_cast<uint8_t>(cell))) {
        EJIT_DIAG("  period=%s cell=%u", period.c_str(), cell);
        ++activeCells;
      }
    }
    if (activeCells == 0)
      EJIT_DIAG("  period=%s (no active cells)", period.c_str());
  }
  // The built-in "static" time window is always active.
  EJIT_DIAG("active static vars: %zu (always active)", reg.getStaticVars().size());
}
