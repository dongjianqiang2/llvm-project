#===-- EmbeddedJIT target triple configuration ----------------------------===#
# When cross-compiling for ARM embedded, set this to the target triple
# so the JIT engine targets the correct architecture at runtime.
# Default: detect host (sufficient for native builds).
set(EJIT_DEFAULT_TARGET_TRIPLE "" CACHE STRING
  "Default target triple for the EmbeddedJIT engine (empty = detect host)")

if(EJIT_DEFAULT_TARGET_TRIPLE)
  message(STATUS "EmbeddedJIT: forcing target triple = ${EJIT_DEFAULT_TARGET_TRIPLE}")
endif()

#===-- Freestanding configuration -----------------------------------------===#
# EJIT_FREESTANDING option is defined in top-level llvm/CMakeLists.txt.
if(EJIT_FREESTANDING)
  message(STATUS "EmbeddedJIT: freestanding mode enabled")
  if(NOT EJIT_DEFAULT_TARGET_TRIPLE)
    message(FATAL_ERROR "EJIT_FREESTANDING requires EJIT_DEFAULT_TARGET_TRIPLE "
            "(e.g. -DEJIT_DEFAULT_TARGET_TRIPLE=aarch64-none-elf)")
  endif()
endif()

option(EJIT_FREESTANDING_MUTEX
  "Inject no-op <mutex>/<shared_mutex>/<condition_variable> headers \
(for freestanding toolchains that lack these C++ standard headers). \
Enable this only when the toolchain does NOT provide <mutex>." OFF)
if(EJIT_FREESTANDING_MUTEX)
  set(BAREMETAL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/BareMetal")
  message(STATUS "EmbeddedJIT: injecting bare-metal mutex stubs from ${BAREMETAL_INCLUDE}")
  # Inject into LLVM libraries that use std::mutex (OrcJIT, Support, JITLink, etc.)
  # but NOT into clang (which uses std::thread and needs the real <mutex>).
  # Targets are created before this CMakeLists.txt runs, so they exist.
  foreach(_tgt LLVMOrcJIT LLVMJITLink LLVMSupport LLVMCodeGen LLVMCore
              LLVMAnalysis LLVMTransformUtils LLVMInstCombine LLVMScalarOpts
              LLVMipo LLVMObject LLVMMC LLVMMCParser LLVMTarget
              LLVMSelectionDAG LLVMGlobalISel LLVMAsmPrinter LLVMPasses
              LLVMExecutionEngine LLVMOrcShared LLVMRuntimeDyld LLVMOrcTargetProcess
              # Target-specific libs (only exist when built for that arch)
              LLVMX86CodeGen LLVMX86Desc LLVMX86Info
              LLVMAArch64CodeGen LLVMAArch64Desc LLVMAArch64Info LLVMAArch64Utils)
    if(TARGET ${_tgt})
      target_include_directories(${_tgt} SYSTEM BEFORE PRIVATE ${BAREMETAL_INCLUDE})
    endif()
  endforeach()
endif()

# Build target component list from LLVM_TARGETS_TO_BUILD.
# Each target needs CodeGen, Desc, and Info for native target initialization.
# AsmPrinter init lives in CodeGen, so no separate AsmPrinter component needed.
set(EJIT_TARGET_COMPONENTS "")
foreach(_t ${LLVM_TARGETS_TO_BUILD})
  list(APPEND EJIT_TARGET_COMPONENTS ${_t}CodeGen ${_t}Desc ${_t}Info)
  if(_t STREQUAL "AArch64")
    list(APPEND EJIT_TARGET_COMPONENTS AArch64AsmParser)
  endif()
endforeach()

set(EJIT_SOURCES
  EJit.cpp
  EJitRuntime.cpp
  EJitRegistryTable.c
  EJitOrcEngine.cpp
  EJitLinkDiagPlugin.cpp
  EJitLinkOptimizationPlugin.cpp
  EJitLibcallStubs.cpp
  EJitCompileDriver.cpp
  EJitRuntimeState.cpp
  EJitModuleLoader.cpp
  EJitRegistrationStore.cpp
  EJitStructFieldPass.cpp
  EJitOptimizer.cpp
  EJitPassBuilder.cpp
  EJitProfileMerge.cpp
  EJitCodePool.cpp
  EJitCodePoolMemoryManager.cpp
  EJitSrePlatform.cpp
)

# EJitLogger has its own #ifndef EJIT_FREESTANDING no-op stubs, so always include it.
list(APPEND EJIT_SOURCES
  EJitLogger.cpp
)

# SRE taskpool compile scheduler — always compiled (PASS3 unconditionally
# generates ejit_taskpool_compile_or_get calls). It uses no C++ threading
# facilities, so it is compatible with EJIT_FREESTANDING.
list(APPEND EJIT_SOURCES
  EJitTaskPool.cpp
  EJitSreQueue.cpp
  EJitWorker.cpp
)
if(EJIT_FREESTANDING)
  list(APPEND EJIT_SOURCES EJitSreTask_sre.cpp)
else()
  list(APPEND EJIT_SOURCES EJitSreTask_host.cpp)
endif()
# Cross-core shared taskpool (single shared worker owner) — optional.
# POD shared state + injectable core-id seam; no C++ threading facilities, so
# it is compatible with EJIT_FREESTANDING.
if(EJIT_SRE_SHARED_TASKPOOL)
  list(APPEND EJIT_SOURCES
    EJitSharedTaskPool.cpp
    EJitSharedPlatform.cpp
  )
endif()

# Async/sync compiler — excluded in freestanding mode (single-threaded, no std::thread)
if(NOT EJIT_FREESTANDING)
  list(APPEND EJIT_SOURCES
  )
endif()

add_llvm_component_library(LLVMEJIT
  ${EJIT_SOURCES}

  PARTIAL_SOURCES_INTENDED

  LINK_COMPONENTS
  BitReader
  Core
  OrcJIT
  JITLink
  Support
  InstCombine
  TransformUtils
  Analysis
  ${EJIT_TARGET_COMPONENTS}
  # PGO (EJIT_ONLINE_PGO.md): Instrumentation (PGOInstrumentationGen/Use +
  # InstrProfilingLowering) and ProfileData (InstrProfWriter) are required for
  # online PGO. This is the P0-1 footprint cost (~640 KB stripped after lipo
  # --gc-sections). PGO behavior is opt-in (Config::enablePgo); the libs are
  # linked whenever LLVMEJIT is, regardless of the flag.
  Instrumentation
  ProfileData
  # IPO and Scalar are not listed here — their LINK_COMPONENTS would
  # recursively pull in BitWriter, Instrumentation, Linker, AsmParser,
  # and AggressiveInstCombine.  Instead, they are added manually to the
  # ejit_minimal target below (see §6.5 of EJIT_LIBRARY_TRIMMING.md).
  # ExecutionEngine is not listed — it's already pulled by OrcJIT.
)

if(EJIT_SRE_TASKPOOL_NO_RECLAIM)
  target_compile_definitions(LLVMEJIT PRIVATE
    EJIT_SRE_TASKPOOL_NO_RECLAIM)
  message(STATUS
    "EmbeddedJIT: NO_RECLAIM shared-cache hot path enabled (JIT code must never be physically reclaimed)")
endif()

# Propagate the target triple setting to the C++ code
if(EJIT_DEFAULT_TARGET_TRIPLE)
  target_compile_definitions(LLVMEJIT PUBLIC
    EJIT_DEFAULT_TRIPLE="${EJIT_DEFAULT_TARGET_TRIPLE}")
endif()

# 4K-granular code-pool sealing. The only user of this macro is the SRE adapter
# (EJitSrePlatform.cpp), so scope it to LLVMEJIT instead of a global
# add_definitions — toggling it then does not recompile the whole LLVM tree.
# Effective only when the SRE code pool itself is enabled.
if(EJIT_SRE_CODE_POOL AND EJIT_CODE_POOL_4K_SEAL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_CODE_POOL_4K_SEAL)
endif()

# Pin the JIT code pool to a fixed linker-script region [__ejit_code_start,
# __ejit_code_end) in the RX code segment (enable_rw write / enable_ex seal)
# instead of dynamic SRE_MemDbgAlloc. The only user of this macro is the SRE
# adapter (EJitSrePlatform.cpp), so scope it to LLVMEJIT like
# EJIT_CODE_POOL_4K_SEAL above. Effective only when the SRE code pool is on.
if(EJIT_SRE_CODE_POOL AND EJIT_FIXED_CODE_POOL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_FIXED_CODE_POOL)
endif()

# EJIT_ICACHE_DIM_SIZE: per-dim bound D of the multi-version inline cache.
# Used by icacheFill/icacheTry linearization in EJitSharedTaskPool. Must match
# the AOT -mllvm -ejit-icache-dim-size value (both from the top-level CMake
# var). Scoped to LLVMEJIT (the icache runtime lives here); gated on the shared
# taskpool, which is the only build the icache is compiled under.
if(EJIT_SRE_SHARED_TASKPOOL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE})
endif()

