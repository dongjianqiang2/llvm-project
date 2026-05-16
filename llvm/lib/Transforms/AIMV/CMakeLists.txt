add_llvm_component_library(LLVMAIMV
  AIMVFeedbackPass.cpp
  AIMVDiagnosticParser.cpp

  LINK_COMPONENTS
  Core
  Analysis
  Support
  TransformUtils
  Vectorize
  TargetParser
)
