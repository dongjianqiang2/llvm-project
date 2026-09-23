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
      for (Instruction &I : BB) {
        I.setMetadata(FrozenSiteMD, nullptr);
        if (!I.getType()->isVoidTy())
          I.setName("");
      }
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

std::pair<std::string, bool> boundedDiagnosticValue(StringRef Value) {
  const size_t Limit = EJitIdentityDiagnostic::MaxValueBytes;
  if (Value.size() <= Limit)
    return {Value.str(), false};
  return {Value.substr(0, Limit).str(), true};
}

std::string digestText(ArrayRef<uint8_t> Digest) {
  static constexpr char Hex[] = "0123456789abcdef";
  std::string Out;
  Out.reserve(Digest.size() * 2);
  for (uint8_t Byte : Digest) {
    Out.push_back(Hex[Byte >> 4]);
    Out.push_back(Hex[Byte & 0xf]);
  }
  return Out;
}

void setDiagnosticValues(EJitIdentityDiagnostic &D, StringRef Field,
                         StringRef LHS, StringRef RHS) {
  auto Label = boundedDiagnosticValue(Field);
  D.field = std::move(Label.first);
  auto Left = boundedDiagnosticValue(LHS);
  auto Right = boundedDiagnosticValue(RHS);
  D.lhsValue = std::move(Left.first);
  D.rhsValue = std::move(Right.first);
  D.valuesTruncated = Label.second || Left.second || Right.second;
}

void setDiagnosticValues(EJitIdentityDiagnostic &D, StringRef Field,
                         uint64_t LHS, uint64_t RHS) {
  setDiagnosticValues(D, Field, std::to_string(LHS), std::to_string(RHS));
}

std::pair<std::string, bool> boundedNamedValue(StringRef Name,
                                               StringRef Value) {
  const size_t Limit = EJitIdentityDiagnostic::MaxValueBytes;
  const size_t NameLimit = std::min(Name.size(), Limit / 2);
  std::string Out = Name.substr(0, NameLimit).str();
  bool Truncated = NameLimit != Name.size();
  if (Truncated && Out.size() < Limit)
    Out.back() = '~';
  if (Out.size() < Limit)
    Out.push_back('=');
  const size_t ValueLimit = std::min(Value.size(), Limit - Out.size());
  Out.append(Value.data(), ValueLimit);
  Truncated |= ValueLimit != Value.size();
  return {std::move(Out), Truncated};
}

void setNamedDiagnosticValues(EJitIdentityDiagnostic &D, StringRef Field,
                              StringRef LName, StringRef LValue,
                              StringRef RName, StringRef RValue) {
  auto Left = boundedNamedValue(LName, LValue);
  auto Right = boundedNamedValue(RName, RValue);
  setDiagnosticValues(D, Field, Left.first, Right.first);
  D.valuesTruncated |= Left.second || Right.second;
}

void attachIRExcerpts(EJitIdentityDiagnostic &D, StringRef LHS, StringRef RHS,
                      uint32_t RequestedBytes) {
  const size_t Limit =
      std::min<uint32_t>(RequestedBytes, EJitIdentityDiagnostic::MaxExcerptBytes);
  if (!Limit)
    return;

  size_t Offset = 0;
  const size_t Common = std::min(LHS.size(), RHS.size());
  while (Offset < Common && LHS[Offset] == RHS[Offset])
    ++Offset;
  D.firstDiffOffset = Offset;
  D.firstDiffLine = 1;
  for (size_t I = 0; I < Offset; ++I)
    D.firstDiffLine += LHS[I] == '\n';

  auto Excerpt = [&](StringRef Text, std::string &Out) {
    if (Text.empty()) {
      return;
    }
    size_t Start = Offset > Limit / 2 ? Offset - Limit / 2 : 0;
    if (Start > Text.size())
      Start = Text.size();
    if (Start + Limit > Text.size())
      Start = Text.size() > Limit ? Text.size() - Limit : 0;
    size_t Length = std::min<size_t>(Limit, Text.size() - Start);
    Out = Text.substr(Start, Length).str();
    D.excerptsTruncated |= Start != 0 || Length != Text.size();
  };
  Excerpt(LHS, D.lhsExcerpt);
  Excerpt(RHS, D.rhsExcerpt);
}

