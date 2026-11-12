//===-- EJitAllocationBounds.h - Allocation size lookup -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITALLOCATIONBOUNDS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITALLOCATIONBOUNDS_H

#include <cstdint>

namespace llvm::ejit {

/// Platform allocation-size lookup, fixed before compiler workers start.
struct AllocationSizeLookup {
  using Callback = uint64_t (*)(const void *Base, void *Context);
  Callback callback = nullptr;
  void *context = nullptr;
};

} // namespace llvm::ejit

#endif
