//===-- EJitBootstrapPool.cpp - Worker-only initialization -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitBootstrapPool.h"
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreTask.h"
#include <cstring>
#include <new>

extern "C" {
#ifndef EJIT_FREESTANDING
extern const ejit_reg_entry_t __start_ejit_group[] __attribute__((weak));
extern const ejit_reg_entry_t __stop_ejit_group[] __attribute__((weak));
#else
extern const ejit_reg_entry_t __start_ejit_group[];
extern const ejit_reg_entry_t __stop_ejit_group[];
#endif
}

namespace llvm::ejit {
namespace {
enum : uint32_t { Empty, Configuring, Configured };
// Per-core .bss on SRE. The object is constructed in place on its first use,
// with no .init_array/.fini_array entry or C++ local-static guard.
alignas(EJitSharedTaskPool) unsigned char PoolStorage[sizeof(EJitSharedTaskPool)];
EJitAtomicU32 BootstrapState;
} // namespace

EJitSharedTaskPool *getEJitBootstrapPool(bool RequireReady) {
  auto &State = gEJitSharedTaskPoolState;
  if (RequireReady &&
      (State.initState.loadAcquire() !=
           static_cast<uint32_t>(EJitSharedInitState::Ready) ||
       State.magic != kEJitSharedAbiMagic ||
       State.abiVersion != kEJitSharedAbiVersion ||
       State.structSize != sizeof(State)))
    return nullptr;

  uint32_t Phase = BootstrapState.loadAcquire();
  if (Phase == Configured)
    return std::launder(reinterpret_cast<EJitSharedTaskPool *>(PoolStorage));
  if (!RequireReady || Phase != Empty ||
      EJitCoreId::current() == EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE ||
      !BootstrapState.compareExchange(Phase, Configuring))
    return nullptr;

  auto *Pool = ::new (PoolStorage) EJitSharedTaskPool();
  Pool->setSplitPoolCallback(
      [](void *, uintptr_t Base, uint64_t Size) {
        return ejitSreSplitPoolForCurrentCore(Base, Size);
      },
      nullptr);
  Pool->setSealPageCallback(
      [](void *, uintptr_t Page) { return ejitSreSealPageForCurrentCore(Page); },
      nullptr);
  Pool->setEnableRwPageCallback(
      [](void *, uintptr_t Page) {
        return ejitSreEnableRwPageForCurrentCore(Page);
      },
      nullptr);
  Pool->setSealMode(true);
  Pool->setCodeSharingEnabled(true);
  Pool->setWorkerIdleHook(
      [](void *, uint32_t Ticks) { EJitSreTask::delay(Ticks); }, nullptr);
  Pool->bind(&State);
  // A reentrant producer must fall back until all permission hooks are wired.
  BootstrapState.storeRelease(Configured);
  return Pool;
}

uint32_t lookupEJitBootstrapDim(const char *Name) {
  if (!Name)
    return kEJitInvalidDimType;
#ifndef EJIT_FREESTANDING
  if (!__start_ejit_group || !__stop_ejit_group)
    return kEJitInvalidDimType;
#endif
  for (auto *Entry = __start_ejit_group; Entry != __stop_ejit_group; ++Entry)
    if (Entry->type == EJIT_REG_LIFECYCLE && Entry->name1 && Entry->ptr &&
        std::strcmp(Entry->name1, Name) == 0) {
      auto Slot = __atomic_load_n(static_cast<const uint32_t *>(Entry->ptr),
                                  __ATOMIC_ACQUIRE);
      return Slot < kEJitSharedDimTypes ? Slot : kEJitInvalidDimType;
    }
  return kEJitInvalidDimType;
}
} // namespace llvm::ejit
#endif
