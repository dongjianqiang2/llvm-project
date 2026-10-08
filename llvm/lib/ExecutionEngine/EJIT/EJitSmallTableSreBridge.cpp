//===-- EJitSmallTableSreBridge.cpp - opt-in, POD owner command bridge ------===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "llvm/ExecutionEngine/EJIT/EJitSmallTableSreBridge.h"
#include "EJitSmallTableSreBridgeInternal.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableHost.h"
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreTask.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/MemoryBuffer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <type_traits>

using namespace llvm;
using namespace llvm::ejit;

#ifdef EJIT_SRE_SHARED_TASKPOOL
namespace {
constexpr uint32_t CommandSlots = 8, LeaseSlots = 32;
constexpr uint64_t BridgeEpochBit = uint64_t{1} << 63;
enum CommandState : uint32_t { Free, Claimed, Queued, Started, Done, Cancelled };
enum Operation : uint32_t { Request = 1, Snapshot, Finish, Cancel, Print,
                           PrepareExecution, CommitExecution, LeaveExecution };
struct TableRange { uintptr_t address; uint64_t bytes; };
struct Command {
  uint32_t state;
  uint32_t operation;
  uint32_t function;
  int32_t result;
  uint32_t numDims;
  uint32_t boundCount;
  uint32_t ownerGeneration;
  uint32_t tableCount;
  uint64_t ownerIdentity;
  uint64_t epoch;
  uint64_t ticket;
  uintptr_t entry;
  uintptr_t sourceAddress;
  uint64_t sourceBytes;
  uintptr_t sourceState;
  ejit_dim_pair_t dims[EJIT_STAB_SRE_MAX_DIMS];
  ejit_bound_ptr_t bounds[8];
  EJitCompiledCodeInfo code;
  TableRange tables[32];
  ejit_small_table_sre_request_t request;
  ejit_small_table_sre_snapshot_t snapshot;
};
static_assert(std::is_trivially_copyable<Command>::value,
              "inter-core command must contain only by-value POD metadata");
struct SharedControl {
  uint32_t enabled;
  uint32_t ownerGeneration;
  uint32_t ownerCore;
  uint32_t function;
  uint64_t ownerIdentity;
  uint64_t workerTask;
  uint64_t policyEpoch;
  uint64_t nextTicket;
  uint64_t leaseTokens[LeaseSlots];
  Command commands[CommandSlots];
};
// Additive shared POD, not a change to the released taskpool blob layout.
EJIT_SHARED_SECTION SharedControl Shared = {};
ejit_small_table_sre_bindings_t LocalBindings = {};
uint32_t LocalPrepared = 0;
uint64_t LocalWorkerTask = 0;
EJitSharedTaskPool *LocalWorkerPool = nullptr;
Command LocalCommands[CommandSlots] = {}; // private scratch, never shared

uint32_t load32(const uint32_t *P) { return EJitAtomicRef<uint32_t>(*const_cast<uint32_t *>(P)).loadAcquire(); }
uint64_t load64(const uint64_t *P) { return EJitAtomicRef<uint64_t>(*const_cast<uint64_t *>(P)).loadAcquire(); }
void store32(uint32_t *P, uint32_t V) { EJitAtomicRef<uint32_t>(*P).storeRelease(V); }
void store64(uint64_t *P, uint64_t V) { EJitAtomicRef<uint64_t>(*P).storeRelease(V); }
bool cas32(uint32_t *P, uint32_t &Expected, uint32_t Desired) {
  return EJitAtomicRef<uint32_t>(*P).compareExchange(Expected, Desired);
}
bool boundedString(const char *S, size_t N) {
  return S && std::memchr(S, 0, N) && S[0];
}
void copyText(char *To, size_t Capacity, StringRef Text) {
  const size_t N = std::min(Capacity - 1, Text.size());
  std::memcpy(To, Text.data(), N);
  To[N] = 0;
}
void delay() { LocalBindings.delay_ticks(LocalBindings.context, 1); }
bool isActualBridgeWorker(void *Pool) {
  return LocalWorkerPool == Pool && LocalWorkerTask && load32(&LocalPrepared) &&
         LocalBindings.current_task_id(LocalBindings.context) == LocalWorkerTask;
}
bool dataReady(uintptr_t Address, uint64_t Bytes, uint32_t Access) {
  return Address && Bytes && Bytes <= UINTPTR_MAX - Address &&
         load32(&LocalPrepared) && LocalBindings.prepare_shared_data(
             LocalBindings.context, Address, Bytes, Access) == 0;
}
struct LocalCommand {
  Command *value = nullptr;
  LocalCommand() {
    for (auto &C : LocalCommands) {
      uint32_t Expected = Free;
      if (cas32(&C.state, Expected, Claimed)) {
        value = &C;
        std::memset(reinterpret_cast<uint8_t *>(&C) + offsetof(Command, operation),
                    0, sizeof(C) - offsetof(Command, operation));
        break;
      }
    }
  }
  ~LocalCommand() { if (value) store32(&value->state, Free); }
};

class SreFacts final : public EJitSmallTableFactSource {
public:
  explicit SreFacts(const ejit_small_table_sre_request_t &R) : request_(R) {
    for (uint32_t I = 0; I < R.numMembers; ++I) {
      EJitSmallTableReadyMember M;
      for (uint32_t D = 0; D < R.numDims; ++D)
        M.indices.push_back(R.members[I].coordinate[D]);
      M.configGeneration = R.members[I].configurationGeneration;
      M.fieldsInitialized = R.members[I].fieldsInitialized != 0;
      members_.push_back(std::move(M));
    }
  }
  StringRef label() const override { return "sre-explicit-config-commit.pr231"; }
  uint64_t domainEpoch() const override { return request_.sourceEpoch; }
  uint64_t configurationRevision() const override {
    return load64(&state()->revision);
  }
  bool epochCurrent(uint64_t Epoch) const override {
    return Epoch == request_.sourceEpoch &&
           load64(&state()->epoch) == Epoch &&
           load64(&state()->revision) == request_.configurationRevision;
  }
  bool coversDeclaredDomain() const override { return request_.domainCoverage; }
  ArrayRef<EJitSmallTableReadyMember> readyMembers() const override {
    return members_;
  }
  std::unique_ptr<EJitSmallTableReadBorrow>
  borrow(StringRef Name, std::string &Why) override {
    if (Name != request_.sourceVarName || !epochCurrent(request_.sourceEpoch) ||
        load32(&state()->writerBlocked)) {
      Why = "SRE source commit is stale, write-locked or not the registered source";
      return nullptr;
    }
    uint32_t Readers = load32(&state()->readers);
    do {
      if (Readers == UINT32_MAX) {
        Why = "SRE source borrow reader count exhausted";
        return nullptr;
      }
    } while (!cas32(&state()->readers, Readers, Readers + 1));
    if (load32(&state()->writerBlocked) || !epochCurrent(request_.sourceEpoch) ||
        !dataReady(request_.sourceAddress, request_.sourceBytes,
                   EJIT_STAB_SRE_DATA_READ)) {
      EJitAtomicRef<uint32_t>(state()->readers).fetchSub(1u);
      Why = "SRE protected read or source mapping refused";
      return nullptr;
    }
    return std::make_unique<EJitSmallTableReadBorrow>(
        this, request_.sourceEpoch,
        EJitSmallTableSource{reinterpret_cast<const uint8_t *>(request_.sourceAddress),
                            request_.sourceBytes});
  }
  void onBorrowReleased(EJitSmallTableReadBorrow *) override {
    EJitAtomicRef<uint32_t>(state()->readers).fetchSub(1u);
  }
  ejit_small_table_sre_source_state_t *state() const {
    return reinterpret_cast<ejit_small_table_sre_source_state_t *>(request_.sourceState);
  }
  const ejit_small_table_sre_request_t &request() const { return request_; }
private:
  ejit_small_table_sre_request_t request_;
  std::vector<EJitSmallTableReadyMember> members_;
};

struct Lease {
  uint64_t ticket = 0;
  uint64_t hostTicket = 0;
  uint64_t epoch = 0;
  uint64_t ownerIdentity = 0;
  uint32_t generation = 0;
  uint32_t ownerCore = UINT32_MAX;
  uint32_t function = 0;
  bool executing = false;
  EJit *runtime = nullptr; // OWNER-private: never placed in Shared.
  EJitSmallTableHost *host = nullptr;
  void *entry = nullptr;
  uint32_t numDims = 0;
  ejit_dim_pair_t dims[4] = {};
  uint32_t versions[4] = {};
};
struct OwnerControl {
  EJit *runtime = nullptr;
  std::shared_ptr<SreFacts> facts;
  uint64_t codeGeneration = 0;
  std::vector<std::string> counterNames;
  std::vector<PgoCounterRef> counterRefs;
  ejit_small_table_sre_snapshot_t frozen = {};
  bool fullProfileValid = false;
  bool finishing = false;
  bool retirementObserved = false;
  Lease leases[LeaseSlots];
};
OwnerControl &owner() { static OwnerControl O; return O; }
Lease *lease(uint64_t Ticket) {
  for (Lease &L : owner().leases)
    if (L.ticket == Ticket && Ticket)
      return &L;
  return nullptr;
}
void closeLease(Lease &L) {
  OwnerControl &O = owner();
  if (!O.retirementObserved && O.runtime == L.runtime &&
      smallTableSreLocalRuntime() != L.runtime) {
    // Runtime shutdown detaches the facade without destroying the still-pinned
    // owner. Settle the CURRENT sampling session logically on its real worker
    // before delivering this actual completion. Otherwise an open window can
    // retain its source borrow even after its last physical call returned.
    // cancel itself cannot release an in-flight borrow/table/code generation.
    O.retirementObserved = true;
    if (auto *Host = O.runtime->smallTableHost())
      Host->cancel("SRE facade logically shut down; exact physical leave follows");
  }
  store64(&Shared.leaseTokens[static_cast<size_t>(&L - owner().leases)], 0);
  // Route through the owner-local retained registry; cancellation did not leave.
  if (L.hostTicket)
    ejit_stab_leave(L.hostTicket);
  EJit *Runtime = L.runtime;
  L = Lease();
  if (Runtime)
    releaseSmallTableSreRuntime(Runtime);
}
uint64_t freshTicket() {
  uint64_t N = load64(&Shared.nextTicket);
  for (;;) {
    if (N >= BridgeEpochBit - 1)
      return 0;
    const uint64_t Next = N + 1;
    if (EJitAtomicRef<uint64_t>(Shared.nextTicket).compareExchange(N, Next))
      return Next | BridgeEpochBit;
  }
}
bool functionMatches(uint32_t F) {
  return F == UINT32_MAX || F == load32(&Shared.function);
}

int fail(Command &C, int Code, StringRef Why) {
  C.result = Code;
  C.snapshot.status = Code;
  copyText(C.snapshot.reason, sizeof(C.snapshot.reason), Why);
  EJIT_DIAG("small-table SRE bridge refused: %s", C.snapshot.reason);
  return Code;
}

bool captureCounters(ejit_small_table_sre_snapshot_t &S, std::string &Why) {
  OwnerControl &O = owner();
  if (O.counterRefs.empty() || O.counterRefs.size() > EJIT_STAB_SRE_MAX_COUNTERS) {
    Why = "missing or out-of-bounds complete counter inventory";
    return false;
  }
  S.expectedCounterPairs = O.counterRefs.size();
  S.counterPairs = S.counterWordCount = 0;
  S.rootEntryCount = 0;
  S.rootEntryCountValid = 0;
  S.countersDigest = 1469598103934665603ULL;
  auto Mix = [&](uint64_t V) { S.countersDigest = (S.countersDigest ^ V) * 1099511628211ULL; };
  for (const PgoCounterRef &R : O.counterRefs) {
    if (!R.pgoName || !R.profcAddr || !R.profdAddr ||
        std::strlen(R.pgoName) >= EJIT_STAB_SRE_NAME_BYTES) {
      Why = "missing expected counter/data pair or unrepresentable name";
      return false;
    }
    const auto *Header = reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(R.profdAddr);
    const uint32_t N = Header->NumCounters;
    if (!N || N > EJIT_STAB_SRE_MAX_COUNTER_WORDS - S.counterWordCount) {
      Why = "actual counter inventory exceeds exact snapshot bounds";
      return false;
    }
    auto &Record = S.counters[S.counterPairs++];
    copyText(Record.name, sizeof(Record.name), R.pgoName);
    Record.hash = Header->FuncHash;
    Record.firstWord = S.counterWordCount;
    Record.wordCount = N;
    const auto *Counts = reinterpret_cast<const uint64_t *>(R.profcAddr);
    for (uint32_t I = 0; I < N; ++I) {
      S.counts[S.counterWordCount++] = Counts[I];
      Mix(Counts[I]);
    }
    Mix(Record.hash);
    for (const char *P = R.pgoName; *P; ++P) Mix(static_cast<unsigned char>(*P));
  }
  return true;
}

bool validateProfile(const EJitSmallTableProfileBundle &Bundle,
                     const ejit_small_table_sre_snapshot_t &S,
                     std::string &Why) {
  if (Bundle.counters.size() != S.expectedCounterPairs ||
      S.counterPairs != S.expectedCounterPairs || Bundle.profileData.empty()) {
    Why = "frozen profile does not contain the complete expected inventory";
    return false;
  }
  // Runtime freeze synthesizes an indexed InstrProfWriter buffer. Select that
  // exact format, reject others, and do not pull unrelated MachO/COFF readers
  // into the trimmed freestanding runtime closure.
  auto Reader = IndexedInstrProfReader::create(MemoryBuffer::getMemBufferCopy(Bundle.profileData));
  if (!Reader) { Why = toString(Reader.takeError()); return false; }
  std::set<std::string> Seen;
  for (const auto &Record : **Reader) {
    if (!Seen.insert(Record.Name.str()).second) {
      Why = "duplicate frozen profile record"; return false;
    }
    const ejit_small_table_sre_counter_t *Expected = nullptr;
    for (uint32_t I = 0; I < S.counterPairs; ++I)
      if (Record.Name == S.counters[I].name) Expected = &S.counters[I];
    if (!Expected || Record.Hash != Expected->hash ||
        Record.Counts.size() != Expected->wordCount ||
        !std::equal(Record.Counts.begin(), Record.Counts.end(),
                    &S.counts[Expected->firstWord])) {
      Why = "frozen profile name/hash/counts differ from actual full T1 capture";
      return false;
    }
  }
  if ((*Reader)->hasError() || Seen.size() != S.expectedCounterPairs) {
    Why = "malformed or incomplete frozen profile"; return false;
  }
  return true;
}

int setupRequest(Command &C) {
  const auto &R = C.request;
  if (R.abiVersion != EJIT_STAB_SRE_ABI_VERSION || R.structSize != sizeof(R) ||
      !boundedString(R.entryName, sizeof(R.entryName)) ||
      !boundedString(R.sourceVarName, sizeof(R.sourceVarName)) ||
      !R.sourceEpoch || !R.configurationRevision || !R.codeGeneration ||
      !R.sampleLimit || R.sampleLimit > 65536 || !R.domainCoverage ||
      !R.numDims || R.numDims > 4 || !R.numMembers || R.numMembers > 32 ||
      !R.sourceState || R.sourceState % alignof(ejit_small_table_sre_source_state_t) ||
      !dataReady(R.sourceState, sizeof(ejit_small_table_sre_source_state_t),
                 EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE) ||
      !dataReady(R.sourceAddress, R.sourceBytes, EJIT_STAB_SRE_DATA_READ))
    return fail(C, EJIT_STAB_SRE_INVALID, "invalid SRE request or shared-source mapping");
  EJit *Runtime = smallTableSreLocalRuntime();
  if (!Runtime || Runtime->sharedTaskPool() != LocalWorkerPool ||
      !LocalWorkerPool->isCurrentOwnerWorker())
    return fail(C, EJIT_STAB_SRE_BLOCKED, "SRE request is not on the real live owner worker");
  const uint32_t Func = EJitFuncRegistry::instance().lookup(R.entryName);
  if (Func >= EJitFuncRegistry::instance().count() ||
      Runtime->moduleLoader().getFuncNameByFuncIdx(Func) != R.entryName)
    return fail(C, EJIT_STAB_SRE_INVALID, "SRE request entry has no exact registered dense function identity");
  const auto *Source = Runtime->getRegistry().getArrayInfo(R.sourceVarName);
  if (!Source || reinterpret_cast<uintptr_t>(Source->baseAddr) != R.sourceAddress)
    return fail(C, EJIT_STAB_SRE_INVALID, "SRE request source differs from the registered period-array identity");
  SmallVector<EJitSmallTableDim, 4> Dims;
  SmallVector<std::string, 4> Periods;
  for (uint32_t I = 0; I < R.numDims; ++I) {
    const auto &D = R.dims[I];
    if (!boundedString(D.periodName, sizeof(D.periodName)) || !D.extent ||
        D.modulus > UINT32_MAX)
      return fail(C, EJIT_STAB_SRE_INVALID, "SRE request dimension is malformed");
    Dims.push_back({D.modulus ? EJitSmallTableDim::Kind::ModuloArgument
                             : EJitSmallTableDim::Kind::Argument,
                   D.argumentIndex, static_cast<uint32_t>(D.modulus), D.extent});
    Periods.push_back(D.periodName);
  }
  for (uint32_t I = 0; I < R.numMembers; ++I) {
    if (!R.members[I].configurationGeneration || !R.members[I].fieldsInitialized)
      return fail(C, EJIT_STAB_SRE_INVALID, "SRE request contains an incomplete configuration member");
    for (uint32_t D = 0; D < R.numDims; ++D)
      if (R.members[I].coordinate[D] >= R.dims[D].extent)
        return fail(C, EJIT_STAB_SRE_INVALID, "SRE ready coordinate leaves its declared dimension");
  }
  auto Facts = std::make_shared<SreFacts>(R);
  if (!Facts->epochCurrent(R.sourceEpoch) || load32(&Facts->state()->writerBlocked))
    return fail(C, EJIT_STAB_SRE_BLOCKED, "SRE request configuration commit is stale or write-locked");
  auto Bitcode = Runtime->moduleLoader().getBitcodeByFuncIdx(Func);
  if (!Bitcode) return fail(C, EJIT_STAB_SRE_FAILED, toString(Bitcode.takeError()));
  LLVMContext Context;
  auto Module = parseBitcodeFile(MemoryBufferRef(*Bitcode, R.entryName), Context);
  if (!Module) return fail(C, EJIT_STAB_SRE_FAILED, toString(Module.takeError()));
  auto *SourceGlobal = (*Module)->getNamedGlobal(R.sourceVarName);
  auto *SourceArray = SourceGlobal ? dyn_cast<ArrayType>(SourceGlobal->getValueType()) : nullptr;
  if (!SourceArray || !SourceArray->isSized() ||
      (*Module)->getDataLayoutStr().empty())
    return fail(C, EJIT_STAB_SRE_INVALID, "registered bitcode has no sized target source array");
  const TypeSize TargetBytes = (*Module)->getDataLayout().getTypeAllocSize(SourceArray);
  if (TargetBytes.isScalable() || TargetBytes.getFixedValue() != R.sourceBytes ||
      Source->periodName != R.dims[0].periodName ||
      (Source->arraySize != SourceArray->getNumElements() &&
       Source->arraySize != TargetBytes.getFixedValue()))
    return fail(C, EJIT_STAB_SRE_INVALID,
                "registered source extent or requested bytes differ from actual target array layout");
  // Compiler registration is the OUTER element count, not a byte extent.
  // Existing explicit C clients also registered bytes; accept that legacy form
  // only when it equals the same independently checked target allocation size.
  // Never use either untyped registry value to authorize source byte reads.
  OwnerControl &O = owner();
  // Logical teardown parks old executions in the EXISTING retained-owner
  // registry. Their opaque bridge leases still name their exact physical call.
  Runtime->disableSmallTable();
  O.runtime = Runtime;
  O.facts = std::move(Facts);
  O.fullProfileValid = false;
  O.finishing = false;
  O.retirementObserved = false;
  O.frozen = {};
  O.counterNames.clear();
  O.counterRefs.clear();
  O.codeGeneration = R.codeGeneration;
  store32(&Shared.function, Func);
  smallTableSrePolicyChanged();
  EJitSmallTableHost::Options Opts;
  Opts.runtime.sampling.aggregateLimit = R.sampleLimit;
  Opts.runtime.sampling.freezeWaitMillis = 1;
  if (Error E = Runtime->enableSmallTable(O.facts, Opts)) {
    return fail(C, EJIT_STAB_SRE_BLOCKED, toString(std::move(E)));
  }
  auto *Host = Runtime->smallTableHost();
  EJitSmallTableHost::EntryRequest Req;
  Req.module = Module->get();
  Req.entryName = R.entryName;
  Req.funcIndex = Func;
  Req.sourceVarName = R.sourceVarName;
  Req.dims = Dims;
  Req.dimPeriodNames = Periods;
  Req.codeGeneration = R.codeGeneration;
  std::string Why;
  if (Error E = Host->requestEntry(Req, reinterpret_cast<void *>(R.aotEntry), Why)) {
    const std::string Detail = toString(std::move(E));
    Runtime->disableSmallTable();
    return fail(C, EJIT_STAB_SRE_FAILED, Detail);
  }
  auto &Engine = Host->runtime().engine();
  const auto Names = Engine.getLastCounterNames();
  if (Names.empty() || Names.size() > EJIT_STAB_SRE_MAX_COUNTERS)
    return fail(C, EJIT_STAB_SRE_FAILED, "common T1 has no bounded complete profile inventory");
  O.counterNames.reserve(Names.size());
  O.counterRefs.reserve(Names.size());
  for (const std::string &N : Names) {
    StringRef Canonical = Engine.getCounterProfileName(N);
    auto Counts = Engine.lookup(R.codeGeneration, "__profc_" + N);
    auto Data = Engine.lookup(R.codeGeneration, "__profd_" + N);
    if (Canonical.empty() || !Counts || !Data) {
      if (!Counts) consumeError(Counts.takeError());
      if (!Data) consumeError(Data.takeError());
      Host->cancel("SRE exact counter inventory could not be resolved");
      return fail(C, EJIT_STAB_SRE_FAILED, "missing real common counter/data pair or canonical name");
    }
    const auto *Header = reinterpret_cast<const RawInstrProf::ProfileData<uintptr_t> *>(*Data);
    if (!*Counts || !*Data || Header->NameRef != IndexedInstrProf::ComputeHash(Canonical))
      return fail(C, EJIT_STAB_SRE_FAILED, "common counter/data pair has invalid canonical identity");
    O.counterNames.push_back(Canonical.str());
    O.counterRefs.push_back({nullptr, reinterpret_cast<uintptr_t>(*Counts),
                            reinterpret_cast<uintptr_t>(*Data)});
  }
  for (size_t I = 0; I < O.counterRefs.size(); ++I)
    O.counterRefs[I].pgoName = O.counterNames[I].c_str();
  C.function = Func;
  C.result = EJIT_STAB_SRE_OK;
  EJIT_DIAG("small-table SRE common T1 requested func=%u members=%u budget=%llu actual_worker_task=%llu",
            Func, R.numMembers, (unsigned long long)R.sampleLimit,
            (unsigned long long)LocalWorkerTask);
  return C.result;
}

int snapshot(Command &C) {
  OwnerControl &O = owner();
  auto *Host = O.runtime ? O.runtime->smallTableHost() : nullptr;
  if (!Host || !functionMatches(C.function))
    return fail(C, EJIT_STAB_SRE_INVALID, "snapshot has no matching common Host");
  auto &S = C.snapshot;
  S = O.fullProfileValid ? O.frozen : ejit_small_table_sre_snapshot_t{};
  S.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  S.structSize = sizeof(S);
  S.funcIndex = Host->funcIndex();
  S.ownerIdentity = load64(&Shared.ownerIdentity);
  S.workerTaskIdentity = LocalWorkerTask;
  S.policyEpoch = smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch());
  S.codeGeneration = O.codeGeneration;
  S.resourceGeneration = Host->runtime().resourceGeneration();
  S.sourceEpoch = O.facts->domainEpoch();
  S.configurationRevision = O.facts->configurationRevision();
  S.sampleLimit = Host->runtime().sampleBudget();
  S.sampleCount = Host->runtime().currentSessionSamples();
  S.inFlight = Host->runtime().sessionInFlight();
  S.physicalExecutions = Host->physicalExecutions();
  S.retainedExecutions = EJitSmallTableHost::retainedOwnerOutstandingExecutions();
  S.borrowReaders = load32(&O.facts->state()->readers);
  S.admittedMembers = Host->admittedCoordinates().size();
  S.publishedSlots = Host->publishedSlots();
  S.ownerWorkerOperations = Host->ownerWorkerOperations();
  S.tier = Host->activeTier() == "final" ? 2 : Host->activeTier() == "instrumented" ? 1 : 0;
  S.fullProfileValid = O.fullProfileValid;
  S.expectedCounterPairs = O.counterRefs.size();
  ejit_taskpool_stats_t Stats{};
  if (ejit_taskpool_get_stats(&Stats) != EJIT_OK)
    return fail(C, EJIT_STAB_SRE_FAILED, "ordinary taskpool statistics unavailable");
  S.genericAsyncEnqueues = Stats.asyncEnqueues;
  S.genericAsyncCompiles = Stats.asyncCompiles;
  S.genericPending = Stats.pendingEntries;
  if (!O.fullProfileValid) {
    if (S.physicalExecutions) {
      S.counterPairs = S.counterWordCount = 0;
      return fail(C, EJIT_STAB_SRE_BUSY, "counter snapshot refused while a real execution is in flight");
    }
    std::string Why;
    if (!captureCounters(S, Why)) return fail(C, EJIT_STAB_SRE_FAILED, Why);
  }
  S.status = C.result = EJIT_STAB_SRE_OK;
  S.reason[0] = 0;
  return C.result;
}

