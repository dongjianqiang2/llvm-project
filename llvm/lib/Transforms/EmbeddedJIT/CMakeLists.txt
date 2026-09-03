add_llvm_component_library(LLVMEmbeddedJIT
  EJitPassOptions.cpp
  EJitRegisterBitcode.cpp
  EJitRegisterPeriod.cpp
  EJitWrapperGen.cpp
  EJitPeriodHandler.cpp
  EJitAotModulePass.cpp

  LINK_COMPONENTS
  Core
  IPO
  IRReader
  Support
  TransformUtils
  BitWriter
  )

# EJIT_ICACHE_DIM_SIZE: the AOT pass (EJitWrapperGen) emits the per-function
# @__ejit_icache_fn_<name> as a [D]^numDims array; the runtime (LLVMEJIT)
# linearizes with the same D on fill. Both targets get the identical value from
# the top-level CMake var so array layout and linearization never disagree.
#
# EJIT_ICACHE_SECTION: the section that array is placed in — the inter-core
# SHARED section on a multi-core target, so one partitioned table backs every
# core and a period toggle can zero a peer's cells directly. Only this pass
# needs it (the runtime reaches the table through the registered base address,
# whatever section it came from), so it is defined here alone.
target_compile_definitions(LLVMEmbeddedJIT PRIVATE
  EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE}
  EJIT_ICACHE_SECTION="${EJIT_ICACHE_SECTION}")