EJitIdentityDiagnostic explainIdentity(
    const EJitCodeIdentityScope &LeftScope,
    ArrayRef<EJitCodeBinding> LeftBindings,
    ArrayRef<PgoFunctionSchema> LeftSchema, StringRef LeftIR,
    const EJitCodeIdentityScope &RightScope,
    ArrayRef<EJitCodeBinding> RightBindings,
    ArrayRef<PgoFunctionSchema> RightSchema, StringRef RightIR,
    uint32_t ExcerptBytes, bool CandidatePrefix = false) {
  EJitIdentityDiagnostic D;
  if (LeftScope.source != RightScope.source) {
    D.kind = EJitIdentityDiagnostic::Kind::SourceMismatch;
    setDiagnosticValues(D, "scope.source", digestText(LeftScope.source),
                       digestText(RightScope.source));
    return D;
  }
  if (LeftScope.entry != RightScope.entry) {
    D.kind = EJitIdentityDiagnostic::Kind::EntryMismatch;
    setDiagnosticValues(D, "scope.entry", LeftScope.entry, RightScope.entry);
    return D;
  }
  if (LeftScope.compilerPolicy != RightScope.compilerPolicy) {
    D.kind = EJitIdentityDiagnostic::Kind::PolicyMismatch;
    setDiagnosticValues(D, "scope.compilerPolicy", LeftScope.compilerPolicy,
                       RightScope.compilerPolicy);
    return D;
  }

  const size_t CommonBindings =
      std::min(LeftBindings.size(), RightBindings.size());
  for (size_t I = 0; I < CommonBindings; ++I) {
    const EJitCodeBinding &L = LeftBindings[I];
    const EJitCodeBinding &R = RightBindings[I];
    if (L.name != R.name) {
      D.kind = EJitIdentityDiagnostic::Kind::BindingNameMismatch;
      setDiagnosticValues(D, "binding.name", L.name, R.name);
      return D;
    }
    if (L.address != R.address) {
      D.kind = EJitIdentityDiagnostic::Kind::BindingAddressMismatch;
      setNamedDiagnosticValues(D, "binding.address", L.name,
                               "0x" + utohexstr(L.address), R.name,
                               "0x" + utohexstr(R.address));
      return D;
    }
    if (L.callable != R.callable) {
      D.kind = EJitIdentityDiagnostic::Kind::BindingCallableMismatch;
      setNamedDiagnosticValues(D, "binding.callable", L.name,
                               L.callable ? "true" : "false", R.name,
                               R.callable ? "true" : "false");
      return D;
    }
  }
  if (LeftBindings.size() != RightBindings.size()) {
    D.kind = EJitIdentityDiagnostic::Kind::BindingCountMismatch;
    setDiagnosticValues(D, "binding.count", LeftBindings.size(),
                       RightBindings.size());
    return D;
  }
  if (LeftScope.bindingGeneration != RightScope.bindingGeneration) {
    D.kind = EJitIdentityDiagnostic::Kind::BindingGenerationMismatch;
    setDiagnosticValues(D, "scope.bindingGeneration",
                       LeftScope.bindingGeneration,
                       RightScope.bindingGeneration);
    return D;
  }

  const size_t CommonSchema = std::min(LeftSchema.size(), RightSchema.size());
  for (size_t I = 0; I < CommonSchema; ++I) {
    const PgoFunctionSchema &L = LeftSchema[I];
    const PgoFunctionSchema &R = RightSchema[I];
    if (L.pgoName != R.pgoName) {
      D.kind = EJitIdentityDiagnostic::Kind::SchemaNameMismatch;
      setDiagnosticValues(D, "schema.pgoName", L.pgoName, R.pgoName);
      return D;
    }
    auto SchemaDifference = [&](EJitIdentityDiagnostic::Kind Kind,
                                StringRef Field, uint64_t Left,
                                uint64_t Right) {
      D.kind = Kind;
      setNamedDiagnosticValues(D, Field, L.pgoName, std::to_string(Left),
                               R.pgoName, std::to_string(Right));
    };
    if (L.funcHash != R.funcHash) {
      SchemaDifference(EJitIdentityDiagnostic::Kind::SchemaFuncHashMismatch,
                       "schema.funcHash", L.funcHash, R.funcHash);
      return D;
    }
    if (L.pgoNameHash != R.pgoNameHash) {
      SchemaDifference(
          EJitIdentityDiagnostic::Kind::SchemaNameHashMismatch,
          "schema.pgoNameHash", L.pgoNameHash, R.pgoNameHash);
      return D;
    }
    if (L.numCounters != R.numCounters) {
      SchemaDifference(
          EJitIdentityDiagnostic::Kind::SchemaCounterCountMismatch,
          "schema.numCounters", L.numCounters, R.numCounters);
      return D;
    }
    if (L.numIcSites != R.numIcSites) {
      SchemaDifference(EJitIdentityDiagnostic::Kind::SchemaIcSiteCountMismatch,
                       "schema.numIcSites", L.numIcSites, R.numIcSites);
      return D;
    }
    if (L.numMemSites != R.numMemSites) {
      SchemaDifference(EJitIdentityDiagnostic::Kind::SchemaMemSiteCountMismatch,
                       "schema.numMemSites", L.numMemSites, R.numMemSites);
      return D;
    }
    if (L.numScalarSites != R.numScalarSites) {
      SchemaDifference(
          EJitIdentityDiagnostic::Kind::SchemaScalarSiteCountMismatch,
          "schema.numScalarSites", L.numScalarSites, R.numScalarSites);
      return D;
    }
  }
  if (LeftSchema.size() != RightSchema.size()) {
    D.kind = EJitIdentityDiagnostic::Kind::SchemaCountMismatch;
    setDiagnosticValues(D, "schema.count", LeftSchema.size(),
                       RightSchema.size());
    return D;
  }
  if (LeftIR != RightIR) {
    D.kind = CandidatePrefix ? EJitIdentityDiagnostic::Kind::PrefixIRMismatch
                             : EJitIdentityDiagnostic::Kind::IRMismatch;
    setDiagnosticValues(D, CandidatePrefix ? "prefix_ir" : "canonical_ir",
                        LeftIR.size(), RightIR.size());
    size_t Offset = 0;
    const size_t Common = std::min(LeftIR.size(), RightIR.size());
    while (Offset < Common && LeftIR[Offset] == RightIR[Offset])
      ++Offset;
    D.firstDiffOffset = Offset;
    D.firstDiffLine = 1;
    for (size_t I = 0; I < Offset; ++I)
      D.firstDiffLine += LeftIR[I] == '\n';
    attachIRExcerpts(D, LeftIR, RightIR, ExcerptBytes);
  }
  return D;
}
} // namespace

