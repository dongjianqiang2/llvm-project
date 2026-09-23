//===-- EJitSreHostPlatformTest.cpp - host SRE platform adapters ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Host implementations used only by EJIT unit-test executables configured
// with the SRE code pool. They preserve the production pool alignment and
// writable-to-executable transition without supplying any board ABI.

#if defined(EJIT_SRE_CODE_POOL)

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char,
                                 unsigned long Size, const char *,
                                 unsigned int) {
  return ::VirtualAlloc(nullptr, Size, MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
}

extern "C" unsigned split_2m_to_4k(unsigned long long,
                                   unsigned long long) {
  return 0;
}

extern "C" unsigned enable_ex(unsigned, unsigned long long Va) {
  DWORD OldProtect = 0;
  void *Page = reinterpret_cast<void *>(static_cast<uintptr_t>(Va));
  if (!::VirtualProtect(Page, 4096, PAGE_EXECUTE_READ, &OldProtect))
    return static_cast<unsigned>(::GetLastError());
  return ::FlushInstructionCache(::GetCurrentProcess(), Page, 4096) ? 0u : 1u;
}

#else

#include <sys/mman.h>

extern "C" void *SRE_MemDbgAlloc(unsigned int, unsigned char,
                                 unsigned long Size, const char *,
                                 unsigned int) {
  constexpr std::size_t PoolAlignment = 2u * 1024u * 1024u;
  const std::size_t MappingSize = static_cast<std::size_t>(Size) + PoolAlignment;
  void *Mapping = ::mmap(nullptr, MappingSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Mapping == MAP_FAILED)
    return nullptr;

  const uintptr_t MappingAddr = reinterpret_cast<uintptr_t>(Mapping);
  const uintptr_t AlignedAddr =
      (MappingAddr + PoolAlignment - 1u) & ~(PoolAlignment - 1u);
  const std::size_t Prefix = AlignedAddr - MappingAddr;
  const std::size_t Suffix = MappingSize - Prefix - Size;
  if (Prefix != 0)
    ::munmap(Mapping, Prefix);
  if (Suffix != 0)
    ::munmap(reinterpret_cast<void *>(AlignedAddr + Size), Suffix);
  return reinterpret_cast<void *>(AlignedAddr);
}

extern "C" unsigned split_2m_to_4k(unsigned long long,
                                   unsigned long long) {
  return 0;
}

extern "C" unsigned enable_ex(unsigned, unsigned long long Va) {
  constexpr std::size_t PageSize = 4096;
  auto *Page = reinterpret_cast<char *>(static_cast<uintptr_t>(Va));
  if (::mprotect(Page, PageSize, PROT_READ | PROT_EXEC) != 0)
    return 1;
  __builtin___clear_cache(Page, Page + PageSize);
  return 0;
}

#endif
#endif
