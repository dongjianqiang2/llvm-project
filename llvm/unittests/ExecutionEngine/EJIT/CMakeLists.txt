set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  Core
  EmbeddedJIT
  EJIT
  ExecutionEngine
  Instrumentation
  OrcJIT
  Passes
  ProfileData
  Support
  TargetParser
  )

add_llvm_unittest(EJITTests
  EJitRuntimeTest.cpp
  EJitFieldOffsetTest.cpp
  EJitLinkOptimizationPluginTest.cpp
  EJitPgoTest.cpp
  EJitVpMergeTest.cpp
  EJitVpScalarSpecTest.cpp
  EJitVpPipelineTest.cpp

  PARTIAL_SOURCES_INTENDED
  )

# The value-profile test sources are #ifdef EJIT_SRE_PGO_VALUE_PROFILE-gated,
# matching the LLVMEJIT build; without these definitions they compile to empty
# translation units.
if(EJIT_SRE_PGO_VALUE_PROFILE)
  target_compile_definitions(EJITTests PRIVATE
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
  target_compile_definitions(EJITTests PRIVATE EJIT_SRE_PGO_BRANCH_AUDIT)
endif()

if(EJIT_SRE_SVE_VECTORIZATION)
  target_compile_definitions(EJITTests PRIVATE EJIT_SRE_SVE_VECTORIZATION)
  if("AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
    target_compile_definitions(EJITTests PRIVATE EJIT_TEST_HAS_AARCH64)
  endif()
endif()

# EJit.cpp calls InitializeAllAsmParsers(); a target's AsmParser lib is only
# referenced at runtime (no CMake dependency edge), so when listed via
# LLVM_LINK_COMPONENTS it can be ordered BEFORE LLVMEJIT on the static link
# line and its LLVMInitialize<T>AsmParser symbol is dropped. Append each built
# target's AsmParser lib AFTER the target so the reference resolves.
foreach(ejit_target ${LLVM_TARGETS_TO_BUILD})
  if(TARGET LLVM${ejit_target}AsmParser)
    target_link_libraries(EJITTests PRIVATE LLVM${ejit_target}AsmParser)
  endif()
endforeach()

# SRE code pool tests live in their own executable so they build and run
# independently of EJITTests (host-runnable: mock allocator + mock enable_ex,
# no real SRE symbols required).
add_llvm_unittest(EJITCodePoolTests
  EJitCodePoolTest.cpp
  EJitCodePoolMemoryManagerTest.cpp

  PARTIAL_SOURCES_INTENDED
  )

# Convenience target: build + run only the SRE code pool unit tests.
add_custom_target(check-ejit-code-pool
  COMMAND $<TARGET_FILE:EJITCodePoolTests>
  DEPENDS EJITCodePoolTests
  COMMENT "Running EmbeddedJIT SRE code pool unit tests"
  USES_TERMINAL
  )

# SRE taskpool tests: compile the taskpool sources directly with the taskpool
# macros so they build and run independently of LLVMEJIT / EJITTests and need
# no real SRE platform symbols (mock compiler + mock ring queue). Only LLVMSupport
# is linked (for the gtest harness), so this target never pulls in OrcJIT/EJIT.
set(EJIT_TASKPOOL_LINK_COMPONENTS_SAVED "${LLVM_LINK_COMPONENTS}")
set(LLVM_LINK_COMPONENTS Support)
add_llvm_unittest(EJITTaskPoolTests
  EJitTaskPoolTest.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitTaskPool.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSreQueue.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitWorker.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSreTask_host.cpp
  # Defines gEJitDiagLevel (the level gate behind every EJIT_DIAG* call in the
  # sources above). Required since "Raise diagnostic print throttle" (2b1aab7)
  # added EJIT_DIAG* calls to EJitTaskPool.cpp: without it this target fails to
  # link, since it deliberately links only LLVMSupport and never the full
  # LLVMEJIT (which carries EJitLogger.cpp) - same pattern as
  # EJITSharedTaskPoolTests below.
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitLogger.cpp

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_TASKPOOL_LINK_COMPONENTS_SAVED}")

# Numeric geometry must come from the CMake cache vars (not literals), so the
# test TU compiles the shared sources with the SAME values as production
# LLVMEJIT — a tuned cache var silently stopping being tested is exactly the
# config-desync class this whole file guards against.
target_compile_definitions(EJITTaskPoolTests PRIVATE
  EJIT_SRE_TASKPOOL
  EJIT_SRE_TASKPOOL_TESTING
  EJIT_STATS_ENABLE
  EJIT_SRE_TASKPOOL_BUCKETS=${EJIT_SRE_TASKPOOL_BUCKETS}
  EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=${EJIT_SRE_TASKPOOL_QUEUE_CAPACITY}
  EJIT_TASKPOOL_SOURCE_DIR="${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT"
  EJIT_TASKPOOL_INCLUDE_DIR="${LLVM_MAIN_SRC_DIR}/include/llvm/ExecutionEngine/EJIT"
  )

# Convenience target: build + run only the SRE taskpool unit tests.
add_custom_target(check-ejit-taskpool
  COMMAND $<TARGET_FILE:EJITTaskPoolTests>
  DEPENDS EJITTaskPoolTests
  COMMENT "Running EmbeddedJIT SRE taskpool unit tests"
  USES_TERMINAL
  )

# Cross-core SHARED taskpool tests: compile the shared sources directly with the
# shared-taskpool macros so they build and run independently of LLVMEJIT and need
# no real SRE platform symbols. Many cores are simulated in one host process via
# the injectable EJitCoreId; no real thread is required. Only LLVMSupport is
# linked (the gtest harness).
set(EJIT_SHARED_TASKPOOL_LINK_COMPONENTS_SAVED "${LLVM_LINK_COMPONENTS}")
set(LLVM_LINK_COMPONENTS Support)
add_llvm_unittest(EJITSharedTaskPoolTests
  EJitSharedTaskPoolTest.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedTaskPool.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp
  # Defines gEJitDiagLevel (the level gate behind every EJIT_DIAG* call in the
  # two sources above). Needed when the global EJIT_DIAG_ENABLE is ON: without
  # it the test binary fails to link, since this target deliberately links only
  # LLVMSupport and never the full LLVMEJIT (which carries EJitLogger.cpp).
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitLogger.cpp

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_SHARED_TASKPOOL_LINK_COMPONENTS_SAVED}")

# The icache geometry must match the production LLVMEJIT build (same CMake
# vars, same static_assert): these sources are compiled INTO the test target,
# so the ${EJIT_*} cache vars must be forwarded explicitly — the header
# fallbacks exist only for non-CMake builds and the static_asserts would fire
# (or silently test different geometry) if they diverged.
target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
  EJIT_SRE_TASKPOOL
  EJIT_SRE_SHARED_TASKPOOL
  EJIT_SRE_TASKPOOL_TESTING
  EJIT_STATS_ENABLE
  EJIT_SRE_TASKPOOL_BUCKETS=${EJIT_SRE_TASKPOOL_BUCKETS}
  EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=${EJIT_SRE_TASKPOOL_QUEUE_CAPACITY}
  EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE=${EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE}
  EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE}
  EJIT_ICACHE_FUNC_SLOTS=${EJIT_ICACHE_FUNC_SLOTS}
  EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX=${EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX}
  # MULT=3 (not the default 1) so tests can assert the multiplier takes effect:
  # the throttle must pass MULT*DELAY_TICKS=300 in one call, catching "passes 1"
  # and "multiply omitted" regressions that MULT=1 cannot distinguish.
  EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT=3
  EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS=100
  )