int finish(Command &C) {
  OwnerControl &O = owner();
  auto *Host = O.runtime ? O.runtime->smallTableHost() : nullptr;
  if (!Host || !functionMatches(C.function))
    return fail(C, EJIT_STAB_SRE_INVALID, "finish has no matching common Host");
  if (Host->physicalExecutions() || Host->runtime().sessionInFlight())
    return fail(C, EJIT_STAB_SRE_BUSY, "finish refused before the last REAL leave");
  if (O.fullProfileValid) return snapshot(C);
  if (O.finishing)
    return fail(C, EJIT_STAB_SRE_BUSY, "full profile consumption is already in progress");
  if (snapshot(C) != EJIT_STAB_SRE_OK) return C.result;
  O.finishing = true;
  struct FinishGuard {
    OwnerControl &O;
    ~FinishGuard() { O.finishing = false; }
  } Guard{O};
  std::string Why;
  auto Bundle = Host->runtime().freeze(Why);
  if (!Bundle) return fail(C, EJIT_STAB_SRE_FAILED, toString(Bundle.takeError()));
  if (!validateProfile(**Bundle, C.snapshot, Why)) {
    Host->cancel(Why);
    return fail(C, EJIT_STAB_SRE_FAILED, Why);
  }
  O.frozen = C.snapshot;
  O.frozen.profileBytes = (*Bundle)->profileData.size();
  if (Error E = Host->publishGeneration(Why))
    return fail(C, EJIT_STAB_SRE_FAILED, toString(std::move(E)));
  uint64_t ActualEntryCount = 0;
  if (!Host->runtime().engine().getFunctionProfileEntryCount(
          Host->entryName(), ActualEntryCount) ||
      ActualEntryCount != (*Bundle)->sampleCount) {
    Host->cancel("actual PGOUse entry metadata is missing or differs from complete T1 samples");
    return fail(C, EJIT_STAB_SRE_FAILED,
                "actual PGOUse entry metadata is missing or differs from complete T1 samples");
  }
  O.frozen.rootEntryCount = ActualEntryCount;
  O.frozen.rootEntryCountValid = 1;
  O.fullProfileValid = true;
  EJIT_DIAG("small-table SRE FULL profile consumed func=%u samples=%llu pairs=%u words=%u root_count=%llu T2=published",
            Host->funcIndex(), (unsigned long long)O.frozen.sampleCount,
            O.frozen.counterPairs, O.frozen.counterWordCount,
            (unsigned long long)O.frozen.rootEntryCount);
  return snapshot(C);
}

