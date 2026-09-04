//===-- EJitCommon.h - EmbeddedJIT Shared Constants -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Shared string constants and configuration values used across the
//  EmbeddedJIT AOT passes, runtime library, and Clang CodeGen.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCOMMON_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCOMMON_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitBoundPtr.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>

//===----------------------------------------------------------------------===//
// Multi-version inline-cache sizing. The per-function @__ejit_icache_fn_<name>
// global is a [D]^numDims array (D = EJIT_ICACHE_DIM_SIZE) indexed by the
// ejit_dim argument values. D MUST be a power of 2 (the hit path indexes with
// shifts, no multiply). The CMake EJIT_ICACHE_DIM_SIZE var overrides this
// default and is applied to BOTH the AOT pass (LLVMEmbeddedJIT, which emits
// the array type) and the runtime (LLVMEJIT, which linearizes on fill) so the
// two always agree. EJIT_ICACHE_MAX_DIMS caps the dimensionality; an ejit_entry
// with more ejit_dim params is a compile error.
//===----------------------------------------------------------------------===//
#ifndef EJIT_ICACHE_DIM_SIZE
#define EJIT_ICACHE_DIM_SIZE 16u
#endif
#ifndef EJIT_ICACHE_MAX_DIMS
#define EJIT_ICACHE_MAX_DIMS 4u
#endif

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// Metadata kind names
//===----------------------------------------------------------------------===//
constexpr const char *MD_EJIT_METADATA = "ejit.metadata";
constexpr const char *MD_EJIT_MAY_CONST = "ejit.may_const";

//===----------------------------------------------------------------------===//
// Metadata entry tags (first operand of sub-nodes in !ejit.metadata)
//===----------------------------------------------------------------------===//
constexpr const char *TAG_EJIT_ENTRY = "ejit_entry";
constexpr const char *TAG_EJIT_PERIOD_LC = "ejit_period_lc";
constexpr const char *TAG_EJIT_PERIOD_ARR_IND = "ejit_period_arr_ind";
constexpr const char *TAG_EJIT_BOUND_PTR = "ejit_bound_ptr";
// Same {tag, MDString, i32 argIndex} shape as TAG_EJIT_PERIOD_ARR_IND; the
// MDString is always empty (a const dim names no period).
constexpr const char *TAG_EJIT_CONST_DIM = "ejit_const_dim";
constexpr const char *TAG_EJIT_PERIOD_ARR = "ejit_period_arr";
constexpr const char *TAG_EJIT_PERIOD = "ejit_period";
constexpr const char *TAG_EJIT_MAY_CONST_FIELD = "ejit_may_const_field";

// PASS1 records the process-unique AOT wrapper symbol for local ejit_entry
// functions on the embedded-bitcode clone. The JIT uses it only when that
// entry is externalized as a nested specialization boundary; compiling the
// entry itself continues to use its original source-level name.
constexpr const char *ATTR_EJIT_WRAPPER_SYMBOL = "ejit.wrapper_symbol";

//===----------------------------------------------------------------------===//
// Global variable and section names
//===----------------------------------------------------------------------===//
constexpr const char *GV_EJIT_BITCODE = "__ejit_bitcode";
// Registry input sections. MUST match the linker-script KEEP() patterns and
// the __start_/__stop_ bounds symbols the runtime walks (EJit.cpp,
// ejit_registry.ld): a typo on either side yields an empty registry with no
// diagnostic. Keep these constants as the single source for the AOT side.
constexpr const char *SECT_EJIT_BITCODE = ".ejit_bitcode";
constexpr const char *SECT_EJIT_PERIOD = ".ejit_period";
constexpr const char *FN_AUTO_REGISTER = "ejit_auto_register";
constexpr const char *CTORS_GLOBAL = "llvm.global_ctors";

