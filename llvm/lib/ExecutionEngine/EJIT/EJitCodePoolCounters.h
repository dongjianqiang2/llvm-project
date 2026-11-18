//===-- EJitCodePoolCounters.h - Code pool diagnostic counters --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_EXECUTIONENGINE_EJIT_EJITCODEPOOLCOUNTERS_H
#define LLVM_LIB_EXECUTIONENGINE_EJIT_EJITCODEPOOLCOUNTERS_H

namespace llvm::ejit {

// The nine counters common to the manager, the shared mirror and the C API.
// rwEnableInvocations is manager-only and must not enter the shared ABI.
template <class Op, class... Records>
inline void forEachPoolCounter(Op Apply, Records &...R) {
  Apply(R.poolCount...);
  Apply(R.sealedCount...);
  Apply(R.activeCount...);
  Apply(R.usedBytes...);
  Apply(R.reservedBytes...);
  Apply(R.wastedBytes...);
  Apply(R.sealInvocations...);
  Apply(R.splitInvocations...);
  Apply(R.finalizedRangeCount...);
}

template <class Dst, class Src>
inline void copyPoolCounters(Dst &D, const Src &S) {
  forEachPoolCounter([](auto &To, auto From) { To = From; }, D, S);
}

template <class Total, class Detail>
inline void sumPoolCounters(Total &T, const Detail &Near, const Detail &Far) {
  forEachPoolCounter([](auto &Sum, auto N, auto F) { Sum = N + F; }, T, Near,
                     Far);
}

} // namespace llvm::ejit

#endif // LLVM_LIB_EXECUTIONENGINE_EJIT_EJITCODEPOOLCOUNTERS_H