int prepareExecution(Command &C) {
  OwnerControl &O = owner();
  EJit *Runtime = smallTableSreLocalRuntime();
  auto *Host = Runtime ? Runtime->smallTableHost() : nullptr;
  if (!Runtime || Runtime != O.runtime || !Host || !Host->isBoundTo(C.function))
    return fail(C, EJIT_STAB_SRE_BLOCKED, "common execution has no live SRE owner");
  if (O.finishing)
    return fail(C, EJIT_STAB_SRE_BUSY, "common admission waits for verified full T2 consumption");
  if (smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch()) == UINT64_MAX)
    return fail(C, EJIT_STAB_SRE_BLOCKED, "shared common policy epoch exhausted");
  Lease *L = nullptr;
  for (Lease &Candidate : O.leases) if (!Candidate.ticket) { L = &Candidate; break; }
  if (!L) return fail(C, EJIT_STAB_SRE_BUSY, "bounded common execution lease capacity exhausted");
  const char *Why = nullptr;
  uint32_t Versions[4] = {}, Generation = 0;
  if (!smallTableSreValidateOwnerCall(C.function, C.dims, C.numDims,
                                     C.bounds, C.boundCount, Versions, Generation, Why))
    return fail(C, EJIT_STAB_SRE_BLOCKED, Why);
  SmallVector<uint32_t, 4> Types, Instances;
  for (uint32_t I = 0; I < C.numDims; ++I) {
    Types.push_back(C.dims[I].dimType); Instances.push_back(C.dims[I].instanceId);
  }
  uint64_t Pin = 0;
  std::string PinWhy;
  if (!Host->pinForExecutionPreparation(Types, Instances, Pin, PinWhy)) {
    // A product fact callback may reject AFTER the physical ticket was made.
    // Every failure closes that exact preparation, even when Pin != 0.
    ejit_stab_leave(Pin);
    return fail(C, EJIT_STAB_SRE_BLOCKED, PinWhy);
  }
  auto ReleasePin = [&]() { ejit_stab_leave(Pin); };
  void *Entry = Host->activeEntry();
  EJitCompiledCodeInfo Info;
  if (!Entry || !Host->runtime().engine().isCodeReady(Entry) ||
      !Host->runtime().engine().findCodeRange(Entry, Info)) {
    ReleasePin();
    return fail(C, EJIT_STAB_SRE_BLOCKED, "common finalized code range is unavailable");
  }
  auto *Resource = Host->runtime().resource();
  if (!Resource || Resource->columns().size() > 32) {
    ReleasePin(); return fail(C, EJIT_STAB_SRE_BLOCKED, "common table range inventory is unavailable or unbounded");
  }
  C.tableCount = 0;
  for (const auto &Column : Resource->columns()) {
    const uintptr_t Address = reinterpret_cast<uintptr_t>(Resource->columnAddress(Column.fieldIndex));
    if (!Address || !Column.payloadBytes) {
      ReleasePin(); return fail(C, EJIT_STAB_SRE_BLOCKED, "common table column has no actual published storage");
    }
    C.tables[C.tableCount++] = {Address, Column.payloadBytes};
  }
  const uint64_t Token = freshTicket();
  if (!Token || !retainSmallTableSreRuntime(Runtime)) {
    ReleasePin(); return fail(C, EJIT_STAB_SRE_BLOCKED, "common physical runtime pin was refused");
  }
  L->ticket = Token; L->hostTicket = Pin;
  L->epoch = smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch());
  L->ownerIdentity = currentEJitRuntimeOwnerIdentity();
  L->ownerCore = Runtime->sharedTaskPool()->state()->ownerCoreId.loadAcquire();
  if (!L->ownerIdentity) {
    *L = Lease();
    ReleasePin();
    releaseSmallTableSreRuntime(Runtime);
    return fail(C, EJIT_STAB_SRE_BLOCKED, "common owner retired during physical preparation");
  }
  L->generation = Generation; L->function = C.function;
  L->runtime = Runtime; L->host = Host; L->entry = Entry;
  L->numDims = C.numDims;
  for (uint32_t I = 0; I < C.numDims; ++I) {
    L->dims[I] = C.dims[I]; L->versions[I] = Versions[I];
  }
  store64(&Shared.leaseTokens[static_cast<size_t>(L - O.leases)], Token);
  C.ticket = Token; C.epoch = L->epoch;
  C.ownerIdentity = L->ownerIdentity; C.ownerGeneration = Generation;
  C.entry = reinterpret_cast<uintptr_t>(Entry); C.code = Info;
  C.sourceAddress = O.facts->request().sourceAddress;
  C.sourceBytes = O.facts->request().sourceBytes;
  C.sourceState = O.facts->request().sourceState;
  return C.result = EJIT_STAB_SRE_OK;
}

