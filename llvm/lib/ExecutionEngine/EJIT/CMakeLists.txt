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

#===-- Online-PGO value profiling (guarded specialization) ----------------===#
# Runtime value collector + Tier-1 instrumentation hooks + Tier-2 guarded
# scalar specialization (EJIT_VALUE_PROFILE.md). DEFAULT OFF: with the option
# off, no collector/pass source is compiled, the shared-memory ABI is
# unchanged, and every other build is byte-for-byte identical. The definitions
# are applied narrowly to LLVMEJIT (their only user) rather than globally, so
# toggling it does not recompile the whole tree.
option(EJIT_SRE_PGO_VALUE_PROFILE
  "EmbeddedJIT online-PGO value profiling (per-core heavy-hitter collector + guarded Tier-2 specialization)" OFF)
set(EJIT_SRE_VP_K "2" CACHE STRING
  "Value-profiling heavy-hitter candidates per site (1-8)")
set(EJIT_SRE_VP_SITES_PER_CORE "64" CACHE STRING
  "Value-profiling fixed sites per core shard (power of two)")
set(EJIT_SRE_VP_MAX_CORES "32" CACHE STRING
  "Value-profiling per-core shard slots (1-256)")
set(EJIT_SRE_VP_TOP_N "2" CACHE STRING
  "Value-profiling top values carried per site (1-K)")
set(EJIT_SRE_VP_DRAIN_TICKS "4096" CACHE STRING
  "Value-profiling snapshot straggler-drain iterations")
set(EJIT_SRE_VP_MIN_SAMPLES "100" CACHE STRING
  "Scalar specialization minimum samples per site")
set(EJIT_SRE_VP_MIN_CONF_PERCENT "99" CACHE STRING
  "Scalar specialization minimum top-1 dominance percent")
if(EJIT_SRE_PGO_VALUE_PROFILE)
  message(STATUS "EmbeddedJIT: online-PGO value profiling enabled "
                 "(K=${EJIT_SRE_VP_K}, sites/core=${EJIT_SRE_VP_SITES_PER_CORE}, "
                 "cores=${EJIT_SRE_VP_MAX_CORES})")
endif()

option(EJIT_SRE_PGO_BRANCH_AUDIT
  "Report per-function branch-profile shape before Tier-2 inlining" OFF)
if(EJIT_SRE_PGO_BRANCH_AUDIT)
  message(STATUS "EmbeddedJIT: online-PGO branch audit enabled")
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
# AsmParser is needed for AArch64 (EJit.cpp's trimmed AArch64 path calls it)
# and for X86 (the untrimmed fallback calls InitializeAllAsmParsers(), which
# references every built target's AsmParser - without the component, any host
# binary linking LLVMEJIT on an X86 build fails to link).
set(EJIT_TARGET_COMPONENTS "")
foreach(_t ${LLVM_TARGETS_TO_BUILD})
  list(APPEND EJIT_TARGET_COMPONENTS ${_t}CodeGen ${_t}Desc ${_t}Info)
  if(_t STREQUAL "AArch64")
    list(APPEND EJIT_TARGET_COMPONENTS AArch64AsmParser)
  endif()
  # initializeEJitTargets() falls through to InitializeAllAsmParsers() on
  # hosted builds, so standalone links of LLVMEJIT (EJITTests) need the
  # native AsmParser component too; linking into clang/lld hides the gap.
  if(_t STREQUAL "X86")
    list(APPEND EJIT_TARGET_COMPONENTS X86AsmParser)
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
  EJitVerify.cpp
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

# Online-PGO value profiling (default OFF): per-core collector + the runtime
# hooks the Tier-1 instrumentation calls, plus the scalar/loop-bound discovery
# and guarded-specialization passes. No C++ threading facilities (the hot path
# is atomic-cell-only), so it is compatible with EJIT_FREESTANDING. The compile
# definitions are applied after the LLVMEJIT target exists (see the option
# block below).
if(EJIT_SRE_PGO_VALUE_PROFILE)
  list(APPEND EJIT_SOURCES
    EJitVpCollector.cpp
    EJitValueProfile.cpp
  )
endif()

if(EJIT_SRE_PGO_BRANCH_AUDIT)
  list(APPEND EJIT_SOURCES EJitBranchProfile.cpp)
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

# may_const substitution verifier (option declared in llvm/CMakeLists.txt). The
# users of this macro are the JIT-side pass, the runtime helper, and the libcall
# table — all inside LLVMEJIT — so scope it to the target instead of a global
# add_definitions, like EJIT_CODE_POOL_4K_SEAL above.
if(EJIT_VERIFY_SUBSTITUTION)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_VERIFY_SUBSTITUTION)
  message(STATUS
    "EmbeddedJIT: may_const substitution verifier built in (diagnostic; "
    "enable per run with ejit_init_verify)")
endif()

# Propagate the target triple setting to the C++ code
if(EJIT_DEFAULT_TARGET_TRIPLE)
  target_compile_definitions(LLVMEJIT PUBLIC
    EJIT_DEFAULT_TRIPLE="${EJIT_DEFAULT_TARGET_TRIPLE}")
endif()