bool EJitCodeBinding::operator==(const EJitCodeBinding &Other) const {
  return name == Other.name && address == Other.address &&
         callable == Other.callable;
}

uint64_t llvm::ejit::EJitBindingGeneration(
    ArrayRef<EJitCodeBinding> Bindings) {
  std::vector<const EJitCodeBinding *> Ordered;
  Ordered.reserve(Bindings.size());
  for (const EJitCodeBinding &B : Bindings)
    Ordered.push_back(&B);
  std::sort(Ordered.begin(), Ordered.end(), [](const EJitCodeBinding *A,
                                               const EJitCodeBinding *B) {
    if (A->name != B->name)
      return A->name < B->name;
    if (A->callable != B->callable)
      return A->callable < B->callable;
    return A->address < B->address;
  });

  // FNV-1a is sufficient here because the full bindings are still retained
  // and compared exactly; this value only prevents unrelated registration
  // growth from creating a different candidate scope.
  uint64_t Hash = 1469598103934665603ull;
  auto Mix = [&](uint8_t Byte) {
    Hash ^= Byte;
    Hash *= 1099511628211ull;
  };
  for (const EJitCodeBinding *B : Ordered) {
    for (unsigned I = 0; I != sizeof(B->address); ++I)
      Mix(static_cast<uint8_t>(B->address >> (I * 8)));
    Mix(B->callable ? 1 : 0);
    for (unsigned I = 0; I != sizeof(uint64_t); ++I)
      Mix(static_cast<uint8_t>(B->name.size() >> (I * 8)));
    for (unsigned char Byte : B->name)
      Mix(Byte);
  }
  return Hash ? Hash : 1;
}

bool EJitCodeIdentityScope::operator==(
    const EJitCodeIdentityScope &Other) const {
  return source == Other.source && entry == Other.entry &&
         compilerPolicy == Other.compilerPolicy &&
         bindingGeneration == Other.bindingGeneration;
}

