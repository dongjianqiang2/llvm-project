//===-- EJitPreparedCode.cpp - Exact final IR and pre-claim emission
//--------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitPreparedCode.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <limits>

using namespace llvm;
using namespace llvm::ejit;

namespace {
Error reject(StringRef Reason, std::errc Code = std::errc::invalid_argument) {
  return make_error<StringError>("prepared-code: " + Reason,
                                 std::make_error_code(Code));
}

Error independent(StringRef Reason) {
  return reject(Reason, std::errc::operation_not_supported);
}

Error exhausted(StringRef Reason) {
  return reject(Reason, std::errc::no_buffer_space);
}

class BoundedIRStream final : public raw_ostream {
  std::string &Out;
  uint64_t Limit;
  bool Full = false;
  void write_impl(const char *Ptr, size_t Size) override {
    if (Full || Size > Limit - Out.size()) {
      Full = true;
      return;
    }
    Out.append(Ptr, Size);
  }
  uint64_t current_pos() const override { return Out.size(); }

public:
  BoundedIRStream(std::string &Out, uint64_t Limit) : Out(Out), Limit(Limit) {
    SetUnbuffered();
  }
  bool exceeded() const { return Full; }
};

uint64_t defaultBucketHash(ArrayRef<uint8_t> Digest) {
  uint64_t Value = 0;
  for (unsigned I = 0; I != 8; ++I)
    Value = (Value << 8) | Digest[I];
  return Value;
}

Error canonicalizeIdentityIR(const Module &M, std::string &Out,
                             uint64_t MaxBytes) {
  std::unique_ptr<Module> Comparison = CloneModule(M);
  StripDebugInfo(*Comparison);
  Comparison->setModuleIdentifier("");
  Comparison->setSourceFileName("");
  for (Function &F : *Comparison) {
    for (Argument &A : F.args())
      A.setName("");
    for (BasicBlock &BB : F) {
      BB.setName("");
      for (Instruction &I : BB)
        if (!I.getType()->isVoidTy())
          I.setName("");
    }
  }
  BoundedIRStream OS(Out, MaxBytes);
  Comparison->print(OS, nullptr);
  if (OS.exceeded())
    return exhausted("canonical IR budget exceeded");
  return Error::success();
}

Error checkCandidateModule(const Module &M,
                           const EJitCodeIdentityScope &Scope,
                           ArrayRef<EJitCodeBinding> Bindings) {
  const Function *Entry = M.getFunction(Scope.entry);
  if (!Entry || Entry->isDeclarationForLinker())
    return reject("candidate entry definition missing");
  auto HasBinding = [&](const GlobalValue &GV, bool Callable) {
    return llvm::any_of(Bindings, [&](const EJitCodeBinding &B) {
      return B.name == GV.getName() && B.callable == Callable;
    });
  };
  for (const EJitCodeBinding &B : Bindings)
    if (const GlobalValue *GV = M.getNamedValue(B.name)) {
      const auto *F = dyn_cast<Function>(GV);
      if (static_cast<bool>(F) != B.callable)
        return reject("candidate binding kind mismatch");
      if (!GV->isDeclaration())
        return reject("candidate binding shadows a definition");
    }
  for (const GlobalVariable &GV : M.globals())
    if (GV.isDeclaration() && !GV.use_empty() && !HasBinding(GV, false))
      return reject("candidate missing external data binding");
  for (const Function &F : M.functions())
    if (F.isDeclaration() && !F.isIntrinsic() && !F.use_empty() &&
        !HasBinding(F, true))
      return reject("candidate missing external function binding");
  return Error::success();
}

Error checkModule(const Module &M, StringRef Entry,
                  ArrayRef<EJitCodeBinding> Bindings,
                  const EJitPreparedCodeLimits &Limits) {
  uint32_t NodeLimit = Limits.maxIRNodes;
  auto HasBinding = [&](const GlobalValue &GV, bool Callable) {
    auto It = llvm::lower_bound(Bindings, GV.getName(),
                                [](const EJitCodeBinding &B, StringRef Name) {
                                  return StringRef(B.name) < Name;
                                });
    return It != Bindings.end() && It->name == GV.getName() &&
           It->callable == Callable;
  };
  if (!M.getModuleInlineAsm().empty() || !M.alias_empty() || !M.ifunc_empty())
    return independent(
        "module asm, aliases or ifuncs require independent code");
  const Function *Root = M.getFunction(Entry);
  if (!Root || Root->isDeclarationForLinker())
    return reject("entry definition missing");
  for (const auto &B : Bindings)
    if (const GlobalValue *GV = M.getNamedValue(B.name))
      if (!GV->isDeclaration())
        return reject("external binding shadows a module definition");
  uint64_t Nodes = 0;
  uint64_t DataBytesLeft = Limits.maxDefinedDataBytes;
  for (const GlobalVariable &GV : M.globals()) {
    if (++Nodes > NodeLimit)
      return exhausted("IR node budget exceeded");
    if (GV.isThreadLocal())
      return independent("TLS cannot be shared by absolute binding");
    if (GV.getAddressSpace() != 0)
      return independent("non-default global address space");
    if (GV.isDeclaration()) {
      if (!GV.use_empty() && !HasBinding(GV, false))
        return reject("unresolved external data binding");
    } else if (!GV.isConstant() || !GV.hasLocalLinkage() ||
               !GV.hasGlobalUnnamedAddr()) {
      return independent("private state or address-observable defined global");
    } else {
      TypeSize Bytes = M.getDataLayout().getTypeAllocSize(GV.getValueType());
      if (Bytes.isScalable() || Bytes.getFixedValue() > DataBytesLeft)
        return exhausted("defined data budget exceeded");
      DataBytesLeft -= Bytes.getFixedValue();
    }
  }
  for (const Function &F : M) {
    if (++Nodes > NodeLimit)
      return exhausted("IR node budget exceeded");
    if (F.getAddressSpace() != 0)
      return independent("non-default function address space");
    if (F.isDeclaration()) {
      if (!F.isIntrinsic() && !F.use_empty() && !HasBinding(F, true))
        return reject("unresolved external function binding");
      continue;
    }
    if ((&F != Root && !F.hasLocalLinkage()) || F.hasAddressTaken())
      return independent("address-observable function definition");
    for (const BasicBlock &BB : F) {
      if (++Nodes > NodeLimit || BB.size() > NodeLimit - Nodes)
        return exhausted("IR node budget exceeded");
      Nodes += BB.size();
      if (BB.hasAddressTaken())
        return independent("blockaddress requires independent code");
      for (const Instruction &I : BB)
        if (const auto *CB = dyn_cast<CallBase>(&I))
          if (CB->isInlineAsm())
            return independent("inline asm requires independent code");
    }
  }
  return Error::success();
}
} // namespace

