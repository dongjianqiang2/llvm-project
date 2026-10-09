//===-- EJitSmallTableSreBridgeInternal.h - owner-only bridge hooks --------===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGEINTERNAL_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGEINTERNAL_H
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifndef EJIT_FREESTANDING
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#endif
namespace llvm { namespace ejit {
class EJit;
class EJitSharedTaskPool;
class EJitSmallTableHost;
namespace detail { class OwnerWorkerContext; }
#ifndef EJIT_FREESTANDING
namespace detail {
/// Implementation-private Linux model of the existing static shared-section
/// deployment contract. It admits only an exact bounded range inventory plus
/// the actual bridge POD; never a whole process/heap mapping. There is no
/// freestanding setter or installed/public SDK hook. Join the worker and close
/// every physical lease before destroying the scope.
class ScopedSmallTableSreStaticDomainForTest {
public:
  explicit ScopedSmallTableSreStaticDomainForTest(
      ArrayRef<EJitWritableRange> Ranges);
  ~ScopedSmallTableSreStaticDomainForTest();
  ScopedSmallTableSreStaticDomainForTest(
      const ScopedSmallTableSreStaticDomainForTest &) = delete;
  ScopedSmallTableSreStaticDomainForTest &operator=(
      const ScopedSmallTableSreStaticDomainForTest &) = delete;
  bool valid() const { return active_; }
  bool addRange(uintptr_t Address, uint64_t Bytes);
private:
  bool active_ = false;
};
}
#endif
EJit *smallTableSreLocalRuntime();
// An ownership acquisition, not a raw observation. With no pool argument only
// the live facade is selected. Owner service may also acquire the deferred
// facade of this exact pool while real execution leases still pin it.
EJit *acquireSmallTableSreRuntime(EJitSharedTaskPool *ExpectedPool = nullptr);
EJit *acquireSmallTableSreRuntime(const detail::OwnerWorkerContext &Worker);
bool retainSmallTableSreRuntime(EJit *Runtime);
void releaseSmallTableSreRuntime(EJit *Runtime);
void releaseSmallTableSreRuntime(EJit *Runtime,
                                const detail::OwnerWorkerContext &Worker);
void smallTableSreWorkerEnter(EJitSharedTaskPool &Pool,
                             const detail::OwnerWorkerContext &Worker);
bool serviceSmallTableSreBridge(EJitSharedTaskPool &Pool,
                                const detail::OwnerWorkerContext &Worker);
void smallTableSreWorkerExit(EJitSharedTaskPool &Pool,
                            const detail::OwnerWorkerContext &Worker);
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
// Owner-private exact ticket completion, never a public bridge transaction.
// A refusal must leave its physical/runtime pin intact for the original owner.
bool smallTableSreLeaveHostTicket(const detail::OwnerWorkerContext &Worker,
                                 EJitSmallTableHost *ExpectedHost,
                                 uint64_t Ticket);
bool smallTableSreValidateOwnerCall(const detail::OwnerWorkerContext &Worker,
                                  uint32_t FuncIndex,
                                  const ejit_dim_pair_t *Dims, uint32_t NumDims,
                                  const ejit_bound_ptr_t *Bounds,
                                  uint32_t BoundCount, uint32_t *Versions,
                                  uint32_t &Generation, const char *&Why);
} }
#endif