#===-- Online-PGO value profiling definitions (after the LLVMEJIT target
#     exists) -------------------------------------------------------------===#
# Online-PGO value profiling: definitions scoped to LLVMEJIT (the only user),
# so toggling the option never recompiles the rest of the tree.
if(EJIT_SRE_PGO_VALUE_PROFILE)
  target_compile_definitions(LLVMEJIT PRIVATE
    EJIT_SRE_PGO_VALUE_PROFILE
    EJIT_SRE_VP_K=${EJIT_SRE_VP_K}
    EJIT_SRE_VP_SITES_PER_CORE=${EJIT_SRE_VP_SITES_PER_CORE}
    EJIT_SRE_VP_MAX_CORES=${EJIT_SRE_VP_MAX_CORES}
    EJIT_SRE_VP_TOP_N=${EJIT_SRE_VP_TOP_N}
    EJIT_SRE_VP_DRAIN_TICKS=${EJIT_SRE_VP_DRAIN_TICKS}
    EJIT_SRE_VP_MIN_SAMPLES=${EJIT_SRE_VP_MIN_SAMPLES}
    EJIT_SRE_VP_MIN_CONF_PERCENT=${EJIT_SRE_VP_MIN_CONF_PERCENT}
  )
endif()

if(EJIT_SRE_PGO_BRANCH_AUDIT)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_SRE_PGO_BRANCH_AUDIT)
endif()

# 4K-granular code-pool sealing. The only user of this macro is the SRE adapter
# (EJitSrePlatform.cpp), so scope it to LLVMEJIT instead of a global
# add_definitions — toggling it then does not recompile the whole LLVM tree.
# Effective only when the SRE code pool itself is enabled.
if(EJIT_SRE_CODE_POOL AND EJIT_CODE_POOL_4K_SEAL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_CODE_POOL_4K_SEAL)
endif()

if(EJIT_CODE_POOL_BATCHED_PUBLISH)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_CODE_POOL_BATCHED_PUBLISH)
endif()

# Pin the JIT code pool to a fixed linker-script region [__ejit_code_start,
# __ejit_code_end) in the RX code segment (enable_rw write / enable_ex seal)
# instead of dynamic SRE_MemDbgAlloc. The only user of this macro is the SRE
# adapter (EJitSrePlatform.cpp), so scope it to LLVMEJIT like
# EJIT_CODE_POOL_4K_SEAL above. Effective only when the SRE code pool is on.
if(EJIT_SRE_CODE_POOL AND EJIT_FIXED_CODE_POOL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_FIXED_CODE_POOL)
endif()

if(EJIT_CODE_POOL_FIXED_NEAR_HOT)
  target_compile_definitions(LLVMEJIT PRIVATE
    EJIT_CODE_POOL_FIXED_NEAR_HOT
    EJIT_CODE_POOL_NEAR_HOT_CELL_BYTES=${EJIT_CODE_POOL_NEAR_HOT_CELL_BYTES}
    EJIT_CODE_POOL_NEAR_HOT_PUBLIC_BYTES=${EJIT_CODE_POOL_NEAR_HOT_PUBLIC_BYTES})
endif()

# EJIT_ICACHE_DIM_SIZE: per-dim bound D of the multi-version inline cache.
# Used by icacheFill/icacheTry linearization in EJitSharedTaskPool. Must match
# the AOT -mllvm -print-ejit-icache-dim-size value (both from the top-level CMake
# var). Scoped to LLVMEJIT (the icache runtime lives here); gated on the shared
# taskpool, which is the only build the icache is compiled under.
if(EJIT_SRE_SHARED_TASKPOOL)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE})
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_ICACHE_FUNC_SLOTS=${EJIT_ICACHE_FUNC_SLOTS})
endif()

# Fixed worker core: close the shared taskpool's owner election to the
# designated core (empty = open CAS election, the default). The consumers are
# EJitSharedTaskPool.cpp (the election gate) and EJit.cpp (the init-failure
# message), both inside LLVMEJIT, so scope the definition to this target like
# EJIT_CODE_POOL_4K_SEAL above - toggling it does not recompile the tree.
# Non-empty test, not truthiness: "0" is a valid core id (CMake if() treats
# the string "0" as FALSE - see the trap note in llvm/CMakeLists.txt).
if(EJIT_SRE_SHARED_TASKPOOL AND
   NOT (EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE STREQUAL ""))
  target_compile_definitions(LLVMEJIT PRIVATE
    EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE=${EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE})
endif()

# Cross-core fnPtr sharing capability (option declared in llvm/CMakeLists.txt,
# only meaningful with the shared taskpool). The consumers are
# EJitSharedTaskPool.cpp (the peer pointer gate) and EJitCompileDriver.cpp
# (the resolve path), both inside LLVMEJIT, so scope it to this target like
# EJIT_CODE_POOL_4K_SEAL above - never a global add_definitions: the
# EJITSharedTaskPoolTests target compiles EJitSharedTaskPool.cpp directly and
# deliberately tests the code-sharing-OFF peer rejection, so a leaked
# -DEJIT_SRE_SHARED_CODE_POINTERS makes the gate always-true and fails 5
# tests. EJITTier1WritableIntegrationTests / EJITSharedCacheQueryBench define
# it themselves when they want the sharing path.
if(EJIT_SRE_SHARED_TASKPOOL AND EJIT_SRE_SHARED_CODE_POINTERS)
  target_compile_definitions(LLVMEJIT PRIVATE EJIT_SRE_SHARED_CODE_POINTERS)
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