int commitExecution(Command &C) {
  Lease *L = lease(C.ticket);
  if (!L || L->executing)
    return fail(C, EJIT_STAB_SRE_BLOCKED, "stale or duplicate common preparation token");
  const auto Refuse = [&](const char *Why) {
    closeLease(*L); return fail(C, EJIT_STAB_SRE_BLOCKED, Why);
  };
  auto *Pool = L->runtime->sharedTaskPool();
  auto *State = Pool ? Pool->state() : nullptr;
  if (!State || L->epoch == UINT64_MAX || smallTableSreLocalRuntime() != L->runtime ||
      L->ownerIdentity != currentEJitRuntimeOwnerIdentity() ||
      L->epoch != smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch()) ||
      EJitSmallTableHost::global() != L->host || !L->host->wrapperAdmissionReady() ||
      State->generation.loadAcquire() != L->generation ||
      State->ownerCoreId.loadAcquire() != L->ownerCore ||
      State->initState.loadAcquire() != static_cast<uint32_t>(EJitSharedInitState::Ready) ||
      L->host->activeEntry() != L->entry)
    return Refuse("common owner/policy/code changed between preparation and admission");
  for (uint32_t I = 0; I < L->numDims; ++I)
    if (!Pool->isInstanceActive(L->dims[I].dimType, L->dims[I].instanceId) ||
        State->version[L->dims[I].dimType][L->dims[I].instanceId].loadAcquire() != L->versions[I])
      return Refuse("common lifecycle changed between preparation and admission");
  SmallVector<uint32_t, 4> Types, Instances;
  for (uint32_t I = 0; I < L->numDims; ++I) {
    Types.push_back(L->dims[I].dimType); Instances.push_back(L->dims[I].instanceId);
  }
  uint64_t Execution = 0;
  std::string Why;
  void *Entry = L->host->enter(Types, Instances, &Execution, &Why);
  if (!Entry || !Execution || Entry != L->entry ||
      L->epoch != smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch()) ||
      EJitSmallTableHost::global() != L->host ||
      smallTableSreLocalRuntime() != L->runtime ||
      currentEJitRuntimeOwnerIdentity() != L->ownerIdentity ||
      State->generation.loadAcquire() != L->generation ||
      State->ownerCoreId.loadAcquire() != L->ownerCore ||
      State->initState.loadAcquire() != static_cast<uint32_t>(EJitSharedInitState::Ready)) {
    ejit_stab_leave(Execution);
    return Refuse("common admission refused or owner changed in product fact callback");
  }
  for (uint32_t I = 0; I < L->numDims; ++I)
    if (!Pool->isInstanceActive(L->dims[I].dimType, L->dims[I].instanceId) ||
        State->version[L->dims[I].dimType][L->dims[I].instanceId].loadAcquire() != L->versions[I]) {
      ejit_stab_leave(Execution);
      return Refuse("common lifecycle changed in product admission callback");
    }
  ejit_stab_leave(L->hostTicket); // preparation pin, NOT a sample completion
  L->hostTicket = Execution; L->executing = true;
  C.entry = reinterpret_cast<uintptr_t>(Entry);
  C.ownerIdentity = L->ownerIdentity; C.ownerGeneration = L->generation;
  return C.result = EJIT_STAB_SRE_OK;
}

