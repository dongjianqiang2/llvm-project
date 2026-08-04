add_llvm_component_library(LLVMEmbeddedJIT
  EJitPassOptions.cpp
  EJitRegisterBitcode.cpp
  EJitRegisterPeriod.cpp
  EJitWrapperGen.cpp
  EJitPeriodHandler.cpp
  EJitAotModulePass.cpp

  LINK_COMPONENTS
  Core
  IRReader
  Support
  TransformUtils
  BitWriter
  )

# EJIT_ICACHE_DIM_SIZE: the AOT pass (EJitWrapperGen) emits the per-function
# @__ejit_icache_fn_<name> as a [D]^numDims array; the runtime (LLVMEJIT)
# linearizes with the same D on fill. Both targets get the identical value from
# the top-level CMake var so array layout and linearization never disagree.
target_compile_definitions(LLVMEmbeddedJIT PRIVATE
  EJIT_ICACHE_DIM_SIZE=${EJIT_ICACHE_DIM_SIZE})
