//===-- EJitFinalReplay.h - Owned T2 replay inputs --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITFINALREPLAY_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITFINALREPLAY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Constant;
class Function;
class LoadInst;
namespace ejit {
struct EJitFinalReplay {
  struct Fold {
    uint32_t frame, ordinal;
    uint64_t bits;
    unsigned typeID, width;
  };
  struct BoundFact {
    uint32_t arg, size;
  };
  struct Replacement {
    LoadInst *LI;
    Constant *ConstVal;
  };
  std::vector<Fold> folds;
  std::vector<BoundFact> bounds;
  std::string profile;
  std::vector<PgoScalarSite> scalars;
  std::array<uint8_t, 32> sourceHash{}, outputHash{};
  uint32_t frames = 0;
  bool usable = true;
  static std::array<uint8_t, 32> digest(StringRef Bytes);
  size_t bytes() const {
    return sizeof(*this) + folds.capacity() * sizeof(Fold) +
           bounds.capacity() * sizeof(BoundFact) + profile.capacity() +
           scalars.capacity() * sizeof(PgoScalarSite);
  }
  void record(Function &F, ArrayRef<Replacement> Roots,
              ArrayRef<Replacement> Fields);
};

// Per-rebuild cursor; a recipe is immutable once admitted.
struct EJitFinalReplayCursor {
  const EJitFinalReplay &recipe;
  uint32_t frame = 0;
  size_t next = 0;
  bool valid = true;
  bool apply(Function &F);
  bool complete() const {
    return valid && frame == recipe.frames && next == recipe.folds.size();
  }
};
} // namespace ejit
} // namespace llvm
#endif