//===----------------------------------------------------------------------===//
// Runtime function names (extern symbols called by AOT-generated code)
//===----------------------------------------------------------------------===//
constexpr const char *FN_REGISTER_BITCODE = "ejit_register_bitcode";
// Symbol registration for the AOT-visible static-var symbol table
// (EJitRuntime.cpp defines this entry point; the AOT pass emits the call).
constexpr const char *FN_REGISTER_SYMBOL = "ejit_register_symbol";
constexpr const char *FN_REGISTER_PERIOD_ARRAY = "ejit_register_period_array";
constexpr const char *FN_REGISTER_STATIC_VAR = "ejit_register_static_var";
constexpr const char *FN_REGISTER_LIFECYCLE = "ejit_register_lifecycle";
constexpr const char *FN_REGISTER_FUNCINDEX = "ejit_register_funcindex";
// Per-function inline-cache slot registration: the wrapper's per-function
// @__ejit_icache_fn_<name> global (a frozen, sticky specialization pointer) is
// registered by name so the runtime can backfill it on a successful resolve
// (icacheFill). The wrapper reads it directly with an inline atomic load - no
// ejit_icache_try call - so the hit path is one load + indirect call (plus a
// null-check only on guarded 3D/4D shapes; <=2D tables are defined pre-filled
// with &MissFn and BLR it branchlessly). Signature: void
// ejit_register_icache_slot(const char *name, void *slot, uint32_t numDims,
// void *missFn), where missFn is the slot's sentinel for branchless tables
// (drain/fill-retract write it back instead of 0) and null for guarded ones.
constexpr const char *FN_REGISTER_ICACHE_SLOT = "ejit_register_icache_slot";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET =
    "ejit_taskpool_compile_or_get";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_BOUND =
    "ejit_taskpool_compile_or_get_bound";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_BOUND_V =
    "ejit_taskpool_compile_or_get_bound_v";
// Fixed-dimension fast-path C ABI entries (0-4 dims), emitted by the wrapper
// when -ejit-wrapper-fixed-dim-entry is enabled and the entry has <= 4 dims.
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_0D =
    "ejit_taskpool_compile_or_get_0d";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_1D =
    "ejit_taskpool_compile_or_get_1d";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_2D =
    "ejit_taskpool_compile_or_get_2d";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_3D =
    "ejit_taskpool_compile_or_get_3d";
constexpr const char *FN_TASKPOOL_COMPILE_OR_GET_4D =
    "ejit_taskpool_compile_or_get_4d";
constexpr const char *FN_TASKPOOL_RELEASE_READ = "ejit_taskpool_release_read";
constexpr const char *FN_TASKPOOL_TRACE_NOW = "ejit_taskpool_trace_now";
constexpr const char *FN_TASKPOOL_TRACE_WRAPPER =
    "ejit_taskpool_trace_wrapper";
constexpr const char *FN_FUNCTION_BODY_CYCLES_RECORD =
    "ejit_function_body_cycles_record";
constexpr uint32_t kEJitFunctionBodyPathAOT = 0u;
constexpr uint32_t kEJitFunctionBodyPathJIT = 1u;
// Wrapper-timing sentinel status passed to ejit_taskpool_trace_wrapper for
// icache-hit samples so they aggregate as their own report line (get_fn_avg =
// probe cost, release_avg = 0), separate from the slow-path compile_or_get
// samples. The value is RESERVED in the ejit_status_t enum itself
// (EJIT_STATUS_ICACHE_HIT_SENTINEL = 0xFE in EJitRuntime.h): a future real
// status assigned 0xFE is a duplicate-enumerator compile error, and a
// static_assert in EJitRuntime.cpp locks this constant to that enumerator.
constexpr uint32_t kEJitIcacheHitTimingStatus = 0xFEu;
// Lifecycle activation is keyed by period/lifecycle name + instance index only.
// PASS4 emits these name-level calls at ejit_period_lc entry/exit; there is no
// array-pointer dimension in the active-state hot path.
constexpr const char *FN_DEACTIVATE = "ejit_deactivate";
constexpr const char *FN_ACTIVATE = "ejit_activate";

//===----------------------------------------------------------------------===//
// Constructor priority (lower = later; 65535 runs last)
//===----------------------------------------------------------------------===//
constexpr unsigned EJIT_CTOR_PRIORITY = 65535;