StringRef EJitIdentityDiagnostic::reasonToken() const {
  switch (kind) {
  case Kind::Equal:
    return "IDENTITY_EQUAL";
  case Kind::SourceMismatch:
    return "SOURCE_DIFF";
  case Kind::EntryMismatch:
    return "ENTRY_DIFF";
  case Kind::PolicyMismatch:
    return "POLICY_DIFF";
  case Kind::BindingNameMismatch:
    return "BINDING_NAME_DIFF";
  case Kind::BindingAddressMismatch:
    return "BINDING_ADDRESS_DIFF";
  case Kind::BindingCallableMismatch:
    return "BINDING_CALLABLE_DIFF";
  case Kind::BindingCountMismatch:
    return "BINDING_COUNT_DIFF";
  case Kind::BindingGenerationMismatch:
    return "BINDING_GENERATION_DIFF";
  case Kind::SchemaNameMismatch:
    return "SCHEMA_NAME_DIFF";
  case Kind::SchemaFuncHashMismatch:
    return "SCHEMA_FUNC_HASH_DIFF";
  case Kind::SchemaNameHashMismatch:
    return "SCHEMA_NAME_HASH_DIFF";
  case Kind::SchemaCounterCountMismatch:
    return "SCHEMA_COUNTER_COUNT_DIFF";
  case Kind::SchemaIcSiteCountMismatch:
    return "SCHEMA_IC_SITE_COUNT_DIFF";
  case Kind::SchemaMemSiteCountMismatch:
    return "SCHEMA_MEM_SITE_COUNT_DIFF";
  case Kind::SchemaScalarSiteCountMismatch:
    return "SCHEMA_SCALAR_SITE_COUNT_DIFF";
  case Kind::SchemaCountMismatch:
    return "SCHEMA_COUNT_DIFF";
  case Kind::PrefixIRMismatch:
    return "PREFIX_IR_DIFF";
  case Kind::IRMismatch:
    return "FINAL_IR_DIFF";
  }
  llvm_unreachable("unhandled EJit identity diagnostic kind");
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

std::optional<EJitIdentityDiagnostic>
EJitPreparedCodeEmitter::explainLinkedIdentity(
    uint64_t CodeId, const EJitFinalCodeIdentity &Other,
    uint32_t ExcerptBytes) const {
  for (const Impl::Record &R : P->records)
    if (R.codeId == CodeId)
      return explainIdentity(R.identity.scope_, R.identity.bindings_, {},
                             R.identity.ir_, Other.scope_, Other.bindings_,
                             {}, Other.ir_, ExcerptBytes);
  return std::nullopt;
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
  // Separate bounded diagnostic storage, excluded from equality/hash/budget
  // admission. Missing history must not change compilation or reuse.
  DenseMap<uint64_t, std::unique_ptr<EJitFrozenSnapshot>> frozen;
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
                                 ArrayRef<PgoFunctionSchema> Schema,
                                 EJitIdentityDiagnosticOptions Diag) {
  if (Error E = checkCandidateModule(M, Scope, Bindings))
    return std::move(E);
  std::string CanonicalIR;
  if (Error E =
          canonicalizeIdentityIR(M, CanonicalIR, P->limits.maxModuleBytes))
    return std::move(E);
  return classifyCanonical(std::move(CanonicalIR), std::move(Scope), Bindings,
                           Schema, Diag);
}
Expected<EJitCandidateResult> EJitCandidateDirectory::classifyCanonical(
    std::string CanonicalIR, EJitCodeIdentityScope Scope,
    ArrayRef<EJitCodeBinding> Bindings, ArrayRef<PgoFunctionSchema> Schema,
    EJitIdentityDiagnosticOptions Diag, const EJitFrozenSnapshot *Frozen) {
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
  EJitCandidateResult Result{G, false};
  if (Diag.explain) {
    const CandidateIdentity *Related = nullptr;
    uint64_t RelatedGroup = 0;
    // The directory's hash buckets are an implementation detail. Search all
    // stored identities for a stable same-entry/source comparison peer.
    for (const auto &Bucket : P->groups)
      for (const auto &Candidate : Bucket.second)
        if (Candidate.second.scope.entry == ID.scope.entry &&
            Candidate.second.scope.source == ID.scope.source &&
            (!RelatedGroup || Candidate.first < RelatedGroup)) {
          Related = &Candidate.second;
          RelatedGroup = Candidate.first;
        }
    if (Related) {
      Result.relatedGroupId = RelatedGroup;
      Result.diagnostic = explainIdentity(
          Related->scope, Related->bindings, Related->schema, Related->ir,
          ID.scope, ID.bindings, ID.schema, ID.ir, Diag.excerptBytes,
          /*CandidatePrefix=*/true);
      auto History = P->frozen.find(RelatedGroup);
      if (Result.diagnostic && Frozen && History != P->frozen.end())
        Result.diagnostic->frozen = compareFrozen(*History->second, *Frozen);
    }
  }
  P->bytes += IdentityBytes;
  ++P->count;
  P->groups[Hash].push_back({G, std::move(ID)});
  if (Frozen && P->frozen.size() < 128)
    P->frozen[G] = std::make_unique<EJitFrozenSnapshot>(*Frozen);
  return Result;
}
uint32_t EJitCandidateDirectory::groupCount() const {
  return P->count;
}
uint64_t EJitCandidateDirectory::identityBytes() const {
  return P->bytes;
}
EJitCandidateCapture::EJitCandidateCapture(EJitCandidateDirectory &D,
                                           EJitCodeIdentityScope S,
                                           ArrayRef<EJitCodeBinding> B,
                                           EJitIdentityDiagnosticOptions Diag)
    : Directory(D), Scope(std::move(S)), Diag(Diag) {
  if (!candidateInputFits(Directory.P->limits.maxIdentityBytes, Scope, B, {})) {
    // Error move-assignment requires the current value (including Success) to
    // have been checked before it is replaced.
    consumeError(std::move(Failure));
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
                                        Bindings, Schema, Diag, &frozen);
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