void process(Command &C) {
  C.result = EJIT_STAB_SRE_BLOCKED;
  if (C.operation == LeaveExecution) {
    // A true late leave remains serviceable after logical shutdown/cancel.
    // It can only close the exact old token, never a replacement lease.
    if (Lease *L = lease(C.ticket)) closeLease(*L);
    C.result = EJIT_STAB_SRE_OK;
    return;
  }
  EJit *Runtime = smallTableSreLocalRuntime();
  if (!Runtime || Runtime->sharedTaskPool() != LocalWorkerPool ||
      !LocalWorkerPool->isCurrentOwnerWorker() ||
      !LocalWorkerPool->state() ||
      LocalWorkerPool->state()->initState.loadAcquire() != static_cast<uint32_t>(EJitSharedInitState::Ready)) {
    fail(C, EJIT_STAB_SRE_BLOCKED, "SRE bridge has no live Ready owner worker"); return;
  }
  switch (C.operation) {
  case Request: setupRequest(C); break;
  case Snapshot: snapshot(C); break;
  case Finish: finish(C); break;
  case PrepareExecution: prepareExecution(C); break;
  case CommitExecution: commitExecution(C); break;
  case Cancel: {
    auto *Host = owner().runtime ? owner().runtime->smallTableHost() : nullptr;
    if (!Host || !functionMatches(C.function)) {
      fail(C, EJIT_STAB_SRE_INVALID, "cancel has no matching common Host"); break;
    }
    Host->cancel("SRE explicit cancel; real executions retain their leases");
    smallTableSrePolicyChanged();
    C.result = EJIT_STAB_SRE_OK;
    break;
  }
  case Print: {
    if (snapshot(C) != EJIT_STAB_SRE_OK && C.result != EJIT_STAB_SRE_BUSY) break;
    const auto &S = C.snapshot;
    EJIT_DIAG("SRE_STAB func=%u worker=%llu tier=%u samples=%llu/%llu physical=%llu borrows=%llu counters=%u/%u words=%u root=%llu full_profile=%u generic_enqueues=%llu generic_compiles=%llu pending=%llu",
              S.funcIndex, (unsigned long long)S.workerTaskIdentity, S.tier,
              (unsigned long long)S.sampleCount, (unsigned long long)S.sampleLimit,
              (unsigned long long)S.physicalExecutions, (unsigned long long)S.borrowReaders,
              S.counterPairs, S.expectedCounterPairs, S.counterWordCount,
              (unsigned long long)S.rootEntryCount, S.fullProfileValid,
              (unsigned long long)S.genericAsyncEnqueues,
              (unsigned long long)S.genericAsyncCompiles,
              (unsigned long long)S.genericPending);
    const std::string Name = owner().runtime->smallTableHost()->entryName().str();
    // Both real engines capture to the same worker-local store. Select the
    // actual common root, never an unrelated ordinary compile dump.
    ejit_print_dumped(Name.c_str()); ejit_print_dumped_module(Name.c_str());
    C.result = EJIT_STAB_SRE_OK;
    break;
  }
  default: fail(C, EJIT_STAB_SRE_INVALID, "unknown SRE owner command"); break;
  }
}