if(EJIT_CODE_POOL_BATCHED_PUBLISH)
  target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
    EJIT_CODE_POOL_BATCHED_PUBLISH)
endif()

# Forward the production code-sharing capability from the cache so the test
# binary compiles EJitSharedTaskPool.cpp with the SAME compile-time fnPtr gate
# as the LLVMEJIT this suite validates (the peer-path tests - FourK seal, peer
# prepare, shared fnPtr - exercise the macro-ON gate; the code-sharing-OFF
# tests are #ifndef-guarded in the test source). The global
# add_definitions(-DEJIT_SRE_SHARED_CODE_POINTERS) was removed from
# llvm/CMakeLists.txt precisely because it leaked into this target and pinned
# the gate ON while the cache said otherwise; the forwarding below restores
# the intended coupling in both directions. EJIT_TEST_NO_RECLAIM below also
# turns it on unconditionally for that opt-in configuration. Mirrors the
# LLVMEJIT-side condition (shared taskpool AND code pointers) so a
# TASKPOOL=OFF + CODE_POINTERS=ON corner config cannot leave the test binary
# gated while the library is not.
if(EJIT_SRE_SHARED_TASKPOOL AND EJIT_SRE_SHARED_CODE_POINTERS)
  target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
    EJIT_SRE_SHARED_CODE_POINTERS)
