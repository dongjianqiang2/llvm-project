//===-- EJitGlobalLayout.h - Complete final layout configuration -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITGLOBALLAYOUT_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITGLOBALLAYOUT_H

#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include <cstdint>
#include <string>

namespace llvm::ejit {
/// Each element's array position is its authoritative physical-body rank.
/// Dimensions/bounds are exact. Version policy0 captures live admission
/// versions; policy1 asserts an explicit active snapshot (see
/// EJitCompileRequest).
struct EJitLayoutTarget {
  std::string entryName;
  EJitCompileRequest identity{};
};

struct EJitLayoutBudgets {
  uint32_t maxMembers = 1024;
  uint64_t maxArtifactBytes = 64ull * 1024 * 1024;
  uint64_t maxProfileBytes = 32ull * 1024 * 1024;
  uint64_t maxNearCodeBytes = 16ull * 1024 * 1024;
  uint64_t maxFarCodeBytes = 16ull * 1024 * 1024;
  uint32_t maxCollectionPolls = 1u << 20;
};

enum class EJitGlobalLayoutState : uint32_t {
  Disabled,
  Collecting,
  Linking,
  Linked,
  Sealing,
  Publishing,
  Complete,
  Failed,
  Cancelled,
  Refreshing
};

enum class EJitGlobalLayoutError : uint32_t {
  None,
  Manifest,
  MissingTarget,
  StaleIdentity,
  ArtifactBudget,
  Preparation,
  Linking,
  PhysicalOrder,
  Range,
  Seal,
  Publish,
  Cancelled
};
} // namespace llvm::ejit
#endif
