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

  PARTIAL_SOURCES_INTENDED
  )

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

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_TASKPOOL_LINK_COMPONENTS_SAVED}")

target_compile_definitions(EJITTaskPoolTests PRIVATE
  EJIT_SRE_TASKPOOL
  EJIT_SRE_TASKPOOL_TESTING
  EJIT_STATS_ENABLE
  EJIT_SRE_TASKPOOL_BUCKETS=32
  EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024
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

  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS "${EJIT_SHARED_TASKPOOL_LINK_COMPONENTS_SAVED}")

target_compile_definitions(EJITSharedTaskPoolTests PRIVATE
  EJIT_SRE_TASKPOOL
  EJIT_SRE_SHARED_TASKPOOL
  EJIT_SRE_TASKPOOL_TESTING
  EJIT_STATS_ENABLE
  EJIT_SRE_TASKPOOL_BUCKETS=32
  EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024
  EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE=1048576
  )

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

# Convenience target: build + run only the cross-core shared taskpool tests.
add_custom_target(check-ejit-shared-taskpool
  COMMAND $<TARGET_FILE:EJITSharedTaskPoolTests>
  DEPENDS EJITSharedTaskPoolTests
  COMMENT "Running EmbeddedJIT cross-core shared taskpool unit tests"
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

    PARTIAL_SOURCES_INTENDED
    )
  target_compile_definitions(EJITSharedCacheQueryBench PRIVATE
    EJIT_SRE_TASKPOOL
    EJIT_SRE_SHARED_TASKPOOL
    EJIT_SRE_TASKPOOL_TESTING
    EJIT_SRE_TASKPOOL_BUCKETS=32
    EJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024
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