endif()

# Opt-in: build the shared taskpool tests with the load-only NO_RECLAIM seqlock
# hit path (and cross-core code sharing, so the peer path is exercised too), to
# validate the optimization. Configure a separate build dir with
# -DEJIT_TEST_NO_RECLAIM=ON. Off by default so the normal build tests the token
# path.
option(EJIT_TEST_NO_RECLAIM
  "Build EJITSharedTaskPoolTests with EJIT_SRE_TASKPOOL_NO_RECLAIM" OFF)
if(EJIT_TEST_NO_RECLAIM)
  target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
    EJIT_SRE_TASKPOOL_NO_RECLAIM
    EJIT_SRE_SHARED_CODE_POINTERS)
endif()

# Opt-in: build the shared taskpool tests with the worker core PINNED (the
# production CMake policy EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE), so the
# fixed-core election path is exercised: a non-designated core never claims the
# blob, and the designated core wins even when a peer called init() first.
# Pinned to 0 (NOT the production cache var) because the whole suite's
# bringUpOwner runs on core 0; the two open-election tests that rely on a
# non-zero core winning are #ifndef-guarded in the test source. Configure a
# separate build dir with -DEJIT_TEST_FIXED_WORKER_CORE=ON. Off by default so
# the normal build tests the open-election path.
option(EJIT_TEST_FIXED_WORKER_CORE
  "Build EJITSharedTaskPoolTests with the shared worker core pinned to 0" OFF)
if(EJIT_TEST_FIXED_WORKER_CORE)
  target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
    EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE=0)
endif()

# Convenience target: build + run only the cross-core shared taskpool tests.
add_custom_target(check-ejit-shared-taskpool
  COMMAND $<TARGET_FILE:EJITSharedTaskPoolTests>
  DEPENDS EJITSharedTaskPoolTests
  COMMENT "Running EmbeddedJIT cross-core shared taskpool unit tests"
  USES_TERMINAL
  )

# Cross-core runtime-writable (Online-PGO Tier-1) END-TO-END integration test.
# It spans BOTH real components changed by the fix in one binary: the code-pool
# JITLink finalize path (EJitCodePool / EJitCodePoolMemoryManager) AND the
# shared taskpool peer preparation (EJitSharedTaskPool). The shared-taskpool
# sources are compiled directly here (with the shared macros + code sharing) so
# the peer read path is active; the code-pool sources are compiled directly too
# so the target links only JITLink + OrcShared (never libLLVMEJIT, which would
# duplicate the directly-compiled EJitSharedTaskPool symbols). No real SRE
# platform symbols are needed (all callbacks are mocked).
set(EJIT_TIER1_LINK_COMPONENTS_SAVED "${LLVM_LINK_COMPONENTS}")
set(LLVM_LINK_COMPONENTS JITLink OrcShared Support TargetParser)
add_llvm_unittest(EJITTier1WritableIntegrationTests
  EJitTier1WritableIntegrationTest.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitCodePool.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedTaskPool.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp
  # Defines gEJitDiagLevel (the level gate behind every EJIT_DIAG* call in the
  # sources above). Required since "Harden shared online PGO" (ea80ca36) made
  # EJitCodePool.cpp emit EJIT_DIAG* calls: without it this target fails to
  # link, since it deliberately links only JITLink/OrcShared and never the
  # full LLVMEJIT (which carries EJitLogger.cpp) - same pattern as
  # EJITSharedTaskPoolTests above.
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitLogger.cpp

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_TIER1_LINK_COMPONENTS_SAVED}")

target_compile_definitions(EJITTier1WritableIntegrationTests PRIVATE
  EJIT_SRE_TASKPOOL
  EJIT_SRE_SHARED_TASKPOOL
  EJIT_SRE_TASKPOOL_TESTING
  EJIT_SRE_SHARED_CODE_POINTERS
  EJIT_STATS_ENABLE
  EJIT_SRE_TASKPOOL_BUCKETS=32
  EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024
  EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE=1048576
  )

add_custom_target(check-ejit-tier1-writable
  COMMAND $<TARGET_FILE:EJITTier1WritableIntegrationTests>
  DEPENDS EJITTier1WritableIntegrationTests
  COMMENT "Running EmbeddedJIT Tier-1 runtime-writable integration test"
  USES_TERMINAL
  )

