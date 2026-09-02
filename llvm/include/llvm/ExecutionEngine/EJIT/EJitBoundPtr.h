//===-- EJitBoundPtr.h - Non-owning bound pointer transport --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bound pointers are borrowed references to shared application objects. This
// header deliberately contains only fixed-layout, trivially-copyable types:
// no payload ownership, allocation, or destruction crosses the taskpool.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDPTR_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDPTR_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace llvm {
namespace ejit {

/// Maximum number of bound pointer parameters carried by one request.
constexpr uint32_t kEJitMaxBoundPointers = 8u;

/// Fixed-layout transport descriptor. The pointer is borrowed and is valid
/// only until the compile callback returns; it is never dereferenced by queue,
/// deduplication, retry, or publication code.
struct EJitBoundPtrDescriptor {
  const void *rawPtr;
  uint32_t size;
  uint32_t argIndex;
};

/// Non-owning view used only while the worker is compiling the request.
struct EJitBoundPointerView {
  const uint8_t *rawPtr = nullptr;
  uint32_t size = 0;
  uint32_t argIndex = 0;
  /// Specialization value of the bound period dimension, when known. This is
  /// internal compile metadata, not part of the queue/C ABI descriptor.
  uint32_t periodInstance = std::numeric_limits<uint32_t>::max();
};

static_assert(std::is_standard_layout<EJitBoundPtrDescriptor>::value,
              "bound pointer descriptor must have a stable C ABI layout");
static_assert(sizeof(EJitBoundPtrDescriptor) ==
                  (sizeof(uintptr_t) == 8 ? 16u : 12u),
              "bound pointer descriptor size must stay pointer-width fixed");
static_assert(offsetof(EJitBoundPtrDescriptor, rawPtr) == 0,
              "raw pointer must be the first descriptor field");
static_assert(offsetof(EJitBoundPtrDescriptor, size) == sizeof(uintptr_t),
              "bound size offset must follow the pointer");
static_assert(offsetof(EJitBoundPtrDescriptor, argIndex) ==
                  sizeof(uintptr_t) + sizeof(uint32_t),
              "bound arg index offset must remain ABI stable");
static_assert(std::is_trivially_copyable<EJitBoundPtrDescriptor>::value,
              "bound pointer descriptor must be copied by value");
static_assert(
    std::is_trivially_default_constructible<EJitBoundPtrDescriptor>::value,
    "bound pointer descriptor must have no initialization logic");
static_assert(std::is_trivially_destructible<EJitBoundPtrDescriptor>::value,
              "bound pointer descriptor must not own storage");

/// Validate descriptors at the ABI/taskpool boundary. A zero-count list is
/// the no-bound-pointer case. A present descriptor must identify a non-empty
/// object and its byte range must not wrap the target pointer width.
inline bool validateBoundPtrDescriptors(const EJitBoundPtrDescriptor *bounds,
                                        uint32_t count) {
  if (count == 0)
    return true;
  if (!bounds || count > kEJitMaxBoundPointers)
    return false;

  for (uint32_t i = 0; i < count; ++i) {
    const EJitBoundPtrDescriptor &B = bounds[i];
    if (!B.rawPtr || B.size == 0)
      return false;
    const uintptr_t Begin = reinterpret_cast<uintptr_t>(B.rawPtr);
    if (Begin > std::numeric_limits<uintptr_t>::max() - B.size)
      return false;
    for (uint32_t j = 0; j < i; ++j)
      if (bounds[j].argIndex == B.argIndex)
        return false;
  }
  return true;
}

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDPTR_H
