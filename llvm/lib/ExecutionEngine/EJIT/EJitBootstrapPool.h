//===-- EJitBootstrapPool.h - Worker-only initialization --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITBOOTSTRAPPOOL_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITBOOTSTRAPPOOL_H

#include <cstdint>

namespace llvm::ejit {
class EJitSharedTaskPool;
struct EJitSharedTaskPoolState;

extern EJitSharedTaskPoolState gEJitSharedTaskPoolState;

// Nonblocking and producer-only. Existing read tokens can be released while
// the pool stops, without constructing a new facade.
EJitSharedTaskPool *getEJitBootstrapPool(bool RequireReady = true);
uint32_t lookupEJitBootstrapDim(const char *Name);
} // namespace llvm::ejit

#endif