//===----------------------------------------------------------------------===//
// Limits
//===----------------------------------------------------------------------===//
// Sema consumes these via using-declarations (SemaEJIT.cpp). The user-facing
// diagnostic TEXTS in clang/include/clang/Basic/DiagnosticSemaKinds.td
// ("exceeds the maximum of 100" / "of 4") are a second copy — update them
// together with these values.
constexpr unsigned MAX_PERIOD_ARR_IND_PARAMS = 4;
constexpr unsigned MAX_PERIOD_ARR_SIZE = 100;
constexpr unsigned MAX_BOUND_PTR_PARAMS = kEJitMaxBoundPointers;

//===----------------------------------------------------------------------===//
// Metadata utility functions (shared across AOT passes)
//===----------------------------------------------------------------------===//

inline bool hasMDStringEntry(const MDNode *Node, StringRef Name) {
  if (!Node)
    return false;
  for (const MDOperand &Op : Node->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() == 0)
      continue;
    if (auto *S = dyn_cast<MDString>(Sub->getOperand(0)))
      if (S->getString() == Name)
        return true;
  }
  return false;
}

inline StringRef getMDStringValue(const MDNode *Node, StringRef Tag) {
  if (!Node)
    return {};
  for (const MDOperand &Op : Node->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (Sub && Sub->getNumOperands() >= 2) {
      if (auto *S = dyn_cast<MDString>(Sub->getOperand(0)))
        if (S->getString() == Tag)
          if (auto *V = dyn_cast<MDString>(Sub->getOperand(1)))
            return V->getString();
    }
  }
  return {};
}

inline uint32_t getMDIntValue(const MDNode *Node, StringRef Tag) {
  if (!Node)
    return 0;
  for (const MDOperand &Op : Node->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (Sub && Sub->getNumOperands() >= 3) {
      if (auto *S = dyn_cast<MDString>(Sub->getOperand(0)))
        if (S->getString() == Tag)
          if (auto *C = dyn_cast<ConstantAsMetadata>(Sub->getOperand(2)))
            if (auto *CI = dyn_cast<ConstantInt>(C->getValue()))
              return static_cast<uint32_t>(CI->getZExtValue());
    }
  }
  return 0;
}

/// Byte offset of a field access *within its period-array element*, which is
/// the coordinate the `ejit_may_const_field` entries on a period-array global
/// are expressed in (they are relative to the element struct, NOT to the array
/// base). Writes the root global to \p RootGV.
///
/// A load's pointer may take any of these shapes, all of which must reduce to
/// the same field offset:
///
///   getelementptr [16 x %S], @g, 0, %ci, i32 4   AOT: dynamic element index
///   getelementptr [16 x %S], @g, 0, 3,   i32 4   constant element index
///   getelementptr %S,        @g, %ci             decayed array-to-pointer
///   getelementptr i8,        @g, 100             post-InstCombine byte offset
///
/// Struct indices contribute their field offset, constant sequential indices
/// their stride. Exactly ONE dynamic sequential index may be skipped: the one
/// selecting an element of the root period array. Its contribution is a whole
/// number of elements, which the closing `% ElemSize` erases, so skipping it is
/// equivalent to including it -- and that is what lets `g_cfg[ci].field` resolve
/// at AOT time, before the JIT replaces `ci` with a constant.
///
/// Any other dynamic index is rejected: typed pointer arithmetic wears the same
/// shape while walking *within* an element, e.g. `((int *)&g[ci])[j]`, where
/// skipping `%j` would misattribute the access to the field at offset 0.
///
/// Returns nullopt when the access cannot be attributed to a fixed field.
inline std::optional<uint64_t>
ejitMayConstFieldOffset(const Value *Ptr, const DataLayout &DL,
                        const GlobalVariable *&RootGV) {
  RootGV = nullptr;

  // Phase 1: walk to the root global, remembering the path. An index cannot be
  // judged before the root is known: "is this the element selector?" is a
  // question about the root array's stride.
  SmallVector<const GEPOperator *, 4> Path;
  const Value *V = Ptr;
  while (V) {
    V = V->stripPointerCasts();
    if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
      RootGV = GV;
      break;
    }
    const auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      return std::nullopt;
    Path.push_back(GEP);
    V = GEP->getPointerOperand();
  }
  if (!RootGV)
    return std::nullopt;

  // Element stride of the period array. Zero when the global is not an array (a
  // scalar `ejit_period` global), in which case no dynamic index is admissible.
  uint64_t ElemSize = 0;
  if (const auto *ATy = dyn_cast<ArrayType>(RootGV->getValueType())) {
    TypeSize TS = DL.getTypeAllocSize(ATy->getElementType());
    if (TS.isScalable())
      return std::nullopt;
    ElemSize = TS.getFixedValue();
  }

  // Only the GEP applied directly to the global can carry the element selector.
  const GEPOperator *RootGEP = Path.empty() ? nullptr : Path.back();

  // Phase 2: accumulate, validating each dynamic index against the root stride.
  // At most ONE dynamic index may be skipped: an array has a single element
  // selector. Two indices can both match the stride test (e.g. `[16 x [1 x i32]]`
  // where the element and its own element are the same size), so the acceptance
  // has to be recorded rather than re-derived per index.
  bool SkippedDynamic = false;
  uint64_t Off = 0;
  for (const GEPOperator *GEP : Path) {
    for (auto GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP); GTI != GTE;
         ++GTI) {
      if (StructType *STy = GTI.getStructTypeOrNull()) {
        const auto *CI = dyn_cast<ConstantInt>(GTI.getOperand());
        if (!CI)
          return std::nullopt; // struct indices are constant in valid IR
        Off += DL.getStructLayout(STy)->getElementOffset(CI->getZExtValue());
        continue;
      }

      TypeSize TS = DL.getTypeAllocSize(GTI.getIndexedType());
      if (TS.isScalable())
        return std::nullopt;
      const uint64_t Stride = TS.getFixedValue();

      if (const auto *CI = dyn_cast<ConstantInt>(GTI.getOperand())) {
        // GEP indices are signed; a negative one cannot be attributed to a
        // field, and zero-extending it would wrap into a plausible offset.
        const int64_t Idx = CI->getSExtValue();
        if (Idx < 0)
          return std::nullopt;
        Off += static_cast<uint64_t>(Idx) * Stride;
        continue;
      }

      // Dynamic index: admissible only as the root array's element selector,
      // and only once.
      if (!SkippedDynamic && GEP == RootGEP && ElemSize != 0 &&
          Stride == ElemSize) {
        SkippedDynamic = true;
        continue;
      }
      return std::nullopt;
    }
  }

  if (ElemSize)
    Off %= ElemSize; // drop whole elements: keep only the field coordinate
  return Off;
}

