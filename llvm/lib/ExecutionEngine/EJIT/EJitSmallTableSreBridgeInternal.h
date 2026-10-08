//===-- EJitSmallTableSreBridgeInternal.h - owner-only bridge hooks --------===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGEINTERNAL_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGEINTERNAL_H
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
namespace llvm { namespace ejit {
class EJit;
class EJitSharedTaskPool;
EJit *smallTableSreLocalRuntime();
// An ownership acquisition, not a raw observation. With no pool argument only
// the live facade is selected. Owner service may also acquire the deferred
// facade of this exact pool while real execution leases still pin it.
EJit *acquireSmallTableSreRuntime(EJitSharedTaskPool *ExpectedPool = nullptr);
bool retainSmallTableSreRuntime(EJit *Runtime);
void releaseSmallTableSreRuntime(EJit *Runtime);
void smallTableSreWorkerEnter(EJitSharedTaskPool &Pool);
bool serviceSmallTableSreBridge(EJitSharedTaskPool &Pool);
void smallTableSreWorkerExit(EJitSharedTaskPool &Pool);
void smallTableSrePolicyChanged();
uint64_t smallTableSreWrapperEpoch(uint64_t LocalEpoch);
bool smallTableSreNoPolicyCurrent(uint64_t Epoch, uint64_t LocalEpoch);
bool smallTableSreOwnsFunction(uint32_t FuncIndex);
bool smallTableSreWrapperEnter(uint32_t FuncIndex, const ejit_dim_pair_t *Dims,
                             uint32_t NumDims, const ejit_bound_ptr_t *Bounds,
                             uint32_t BoundCount, uint64_t *OutTicket,
                             const char **OutWhy, uint64_t *OutEpoch,
                             void *&Entry);
bool smallTableSreLeave(uint64_t Ticket);
bool smallTableSreValidateOwnerCall(uint32_t FuncIndex,
                                  const ejit_dim_pair_t *Dims, uint32_t NumDims,
                                  const ejit_bound_ptr_t *Bounds,
                                  uint32_t BoundCount, uint32_t *Versions,
                                  uint32_t &Generation, const char *&Why);
} }
#endif