int transact(Command &C, bool RealLeave = false) {
  if (!load32(&LocalPrepared) || load32(&Shared.enabled) != 1)
    return EJIT_STAB_SRE_BLOCKED;
  const uint32_t Limit = LocalBindings.waitRounds ? LocalBindings.waitRounds : 8192u;
  uint32_t ReadyWait = 0;
  while (!RealLeave &&
         (!load64(&Shared.workerTask) || !load64(&Shared.ownerIdentity))) {
    if (++ReadyWait >= Limit) return EJIT_STAB_SRE_BLOCKED;
    delay();
  }
  if (LocalWorkerPool && isActualBridgeWorker(LocalWorkerPool)) {
    EJit *Pinned = acquireSmallTableSreRuntime(LocalWorkerPool);
    if (!Pinned) return EJIT_STAB_SRE_BLOCKED;
    process(C);
    releaseSmallTableSreRuntime(Pinned);
    return C.result;
  }
  Command *Slot = nullptr;
  uint32_t Wait = 0;
  while (!Slot) {
    for (auto &Candidate : Shared.commands) {
      uint32_t Expected = Free;
      if (cas32(&Candidate.state, Expected, Claimed)) { Slot = &Candidate; break; }
    }
    if (Slot) break;
    if (!RealLeave && ++Wait >= Limit) return EJIT_STAB_SRE_BUSY;
    delay();
  }
  std::memcpy(reinterpret_cast<uint8_t *>(Slot) + offsetof(Command, operation),
              reinterpret_cast<const uint8_t *>(&C) + offsetof(Command, operation),
              sizeof(C) - offsetof(Command, operation));
  store32(&Slot->state, Queued);
  for (;;) {
    const uint32_t State = load32(&Slot->state);
    if (State == Done) {
      std::memcpy(reinterpret_cast<uint8_t *>(&C) + offsetof(Command, operation),
                  reinterpret_cast<const uint8_t *>(Slot) + offsetof(Command, operation),
                  sizeof(C) - offsetof(Command, operation));
      store32(&Slot->state, Free);
      return C.result;
    }
    if (!RealLeave && ++Wait >= Limit) {
      uint32_t Expected = Queued;
      if (cas32(&Slot->state, Expected, Cancelled)) {
        // Owner can observe Cancelled but never owns this payload. Releasing
        // immediately is safe: CAS Queued->Started can no longer succeed.
        store32(&Slot->state, Free);
        return EJIT_STAB_SRE_BUSY;
      }
      // Started cannot be abandoned: join real completion and retain captures.
    }
    delay();
  }
}

bool liveBridgeTicket(uint64_t Ticket) {
  for (const auto &Token : Shared.leaseTokens)
    if (load64(&Token) == Ticket) return true;
  return false;
}
void completeRealLeave(Command &C) {
  // An issued physical token is never abandoned on a diagnostic timeout or
  // logical shutdown. Its original worker remains pinned until this exact
  // completion. A terminated OS worker is an external fail-stop condition,
  // not permission to pretend that an executing call returned.
  while (liveBridgeTicket(C.ticket)) {
    if (transact(C, /*RealLeave=*/true) == EJIT_STAB_SRE_OK) return;
    if (load32(&LocalPrepared)) delay();
    else EJitSreTask::yield();
  }
}
} // namespace
#endif // EJIT_SRE_SHARED_TASKPOOL

