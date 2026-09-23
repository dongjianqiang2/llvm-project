// Diagnostic provenance only: never part of code identity or a reuse decision.
#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITFROZENVALUES_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITFROZENVALUES_H
#include "llvm/ADT/StringRef.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
namespace llvm {
namespace ejit {
inline constexpr const char *FrozenSiteMD = "ejit.reuse.frozen.site";
template <size_t N> bool frozenText(char (&Out)[N], StringRef Text) {
  size_t Size = std::min(Text.size(), N - 1);
  for (size_t I = 0; I < Size; ++I)
    Out[I] = Text[I] >= 32 && Text[I] < 127 ? Text[I] : ' ';
  Out[Size] = 0;
  return Size != Text.size();
}
struct EJitFrozenValue {
  uint64_t site = 0;
  char origin[64] = {}, value[48] = {};
  bool ambiguous = false, truncated = false;
};
struct EJitFrozenSnapshot {
  static constexpr unsigned Capacity = 32;
  EJitFrozenValue values[Capacity];
  unsigned count = 0, omitted = 0;
  bool captured = false;
  void record(uint64_t Site, StringRef Origin, StringRef Value) {
    if (!Site) { ++omitted; return; }
    for (unsigned I = 0; I < count; ++I)
      if (values[I].site == Site) {
        // Cloned/unrolled loads cannot be paired by encounter order.
        values[I].ambiguous = true;
        ++omitted;
        return;
      }
    if (count == Capacity) { ++omitted; return; }
    auto &V = values[count++];
    V.site = Site;
    V.truncated = frozenText(V.origin, Origin);
    V.truncated |= frozenText(V.value, Value);
  }
};
struct EJitFrozenDifference {
  uint64_t site = 0;
  char origin[64] = {}, peer[48] = {}, request[48] = {};
};
struct EJitFrozenComparison {
  static constexpr unsigned Capacity = 4;
  EJitFrozenDifference differences[Capacity];
  unsigned shown = 0, different = 0, compared = 0;
  bool available = false, incomplete = false;
};
inline EJitFrozenComparison compareFrozen(const EJitFrozenSnapshot &A,
                                         const EJitFrozenSnapshot &B) {
  EJitFrozenComparison R;
  R.available = A.captured && B.captured;
  if (!R.available) return R;
  R.incomplete = A.omitted || B.omitted;
  for (unsigned I = 0; I < A.count; ++I) {
    const auto &L = A.values[I];
    const EJitFrozenValue *Match = nullptr;
    for (unsigned J = 0; J < B.count; ++J)
      if (B.values[J].site == L.site) Match = &B.values[J];
    if (!Match || L.ambiguous || Match->ambiguous ||
        L.truncated || Match->truncated ||
        std::strcmp(L.origin, Match->origin)) {
      R.incomplete = true;
      continue;
    }
    ++R.compared;
    if (!std::strcmp(L.value, Match->value)) continue;
    ++R.different;
    if (R.shown == R.Capacity) continue;
    auto &D = R.differences[R.shown++];
    D.site = L.site;
    frozenText(D.origin, L.origin);
    frozenText(D.peer, L.value);
    frozenText(D.request, Match->value);
  }
  if (R.compared != A.count || R.compared != B.count) R.incomplete = true;
  return R;
}
} // namespace ejit
} // namespace llvm
#endif