/// Size in bytes of the may_const field beginning at \p Off inside the period
/// element of \p RootGV, or nullopt when \p Off is not the start of a field.
///
/// The `ejit_may_const_field` metadata records offsets and nothing else, so it
/// cannot bound how many bytes an access may read; the aggregate layout supplies
/// that bound. A nested aggregate shares its offset with its own first field, so
/// the descent continues to the innermost field starting at \p Off. That is the
/// conservative choice, and it is what rejects a widened load (say an i64 formed
/// from a memcpy over a may_const i32 and the mutable i32 beside it).
inline std::optional<uint64_t>
ejitMayConstFieldSize(const GlobalVariable *RootGV, uint64_t Off,
                      const DataLayout &DL) {
  if (!RootGV)
    return std::nullopt;
  Type *Ty = RootGV->getValueType();
  if (const auto *ATy = dyn_cast<ArrayType>(Ty))
    Ty = ATy->getElementType();

  // Descend into whichever element contains Off, rebasing Off as we go. Only at
  // the leaf does Off == 0 mean "this is where a field begins".
  while (true) {
    if (auto *STy = dyn_cast<StructType>(Ty)) {
      if (STy->isOpaque())
        return std::nullopt;
      const StructLayout *SL = DL.getStructLayout(STy);
      if (Off >= SL->getSizeInBytes())
        return std::nullopt;
      const unsigned Idx = SL->getElementContainingOffset(Off);
      Off -= SL->getElementOffset(Idx);
      Ty = STy->getElementType(Idx);
      continue;
    }
    if (auto *ATy = dyn_cast<ArrayType>(Ty)) {
      if (ATy->getNumElements() == 0)
        return std::nullopt;
      TypeSize ES = DL.getTypeAllocSize(ATy->getElementType());
      if (ES.isScalable() || ES.getFixedValue() == 0)
        return std::nullopt;
      Off %= ES.getFixedValue();
      Ty = ATy->getElementType();
      continue;
    }
    break;
  }

  if (Off != 0)
    return std::nullopt; // lands inside a field, not at its start

  TypeSize TS = DL.getTypeStoreSize(Ty);
  if (TS.isScalable())
    return std::nullopt;
  return TS.getFixedValue();
}