namespace llvm { namespace ejit {
void smallTableSrePolicyChanged() {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (load32(&Shared.enabled) != 1) return;
  uint64_t N = load64(&Shared.policyEpoch);
  while (N < BridgeEpochBit - 1 &&
         !EJitAtomicRef<uint64_t>(Shared.policyEpoch).compareExchange(N, N + 1)) {}
#endif
}
uint64_t smallTableSreWrapperEpoch(uint64_t LocalEpoch) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (load32(&Shared.enabled) == 1)
    return BridgeEpochBit | load64(&Shared.policyEpoch);
#endif
  return LocalEpoch;
}
bool smallTableSreNoPolicyCurrent(uint64_t Epoch, uint64_t LocalEpoch) {
  return Epoch && Epoch != UINT64_MAX && Epoch == smallTableSreWrapperEpoch(LocalEpoch);
}
bool smallTableSreOwnsFunction(uint32_t Func) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  return load32(&Shared.enabled) == 1 && Func != UINT32_MAX &&
         load32(&Shared.function) == Func;
#else
  (void)Func; return false;
#endif
}
void smallTableSreWorkerEnter(EJitSharedTaskPool &Pool) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!load32(&LocalPrepared)) return;
  LocalWorkerPool = &Pool;
  LocalWorkerTask = LocalBindings.current_task_id(LocalBindings.context);
  if (!LocalWorkerTask) {
    EJIT_DIAG("small-table SRE bridge BLOCKED: actual worker task identity unavailable");
    return;
  }
  // This hook is called ONLY from the actual worker loop, not pollOnce/core-ID
  // simulation. Subsequent controls compare the real SDK task token exactly.
  Pool.setWorkerIdentityCallback(isActualBridgeWorker, &Pool);
#else
  (void)Pool;
#endif
}
bool serviceSmallTableSreBridge(EJitSharedTaskPool &Pool) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (load32(&Shared.enabled) != 1 || !isActualBridgeWorker(&Pool)) return false;
  bool QueuedWork = false;
  for (const auto &C : Shared.commands)
    if (load32(&C.state) == Queued) { QueuedWork = true; break; }
  if (!QueuedWork && load64(&Shared.ownerIdentity) != 0 && Pool.state() &&
      load32(&Shared.ownerGeneration) == Pool.state()->generation.loadAcquire())
    return false; // no idle-loop runtime pin, no accidental deferred shutdown
  EJit *Runtime = acquireSmallTableSreRuntime(&Pool);
  if (!Runtime) return false;
  struct RuntimePin {
    EJit *Runtime;
    ~RuntimePin() { releaseSmallTableSreRuntime(Runtime); }
  } Pin{Runtime};
  if (!Pool.state()) return false;
  auto *State = Pool.state();
  if (State->initState.loadAcquire() != static_cast<uint32_t>(EJitSharedInitState::Ready)) return false;
  const uint64_t LiveOwner = currentEJitRuntimeOwnerIdentity();
  if (LiveOwner && smallTableSreLocalRuntime() == Runtime) {
    store32(&Shared.ownerCore, State->ownerCoreId.loadAcquire());
    store32(&Shared.ownerGeneration, State->generation.loadAcquire());
    store64(&Shared.workerTask, LocalWorkerTask);
    // Logical shutdown may clear the live publication after the test above.
    // Never erase the prior routing identity of still-pinned real executions.
    store64(&Shared.ownerIdentity, LiveOwner);
  }
  for (auto &C : Shared.commands) {
    uint32_t Expected = Queued;
    if (!cas32(&C.state, Expected, Started)) continue;
    // Owner-local temporary lifetime protection; no private pointer rides the
    // command. A physical preparation/execution adds its own persistent pin.
    process(C);
    store32(&C.state, Done);
    return true;
  }
#else
  (void)Pool;
#endif
  return false;
}
void smallTableSreWorkerExit(EJitSharedTaskPool &Pool) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (LocalWorkerPool != &Pool) return;
  for (Lease &L : owner().leases)
    if (L.ticket) {
      EJIT_DIAG("SRE worker exited with live physical ticket=%llu; NOT reclaimed",
                (unsigned long long)L.ticket);
      return;
    }
  store64(&Shared.ownerIdentity, 0);
  store64(&Shared.workerTask, 0);
  store32(&Shared.function, UINT32_MAX);
  smallTableSrePolicyChanged();
  OwnerControl &O = owner();
  O.runtime = nullptr;
  O.facts.reset();
  O.counterNames.clear();
  O.counterRefs.clear();
  O.frozen = {};
  O.fullProfileValid = false;
  O.finishing = false;
  O.retirementObserved = false;
  O.codeGeneration = 0;
  LocalWorkerPool = nullptr; LocalWorkerTask = 0;
#else
  (void)Pool;
#endif
}

bool smallTableSreWrapperEnter(uint32_t Func, const ejit_dim_pair_t *Dims,
                             uint32_t NumDims, const ejit_bound_ptr_t *Bounds,
                             uint32_t BoundCount, uint64_t *OutTicket,
                             const char **OutWhy, uint64_t *OutEpoch,
                             void *&Entry) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (load32(&Shared.enabled) != 1 || Func != load32(&Shared.function)) return false;
  Entry = nullptr;
  if (OutTicket) *OutTicket = 0;
  if (OutWhy) *OutWhy = "SRE common policy refused admission or caller preparation";
  if (OutEpoch) *OutEpoch = smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch());
  if (!OutTicket || !OutWhy || !OutEpoch || NumDims > 4 || (NumDims && !Dims) ||
      BoundCount > 8 || (BoundCount && !Bounds) || !load32(&LocalPrepared)) return true;
  LocalCommand Scratch;
  if (!Scratch.value) return true;
  Command &C = *Scratch.value;
  C.operation = PrepareExecution; C.function = Func; C.numDims = NumDims; C.boundCount = BoundCount;
  for (uint32_t I = 0; I < NumDims; ++I) C.dims[I] = Dims[I];
  for (uint32_t I = 0; I < BoundCount; ++I) C.bounds[I] = Bounds[I];
  if (transact(C) != EJIT_STAB_SRE_OK) return true;
  const uint64_t Token = C.ticket;
  EJit *Caller = acquireSmallTableSreRuntime(nullptr);
  struct CallerRuntimePin {
    EJit *Runtime;
    ~CallerRuntimePin() { if (Runtime) releaseSmallTableSreRuntime(Runtime); }
  } CallerPin{Caller};
  auto *Pool = Caller ? Caller->sharedTaskPool() : nullptr;
  auto *State = Pool ? Pool->state() : nullptr;
  bool Prepared = State && State->initState.loadAcquire() == static_cast<uint32_t>(EJitSharedInitState::Ready) &&
      State->generation.loadAcquire() == C.ownerGeneration &&
      State->ownerCoreId.loadAcquire() == load32(&Shared.ownerCore) &&
      C.ownerIdentity == load64(&Shared.ownerIdentity) &&
      C.epoch == smallTableSreWrapperEpoch(EJitSmallTableHost::policyEpoch());
  // Ordinary/live loads still read the genuine source during common execution.
  // Prepare its validated extent AND borrow-state POD on the actual caller,
  // not only on the worker that acquired the protected configuration borrow.
  if (Prepared)
    Prepared = dataReady(C.sourceAddress, C.sourceBytes, EJIT_STAB_SRE_DATA_READ) &&
               dataReady(C.sourceState, sizeof(ejit_small_table_sre_source_state_t),
                         EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE);
  for (uint32_t I = 0; I < C.tableCount && Prepared; ++I)
    Prepared = dataReady(C.tables[I].address, C.tables[I].bytes, EJIT_STAB_SRE_DATA_READ);
  if (Prepared) Prepared = Pool->prepareExternalExecution(reinterpret_cast<void *>(C.entry), C.code);
  // Common writable counters need both real range permissions and coherence;
  // the callback is for THIS caller, not borrowed ordinary-object preparation.
  for (uint32_t I = 0; I < C.code.writableCount && Prepared; ++I)
    Prepared = dataReady(C.code.writableRanges[I].addr,
                         C.code.writableRanges[I].size,
                         EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE);
  C.operation = Prepared ? CommitExecution : LeaveExecution;
  C.ticket = Token;
  if (!Prepared) {
    completeRealLeave(C);
    return true;
  }
  const int Result = transact(C);
  if (!Prepared || Result != EJIT_STAB_SRE_OK) {
    C.operation = LeaveExecution; C.ticket = Token; completeRealLeave(C);
    return true;
  }
  Entry = reinterpret_cast<void *>(C.entry);
  *OutTicket = Token; *OutWhy = nullptr;
  return true;