bool EJitCodeBinding::operator==(const EJitCodeBinding &Other) const {
  return name == Other.name && address == Other.address &&
         callable == Other.callable;
}

bool EJitCodeIdentityScope::operator==(
    const EJitCodeIdentityScope &Other) const {
  return source == Other.source && entry == Other.entry &&
         compilerPolicy == Other.compilerPolicy &&
         bindingGeneration == Other.bindingGeneration;
}

uint64_t EJitFinalCodeIdentity::storageBytes() const {
  uint64_t Bytes = sizeof(*this) + ir_.size() + scope_.entry.size() +
                   scope_.compilerPolicy.size();
  for (const auto &B : bindings_)
    Bytes += sizeof(B) + B.name.size();
  return Bytes;
}

bool EJitFinalCodeIdentity::equals(const EJitFinalCodeIdentity &Other) const {
  return scope_ == Other.scope_ && bindings_ == Other.bindings_ &&
         ir_ == Other.ir_;
}

Expected<std::unique_ptr<EJitPreparedCode>> EJitPreparedCode::create(
    orc::ThreadSafeModule FinalModule, EJitCodeIdentityScope Scope,
    ArrayRef<EJitCodeBinding> Bindings, const EJitPreparedCodeLimits &Limits) {
  if (!FinalModule || Scope.entry.empty() || Scope.compilerPolicy.empty() ||
      Scope.bindingGeneration == 0 || Limits.maxModuleBytes == 0)
    return reject("missing module, entry, policy, generation or budget");
  bool HasSource = false;
  for (uint8_t Byte : Scope.source)
    HasSource |= Byte != 0;
  if (!HasSource)
    return reject("missing source bitcode digest");
  auto Result = std::unique_ptr<EJitPreparedCode>(
      new EJitPreparedCode(std::move(FinalModule)));
  auto &ID = Result->identity_;
  ID.scope_ = std::move(Scope);
  // Bound input metadata before copying it. Even duplicate or unused bindings
  // belong to the resolution environment and must not evade the total budget.
  uint64_t Remaining = Limits.maxIdentityBytes;
  auto Charge = [&](uint64_t Bytes) {
    if (Bytes > Remaining)
      return false;
    Remaining -= Bytes;
    return true;
  };
  if (!Charge(sizeof(ID)) || !Charge(ID.scope_.entry.size()) ||
      !Charge(ID.scope_.compilerPolicy.size()))
    return exhausted("identity metadata budget exceeded");
  for (const auto &B : Bindings)
    if (!Charge(sizeof(B)) || !Charge(B.name.size()))
      return exhausted("binding metadata budget exceeded");
  ID.bindings_.assign(Bindings.begin(), Bindings.end());
  llvm::sort(ID.bindings_,
             [](const auto &A, const auto &B) { return A.name < B.name; });
  for (size_t I = 0; I < ID.bindings_.size(); ++I) {
    const auto &B = ID.bindings_[I];
    if (B.name.empty() || B.address == 0 ||
        (I && ID.bindings_[I - 1].name == B.name))
      return reject("empty, null or duplicate external binding");
  }
  Error Err = Result->module_.withModuleDo([&](Module &M) -> Error {
    if (M.getDataLayoutStr().empty() || M.getTargetTriple().str().empty())
      return reject("target triple or DataLayout missing");
    if (verifyModule(M))
      return reject("invalid final IR");
    if (auto E = checkModule(M, ID.scope_.entry, ID.bindings_, Limits))
      return E;
    // The root is the lookup symbol; finalize this attribute BEFORE identity.
    Function *Root = M.getFunction(ID.scope_.entry);
    if (Root->hasLocalLinkage())
      Root->setLinkage(GlobalValue::ExternalLinkage);
    if (Error E = canonicalizeIdentityIR(
            M, ID.ir_, std::min(Limits.maxModuleBytes, Remaining)))
      return E;
    ID.digest_ = SHA256::hash(arrayRefFromStringRef(ID.ir_));
    return Error::success();
  });
  if (Err)
    return std::move(Err);
  return std::move(Result);
}