/// True when [\p Off, \p Off + \p AccessSize) lies entirely within the may_const
/// field that begins at \p Off. Guards the offset-matching paths, which would
/// otherwise let a widened load freeze the bytes of an adjacent mutable field.
inline bool ejitAccessFitsMayConstField(const GlobalVariable *RootGV,
                                        uint64_t Off, uint64_t AccessSize,
                                        const DataLayout &DL) {
  std::optional<uint64_t> FieldSize = ejitMayConstFieldSize(RootGV, Off, DL);
  return FieldSize && AccessSize != 0 && AccessSize <= *FieldSize;
}

//===----------------------------------------------------------------------===//
// Explicit, registration-time identity assignment for cross-module agreement.
//
// Neither funcIndex nor dimType can be derived independently per module: a
// modulo name hash collides (50 functions ~26%, 200 ~99% at 4096 slots; and
// fnv("cell")%8 == fnv("tenant")%8 for 8 dimType slots), and no AOT pass sees
// every final module. Both are therefore assigned ONCE, by name, in a
// process-global registry at registration time and read back by the wrapper
// through a per-function / per-lifecycle global the registration backfills:
//
//   * funcIndex: a dense index in [0, kEJitMaxFuncIndex) handed out by
//     EJitFuncRegistry. The wrapper loads @__ejit_funcidx_<name> (initialized
//     to kEJitInvalidFuncIndex) and falls back WITHOUT entering the taskpool
//     when it is still invalid (unregistered / capacity exhausted). The module
//     loader keys its table by the SAME registry index, so a distinct function
//     can never alias another's slot. See EJitFuncRegistry.h.
//   * dimType: a dense lifecycle slot in [0, kEJitMaxDimTypes) handed out by
//     EJitLifecycleRegistry, read back through @__ejit_dimtype_<name>. See
//     EJitLifecycleRegistry.h.
//
// Same name -> same index across every module and registration order; a new
// module never shifts an existing index; capacity exhaustion is a clean,
// propagated failure (ejit_init fails) — never a silent alias or hash collision
// on the correctness path.
//===----------------------------------------------------------------------===//

constexpr unsigned kEJitMaxDimTypes = 8;    // spec §5.1 MAX_DIM_TYPES
constexpr unsigned kEJitMaxInstances = 256; // spec §5.1 MAX_INSTANCES

// Flat-dedup capacity = max dense funcIndex (spec §3.5 inFlight_[]). Mirrors
// the runtime EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX; keep the two defaults in sync.
#ifndef EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX
#define EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX 4096u
#endif
constexpr uint32_t kEJitMaxFuncIndex = EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX;

/// Sentinel for "no dimType" (unknown / unregistered lifecycle). Out of
/// [0, kEJitMaxDimTypes), so it can never be a valid dimType.
constexpr uint32_t kEJitInvalidDimType = 0xFFFFFFFFu;

/// dimType reserved for every ejit_const_dim, in every function. It must be a
/// real in-range slot (the ABI rejects dimType >= kEJitMaxDimTypes), but it
/// never indexes a lifecycle: the enable gate always passes it, its version is
/// pinned at 0, and activate/deactivate refuse it. EJitLifecycleRegistry hands
/// out [0, kEJitConstDimType) so no lifecycle can ever land on it.
constexpr uint32_t kEJitConstDimType = kEJitMaxDimTypes - 1;

/// How many distinct lifecycles EJitLifecycleRegistry can assign: every dimType
/// slot except the one reserved above.
constexpr uint32_t kEJitMaxLifecycles = kEJitConstDimType;

/// Sentinel for "no funcIndex" (unregistered function or funcIndex capacity
/// exhausted). Out of [0, kEJitMaxFuncIndex); the wrapper treats it as
/// "fall back, never enter the taskpool".
constexpr uint32_t kEJitInvalidFuncIndex = 0xFFFFFFFFu;

} // namespace ejit
} // namespace llvm

#endif
