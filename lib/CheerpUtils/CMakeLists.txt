add_llvm_library(LLVMCheerpUtils
  AllocaMerging.cpp
  NativeRewriter.cpp
  PointerAnalyzer.cpp
  PointerPasses.cpp
  ReplaceNopCasts.cpp
  ResolveAliases.cpp
  Registerize.cpp
  StructMemFuncLowering.cpp
  Utility.cpp
  )

add_dependencies(LLVMCheerpUtils intrinsics_gen)