#else
  (void)Func; (void)Dims; (void)NumDims; (void)Bounds; (void)BoundCount;
  (void)OutTicket; (void)OutWhy; (void)OutEpoch; (void)Entry;
  return false;
#endif
}
bool smallTableSreLeave(uint64_t Ticket) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!(Ticket & BridgeEpochBit) || load32(&Shared.enabled) != 1) return false;
  if (!liveBridgeTicket(Ticket)) return true; // exact old token: no replacement touched
  LocalCommand Scratch;
  // A real leave is not lossy. Wait for bounded scratch occupancy to drain;
  // once started it must join real completion, even after logical shutdown.
  while (!Scratch.value) {
    if (load32(&LocalPrepared)) delay(); else EJitSreTask::yield();
    Scratch.~LocalCommand(); new (&Scratch) LocalCommand();
  }
  Scratch.value->operation = LeaveExecution; Scratch.value->ticket = Ticket;
  completeRealLeave(*Scratch.value);
  return true;
#else
  (void)Ticket; return false;
#endif
}
} } // namespace llvm::ejit

extern "C" {
int ejit_small_table_sre_prepare(const ejit_small_table_sre_bindings_t *Bindings) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!Bindings || Bindings->abiVersion != EJIT_STAB_SRE_ABI_VERSION ||
      Bindings->structSize != sizeof(*Bindings) || !Bindings->current_task_id ||
      !Bindings->delay_ticks || !Bindings->prepare_shared_data ||
      Bindings->waitRounds > 65536 ||
      (Bindings->flags & ~EJIT_STAB_SRE_ENABLE_FIXED_DOMAIN) ||
      !Bindings->current_task_id(Bindings->context)) return EJIT_STAB_SRE_BLOCKED;
  if (load32(&LocalPrepared)) {
    return LocalBindings.current_task_id == Bindings->current_task_id &&
                   LocalBindings.delay_ticks == Bindings->delay_ticks &&
                   LocalBindings.prepare_shared_data == Bindings->prepare_shared_data &&
                   LocalBindings.context == Bindings->context &&
                   LocalBindings.flags == Bindings->flags &&
                   LocalBindings.waitRounds == Bindings->waitRounds
               ? EJIT_STAB_SRE_OK : EJIT_STAB_SRE_BUSY;
  }
  // Bindings must precede worker startup; installing them later could capture
  // a shell task as an already-existing worker and is explicitly forbidden.
  if (smallTableSreLocalRuntime()) return EJIT_STAB_SRE_BLOCKED;
  LocalBindings = *Bindings;
  store32(&LocalPrepared, 1);
  if (!dataReady(reinterpret_cast<uintptr_t>(&Shared), sizeof(Shared),
                 EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE)) {
    store32(&LocalPrepared, 0); return EJIT_STAB_SRE_BLOCKED;
  }
  if (Bindings->flags & EJIT_STAB_SRE_ENABLE_FIXED_DOMAIN) {
#if defined(EJIT_SRE_CODE_POOL) && defined(EJIT_FIXED_CODE_POOL)
    if (Error E = enableSreSharedFixedCodePoolDomain()) {
      const std::string Why = toString(std::move(E));
      EJIT_DIAG("SRE small-table fixed-domain preparation refused: %s", Why.c_str());
      store32(&LocalPrepared, 0); return EJIT_STAB_SRE_BLOCKED;
    }
#else
    store32(&LocalPrepared, 0); return EJIT_STAB_SRE_BLOCKED;
#endif
  }
  uint32_t Expected = 0;
  if (cas32(&Shared.enabled, Expected, 2)) {
    store32(&Shared.function, UINT32_MAX);
    store64(&Shared.policyEpoch, 1);
    store32(&Shared.enabled, 1);
  } else {
    uint32_t Wait = 0;
    while (load32(&Shared.enabled) == 2) {
      if (++Wait >= (Bindings->waitRounds ? Bindings->waitRounds : 8192u)) return EJIT_STAB_SRE_BUSY;
      delay();
    }
  }
  return EJIT_STAB_SRE_OK;
#else
  (void)Bindings; return EJIT_STAB_SRE_BLOCKED;
#endif
}
int ejit_small_table_sre_request(const ejit_small_table_sre_request_t *R) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!R || R->abiVersion != EJIT_STAB_SRE_ABI_VERSION || R->structSize != sizeof(*R)) return EJIT_STAB_SRE_INVALID;
  LocalCommand Scratch; if (!Scratch.value) return EJIT_STAB_SRE_BUSY;
  Scratch.value->operation = Request; Scratch.value->request = *R;
  return transact(*Scratch.value);
#else
  (void)R; return EJIT_STAB_SRE_BLOCKED;
#endif
}
int ejit_small_table_sre_get_snapshot(uint32_t Func, ejit_small_table_sre_snapshot_t *S) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!S) return EJIT_STAB_SRE_INVALID;
  LocalCommand Scratch; if (!Scratch.value) return EJIT_STAB_SRE_BUSY;
  Scratch.value->operation = Snapshot; Scratch.value->function = Func;
  const int Result = transact(*Scratch.value);
  *S = Scratch.value->snapshot;
  return Result;
#else
  (void)Func; (void)S; return EJIT_STAB_SRE_BLOCKED;
#endif
}
int ejit_small_table_sre_finish(uint32_t Func) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  LocalCommand Scratch; if (!Scratch.value) return EJIT_STAB_SRE_BUSY;
  Scratch.value->operation = Finish; Scratch.value->function = Func;
  return transact(*Scratch.value);
#else
  (void)Func; return EJIT_STAB_SRE_BLOCKED;
#endif
}
int ejit_small_table_sre_cancel(uint32_t Func) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  LocalCommand Scratch; if (!Scratch.value) return EJIT_STAB_SRE_BUSY;
  Scratch.value->operation = Cancel; Scratch.value->function = Func;
  return transact(*Scratch.value);
#else
  (void)Func; return EJIT_STAB_SRE_BLOCKED;
#endif
}
int ejit_small_table_sre_print(uint32_t Func) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  LocalCommand Scratch; if (!Scratch.value) return EJIT_STAB_SRE_BUSY;
  Scratch.value->operation = Print; Scratch.value->function = Func;
  return transact(*Scratch.value);
#else
  (void)Func; return EJIT_STAB_SRE_BLOCKED;
#endif
}
} // extern "C"