# EJIT_FREESTANDING must be visible in LLVMJITLink headers so that
# std::promise / std::future usage is guarded (bare-metal has no <future>).
if(EJIT_FREESTANDING)
  target_compile_definitions(LLVMJITLink PUBLIC EJIT_FREESTANDING)
endif()

#===-- Build-version header (ejit_print_version) -------------------------===#
# Generate EJitVersion.h carrying the llvm-project git commit + branch of the
# source tree the runtime was built from. The LLVM release version itself is
# read at runtime from llvm/Config/llvm-config.h (LLVM_VERSION_STRING), so it
# is not duplicated here.
#
# The header is regenerated on every build by a custom target, but the
# generator script writes it with copy_if_different: the file's mtime (and
# therefore EJitRuntime.cpp, the only includer) only changes when the commit
# actually changes. This keeps ejit_print_version() fresh across rebuilds
# (a rebuild after `git commit` picks up the new hash) without forcing a full
# CMake reconfigure, and without recompiling on every no-op build.
find_package(Git QUIET)
set(EJIT_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/EJitVersion.h")
add_custom_target(ejit_version_header
  COMMAND ${CMAKE_COMMAND}
    -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
    -DSOURCE_DIR=${LLVM_MAIN_SRC_DIR}
    -DOUT_FILE=${EJIT_VERSION_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateEJitVersion.cmake"
  BYPRODUCTS "${EJIT_VERSION_HEADER}"
  COMMENT "Refreshing EJIT build-version header (git commit/branch)"
  VERBATIM)
add_dependencies(LLVMEJIT ejit_version_header)
# EJitRuntime.cpp #includes "EJitVersion.h"; put the build dir on the path.
target_include_directories(LLVMEJIT PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

#===-- Minimal embedded runtime target -----------------------------------===#
# Builds a single combined static library (libejit_minimal.a) that bundles
# EJIT + all transitive LLVM dependencies. Designed for ARM embedded targets
# with a ~10MB RAM budget.
#
# Usage:
#   cmake --preset ejit-minimal
#   ninja LLVMEJIT              # build LLVMEJIT + its dependency libraries
#   ninja ejit_minimal           # combine into fat archive
#   ninja check-ejit-size        # verify size fits in budget

# Resolve LLVM component names → library names (e.g. "Core" → "LLVMCore")
llvm_map_components_to_libnames(EJIT_LLVM_LIBS
  BitReader Core OrcJIT JITLink Support
  InstCombine TransformUtils Analysis
  ${EJIT_TARGET_COMPONENTS}
  )
# IPO and Scalar are added manually to avoid their LINK_COMPONENTS
# (BitWriter, Instrumentation, Linker, AsmParser, AggressiveInstCombine).
# See EJIT_LIBRARY_TRIMMING.md §6.5.
list(APPEND EJIT_LLVM_LIBS LLVMipo LLVMScalarOpts)
# PGO (EJIT_ONLINE_PGO.md): LLVMInstrumentation + LLVMProfileData are NOT added
# to this fat archive yet. The fat archive is pre-gc and bundling the full
# Instrumentation.a (all sanitizers) would blow the 10 MB check-ejit-size gate.
# The deployable is the lipo ld -r --gc-sections ejit.o (~640 KB PGO increment,
# P0-1), which keeps only reachable PGO objects. The lipo pipeline's COMMON_LIBS
# list (ejit_test/lipo) + moving check-ejit-size to the lipo output are wired in
# the deployable-integration task. LLVMEJIT LINK_COMPONENTS (above) already
# pulls these for test-binary linking.

# Build a file listing all .a files to combine (one per line)
# LLVM_LIBRARY_DIR is the build output directory for static libraries
set(EJIT_LIBS_LIST "${CMAKE_CURRENT_BINARY_DIR}/ejit_libs.txt")
set(EJIT_MINIMAL_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libejit_minimal.a")

# Generate the library path list at configure time
# LLVMEJIT is special: its .a name includes the component name
set(_ejit_lib_paths "${LLVM_LIBRARY_DIR}/libLLVMEJIT.a")
foreach(_lib ${EJIT_LLVM_LIBS})
  list(APPEND _ejit_lib_paths "${LLVM_LIBRARY_DIR}/lib${_lib}.a")
endforeach()
string(REPLACE ";" "\n" _ejit_lib_paths_str "${_ejit_lib_paths}")
file(WRITE "${EJIT_LIBS_LIST}" "${_ejit_lib_paths_str}\n")

# Combine all libraries into one fat archive
add_custom_target(ejit_minimal
  COMMAND ${CMAKE_COMMAND}
    -DLIB_LIST_FILE="${EJIT_LIBS_LIST}"
    -DOUTPUT="${EJIT_MINIMAL_OUTPUT}"
    -DCMAKE_AR="${CMAKE_AR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CombineArchives.cmake"
  DEPENDS LLVMEJIT ${EJIT_LLVM_LIBS}
  COMMENT "Combining EJIT + LLVM dependencies into libejit_minimal.a"
  )

# Size verification target — checks the combined archive fits within budget
add_custom_target(check-ejit-size
  COMMAND ${CMAKE_COMMAND}
    -DARCHIVE_FILE="${EJIT_MINIMAL_OUTPUT}"
    -DMAX_SIZE_MB=10
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckSize.cmake"
  DEPENDS ejit_minimal
  )

#===-- Bare-metal stub library ---------------------------------------------===#
# libejit_baremetal.a — no-op stubs for POSIX functions that EJIT+LLVM
# link against but never call at runtime on bare-metal (pthread, dl, mmap,
# file I/O, signals, etc.).  See jit_design_doc/EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL_STUBS.md.
add_library(ejit_baremetal STATIC
  BareMetal/ejit_baremetal_stubs.c)
target_compile_options(ejit_baremetal PRIVATE -ffunction-sections -fdata-sections)
target_include_directories(ejit_baremetal PUBLIC BareMetal)