struct EJitPreparedCodeEmitter::Impl {
  struct Record {
    EJitFinalCodeIdentity identity;
    uint64_t bucket;
    uint64_t codeId;
    void *fn;
  };
  orc::LLJIT &jit;
  uint64_t epoch;
  EJitPreparedCodeLimits limits;
  BucketHash hash;
  uint64_t nextId = 1;
  bool linking = false;
  Stats stats;
  std::vector<Record> records;
  Impl(orc::LLJIT &J, uint64_t Epoch, EJitPreparedCodeLimits Limits,
       BucketHash Hash)
      : jit(J), epoch(Epoch), limits(Limits),
        hash(Hash ? Hash : defaultBucketHash) {}
};

EJitPreparedCodeEmitter::EJitPreparedCodeEmitter(orc::LLJIT &J,
                                                 uint64_t OwnerEpoch,
                                                 EJitPreparedCodeLimits Limits,
                                                 BucketHash Hash)
    : P(std::make_unique<Impl>(J, OwnerEpoch, Limits, Hash)) {}
EJitPreparedCodeEmitter::~EJitPreparedCodeEmitter() = default;

Expected<EJitPreparedCodeEmitter::LinkedCode>
EJitPreparedCodeEmitter::link(std::unique_ptr<EJitPreparedCode> Prepared) {
  if (!Prepared || !P->epoch || P->linking)
    return reject("missing prepared module/owner epoch or reentrant emission");
  P->linking = true;
  auto Reset = make_scope_exit([&] { P->linking = false; });
  auto &ID = Prepared->identity_;
  bool TargetMatches = Prepared->module_.withModuleDo([&](const Module &M) {
    return M.getDataLayout() == P->jit.getDataLayout() &&
           M.getTargetTriple() == P->jit.getTargetTriple();
  });
  if (!TargetMatches) {
    ++P->stats.failed;
    return reject("prepared module does not match emitter target");
  }
  uint64_t Bucket = P->hash(ID.digest());
  for (const auto &R : P->records)
    if (R.bucket == Bucket && R.identity.equals(ID)) {
      ++P->stats.reused;
      return LinkedCode{R.codeId, R.fn, true};
    }
  uint64_t Bytes = ID.storageBytes();
  if (P->records.size() >= P->limits.maxCodeObjects ||
      Bytes > P->limits.maxIdentityBytes - P->stats.identityBytes ||
      P->nextId == std::numeric_limits<uint64_t>::max()) {
    ++P->stats.capacityRejected;
    return exhausted("physical code/identity budget exhausted");
  }
  uint64_t CodeID = P->nextId++;
  // A physical JD has no logical cacheKey owner. Removing the first logical
  // member must never remove code still used by another member.
  std::string JDName = "spec_t2_shared_" + std::to_string(P->epoch) + "_" +
                       std::to_string(CodeID);
  auto &ES = P->jit.getExecutionSession();
  if (ES.getJITDylibByName(JDName)) {
    ++P->stats.failed;
    return reject("physical owner epoch/JITDylib name already in use");
  }
  // LLJIT::createJITDylib adds process/platform default links. They would
  // resolve undeclared backend libcalls outside the compared binding list.
  // A bare JD contains only this module and its exact absolute bindings.
  auto &JD = ES.createBareJITDylib(std::move(JDName));
  auto Fail = [&](Error E) -> Expected<LinkedCode> {
    ++P->stats.failed;
    return joinErrors(std::move(E), ES.removeJITDylib(JD));
  };
  orc::SymbolMap Symbols;
  for (const auto &B : ID.bindings_) {
    JITSymbolFlags Flags = JITSymbolFlags::Exported;
    if (B.callable)
      Flags |= JITSymbolFlags::Callable;
    Symbols[P->jit.mangleAndIntern(B.name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr(B.address), Flags);
  }
  if (!Symbols.empty())
    if (auto E = JD.define(orc::absoluteSymbols(std::move(Symbols))))
      return Fail(std::move(E));
  // IR identity was taken after ALL transforms. Adding to IRCompileLayer
  // creates claims from that same module, without another IRTransform pass.
  if (auto E = P->jit.getIRCompileLayer().add(JD, std::move(Prepared->module_)))
    return Fail(std::move(E));
  auto Address = P->jit.lookup(JD, ID.scope_.entry);
  if (!Address)
    return Fail(Address.takeError());
  if (!Address->getValue())
    return Fail(reject("materialization returned a null entry"));
  void *Fn = Address->toPtr<void *>();
  P->records.push_back({std::move(ID), Bucket, CodeID, Fn});
  ++P->stats.codeObjects;
  P->stats.identityBytes += Bytes;
  return LinkedCode{CodeID, Fn, false};
}

EJitPreparedCodeEmitter::Stats EJitPreparedCodeEmitter::stats() const {
  return P->stats;
}

bool EJitPreparedCodeEmitter::linkedIdentityEquals(
    uint64_t CodeId, const EJitFinalCodeIdentity &Other) const {
  for (const Impl::Record &R : P->records)
    if (R.codeId == CodeId)
      return R.identity.equals(Other);
  return false;
}

namespace {
struct CandidateIdentity {
  EJitCodeIdentityScope scope;
  std::vector<EJitCodeBinding> bindings;
  std::vector<PgoFunctionSchema> schema;
  std::string ir;
  std::array<uint8_t, 32> digest{};
  uint64_t bytes() const {
    uint64_t N = sizeof(*this) + ir.size() + scope.entry.size() +
                 scope.compilerPolicy.size();
    for (const auto &B : bindings)
      N += sizeof(B) + B.name.size();
    for (const auto &S : schema)
      N += sizeof(S) + S.pgoName.size();
    return N;
  }
};
bool sameSchema(const PgoFunctionSchema &A, const PgoFunctionSchema &B) {
  return A.pgoName == B.pgoName && A.funcHash == B.funcHash &&
         A.pgoNameHash == B.pgoNameHash && A.numCounters == B.numCounters &&
         A.numIcSites == B.numIcSites && A.numMemSites == B.numMemSites &&
         A.numScalarSites == B.numScalarSites;
}
bool sameCandidate(const CandidateIdentity &A, const CandidateIdentity &B) {
  return A.scope == B.scope && A.bindings == B.bindings &&
         A.schema.size() == B.schema.size() &&
         llvm::equal(A.schema, B.schema, sameSchema) && A.ir == B.ir;
}
bool candidateInputFits(uint64_t Limit, const EJitCodeIdentityScope &Scope,
                        ArrayRef<EJitCodeBinding> Bindings,
                        ArrayRef<PgoFunctionSchema> Schema,
                        StringRef CanonicalIR = {}) {
  uint64_t Bytes = sizeof(CandidateIdentity);
  auto Add = [&](uint64_t N) {
    if (Bytes > Limit || N > Limit - Bytes)
      return false;
    Bytes += N;
    return true;
  };
  if (!Add(Scope.entry.size()) || !Add(Scope.compilerPolicy.size()) ||
      !Add(CanonicalIR.size()))
    return false;
  for (const auto &B : Bindings)
    if (!Add(sizeof(B)) || !Add(B.name.size()))
      return false;
  for (const auto &S : Schema)
    if (!Add(sizeof(S)) || !Add(S.pgoName.size()))
      return false;
  return true;
}
Expected<CandidateIdentity>
makeCandidate(std::string CanonicalIR, EJitCodeIdentityScope Scope,
              ArrayRef<EJitCodeBinding> Bindings,
              ArrayRef<PgoFunctionSchema> Schema) {
  bool HasSource = false;
  for (uint8_t B : Scope.source)
    HasSource |= B != 0;
  if (!HasSource || Scope.entry.empty() || Scope.compilerPolicy.empty() ||
      Scope.bindingGeneration == 0 || CanonicalIR.empty())
    return reject("candidate missing scope or prefix IR");
  CandidateIdentity ID;
  ID.scope = std::move(Scope);
  ID.bindings.assign(Bindings.begin(), Bindings.end());
  llvm::sort(ID.bindings,
             [](const auto &A, const auto &B) { return A.name < B.name; });
  for (size_t I = 0; I < ID.bindings.size(); ++I)
    if (ID.bindings[I].name.empty() || !ID.bindings[I].address ||
        (I && ID.bindings[I - 1].name == ID.bindings[I].name))
      return reject("candidate invalid binding");
  if (Schema.empty())
    return reject("candidate missing PGO schema");
  ID.schema.assign(Schema.begin(), Schema.end());
  llvm::sort(ID.schema, [](const auto &A, const auto &B) {
    return A.pgoName < B.pgoName;
  });
  for (size_t I = 0; I < ID.schema.size(); ++I)
    if (ID.schema[I].pgoName.empty() || !ID.schema[I].numCounters ||
        (I && ID.schema[I - 1].pgoName == ID.schema[I].pgoName))
      return reject("candidate invalid schema");
  ID.ir = std::move(CanonicalIR);
  ID.digest = SHA256::hash(arrayRefFromStringRef(ID.ir));
  return ID;
}
} // namespace
struct EJitCandidateDirectory::Impl {
  EJitCandidateLimits limits;
  BucketHash hash;
  uint64_t bytes = 0, nextGroup = 1;
  uint32_t count = 0;
  DenseMap<uint64_t, std::vector<std::pair<uint64_t, CandidateIdentity>>>
      groups;
  Impl(EJitCandidateLimits L, BucketHash H)
      : limits(L), hash(H ? H : defaultBucketHash) {}
};
EJitCandidateDirectory::EJitCandidateDirectory(EJitCandidateLimits L,
                                               BucketHash H)
    : P(std::make_unique<Impl>(L, H)) {}
EJitCandidateDirectory::~EJitCandidateDirectory() = default;
Expected<EJitCandidateResult>
EJitCandidateDirectory::classify(const Module &M, EJitCodeIdentityScope Scope,
                                 ArrayRef<EJitCodeBinding> Bindings,
                                 ArrayRef<PgoFunctionSchema> Schema) {
  if (Error E = checkCandidateModule(M, Scope, Bindings))
    return std::move(E);
  std::string CanonicalIR;
  if (Error E =
          canonicalizeIdentityIR(M, CanonicalIR, P->limits.maxModuleBytes))
    return std::move(E);
  return classifyCanonical(std::move(CanonicalIR), std::move(Scope), Bindings,
                           Schema);
}
Expected<EJitCandidateResult> EJitCandidateDirectory::classifyCanonical(
    std::string CanonicalIR, EJitCodeIdentityScope Scope,
    ArrayRef<EJitCodeBinding> Bindings, ArrayRef<PgoFunctionSchema> Schema) {
  if (!candidateInputFits(P->limits.maxIdentityBytes, Scope, Bindings, Schema,
                          CanonicalIR))
    return exhausted("candidate identity budget exceeded");
  auto Made = makeCandidate(std::move(CanonicalIR), std::move(Scope), Bindings,
                            Schema);
  if (!Made)
    return Made.takeError();
  CandidateIdentity ID = std::move(*Made);
  const uint64_t IdentityBytes = ID.bytes();
  const uint64_t Hash = P->hash(ID.digest);
  auto It = P->groups.find(Hash);
  if (It != P->groups.end())
    for (const auto &E : It->second)
      if (sameCandidate(E.second, ID))
        return EJitCandidateResult{E.first, true};
  if (P->count >= P->limits.maxGroups ||
      P->bytes > P->limits.maxIdentityBytes ||
      IdentityBytes > P->limits.maxIdentityBytes - P->bytes)
    return exhausted("candidate directory budget exceeded");
  uint64_t G = P->nextGroup++;
  P->bytes += IdentityBytes;
  ++P->count;
  P->groups[Hash].push_back({G, std::move(ID)});
  return EJitCandidateResult{G, false};
}
uint32_t EJitCandidateDirectory::groupCount() const {
  return P->count;
}
uint64_t EJitCandidateDirectory::identityBytes() const {
  return P->bytes;
}
EJitCandidateCapture::EJitCandidateCapture(EJitCandidateDirectory &D,
                                           EJitCodeIdentityScope S,
                                           ArrayRef<EJitCodeBinding> B)
    : Directory(D), Scope(std::move(S)) {
  if (!candidateInputFits(Directory.P->limits.maxIdentityBytes, Scope, B, {})) {
    Failure = exhausted("candidate identity budget exceeded");
    return;
  }
  Bindings.assign(B.begin(), B.end());
}
EJitCandidateCapture::~EJitCandidateCapture() {
  consumeError(std::move(Failure));
}
void EJitCandidateCapture::capturePrefix(const Module &M) {
  if (PrefixCaptured || Completed)
    return;
  PrefixCaptured = true;
  if (Failure)
    return;
  if (Error E = checkCandidateModule(M, Scope, Bindings)) {
    Failure = std::move(E);
    return;
  }
  if (Error E = canonicalizeIdentityIR(
          M, CanonicalIR,
          std::min(Directory.P->limits.maxModuleBytes,
                   Directory.P->limits.maxIdentityBytes)))
    Failure = std::move(E);
}
void EJitCandidateCapture::complete(ArrayRef<PgoFunctionSchema> Schema) {
  if (Completed)
    return;
  Completed = true;
  if (!PrefixCaptured) {
    if (!Failure)
      Failure = reject("candidate prefix not reached");
    return;
  }
  if (Failure)
    return;
  auto R = Directory.classifyCanonical(std::move(CanonicalIR), std::move(Scope),
                                       Bindings, Schema);
  if (R)
    Result = *R;
  else
    Failure = R.takeError();
}
Expected<EJitCandidateResult> EJitCandidateCapture::takeResult() {
  if (!Completed)
    return reject("candidate schema not reached");
  if (Failure)
    return std::move(Failure);
  if (!Result)
    return reject("candidate classification missing");
  return *Result;
}
