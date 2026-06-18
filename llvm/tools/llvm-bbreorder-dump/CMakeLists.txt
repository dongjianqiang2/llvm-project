set(LLVM_LINK_COMPONENTS
  BinaryFormat
  Core
  Object
  Option
  Support
  )

add_llvm_tool(llvm-bbreorder-dump
  llvm-bbreorder-dump.cpp
  GENERATE_DRIVER
  )