# Online-PGO value-profiling collector tests: compile the collector sources
# directly with the VP macros so they build and run independently of LLVMEJIT
# and need no real SRE platform symbols. Only LLVMSupport is linked (the gtest
# harness), so this target never pulls in OrcJIT/EJIT. Multi-core behavior is
# simulated in one host process via the injectable EJitCoreId (no real thread).
set(EJIT_VP_LINK_COMPONENTS_SAVED "${LLVM_LINK_COMPONENTS}")
set(LLVM_LINK_COMPONENTS Support)
add_llvm_unittest(EJITValueProfileTests
  EJitVpCollectorTest.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitVpCollector.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSreTask_host.cpp

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_VP_LINK_COMPONENTS_SAVED}")

target_compile_definitions(EJITValueProfileTests PRIVATE
  EJIT_SRE_PGO_VALUE_PROFILE
  EJIT_SRE_VP_K=2
  EJIT_SRE_VP_SITES_PER_CORE=64
  EJIT_SRE_VP_MAX_CORES=${EJIT_SRE_VP_MAX_CORES}
  EJIT_SRE_VP_TOP_N=2
  EJIT_SRE_VP_DRAIN_TICKS=64
  )

# Convenience target: build + run only the value-profiling collector tests.
add_custom_target(check-ejit-value-profile
  COMMAND $<TARGET_FILE:EJITValueProfileTests>
  DEPENDS EJITValueProfileTests
  COMMENT "Running EmbeddedJIT online-PGO value-profiling collector unit tests"
  USES_TERMINAL
  )

# Branch-audit tests are independent of LLVMEJIT and always compile the small
# read-only analyzer, even though production inclusion is default-off.
set(EJIT_BRANCH_AUDIT_LINK_COMPONENTS_SAVED "${LLVM_LINK_COMPONENTS}")
set(LLVM_LINK_COMPONENTS Core Support)
add_llvm_unittest(EJITBranchProfileTests
  EJitBranchProfileTest.cpp
  ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitBranchProfile.cpp

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_BRANCH_AUDIT_LINK_COMPONENTS_SAVED}")

add_custom_target(check-ejit-branch-profile
  COMMAND $<TARGET_FILE:EJITBranchProfileTests>
  DEPENDS EJITBranchProfileTests
  COMMENT "Running EmbeddedJIT branch-profile audit unit tests"
  USES_TERMINAL
  )

# Optional cache-query microbenchmark (NOT a gtest binary: standalone main()).
# Off by default so it never bloats normal test builds. Enable with
# -DEJIT_BUILD_CACHE_QUERY_BENCH=ON to build a host-runnable binary that prints a
# machine-readable table of the 0D/1D/2D owner/peer cache-hit query cost by slot
# depth (drive it under `perf stat -e instructions` for per-hit attribution).
# Links only the shared-taskpool sources + platform, like the tests above.
option(EJIT_BUILD_CACHE_QUERY_BENCH
  "Build the EJIT shared taskpool cache-query microbenchmark" OFF)
if(EJIT_BUILD_CACHE_QUERY_BENCH)
  add_llvm_executable(EJITSharedCacheQueryBench
    EJitSharedCacheQueryBench.cpp
    ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedTaskPool.cpp
    ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp
    ${LLVM_MAIN_SRC_DIR}/lib/ExecutionEngine/EJIT/EJitLogger.cpp

    PARTIAL_SOURCES_INTENDED
    )
  target_compile_definitions(EJITSharedCacheQueryBench PRIVATE
    EJIT_SRE_TASKPOOL
    EJIT_SRE_SHARED_TASKPOOL
    EJIT_SRE_TASKPOOL_TESTING
    EJIT_SRE_TASKPOOL_BUCKETS=${EJIT_SRE_TASKPOOL_BUCKETS}
    EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=${EJIT_SRE_TASKPOOL_QUEUE_CAPACITY}
    EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE}
    EJIT_ICACHE_FUNC_SLOTS=${EJIT_ICACHE_FUNC_SLOTS}
    EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX=${EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX}
    )
  # Opt-in: build the benchmark with the load-only NO_RECLAIM seqlock hit path
  # + cross-core code sharing (so peer hits and the fixed-dim seqlock lookups
  # are exercised), mirroring EJIT_TEST_NO_RECLAIM above.
  if(EJIT_TEST_NO_RECLAIM)
    target_compile_definitions(EJITSharedCacheQueryBench PRIVATE
      EJIT_SRE_TASKPOOL_NO_RECLAIM
      EJIT_SRE_SHARED_CODE_POINTERS)
  endif()
endif()
