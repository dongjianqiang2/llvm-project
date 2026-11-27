#!/bin/bash
# Build the talk's microbenchmarks (demo.c / probe.c) against the release x86 toolchain.
# usage: bench/build.sh [aot]      builds demo + probe (or only the AOT asm of demo) into bench/out/
# env:  ROOT=<checkout>  D=<output dir>
set -euo pipefail
ROOT=${ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
B=$ROOT/build_release_x86
S=$(cd "$(dirname "$0")" && pwd)
D=${D:-$S/out}          # outputs (git-ignored)
mkdir -p "$D"
l=$B/lib
LIBS="
$l/libLLVMCore.a $l/libLLVMSupport.a $l/libLLVMDemangle.a
$l/libLLVMBinaryFormat.a $l/libLLVMBitReader.a $l/libLLVMBitstreamReader.a
$l/libLLVMAnalysis.a $l/libLLVMScalarOpts.a $l/libLLVMInstCombine.a
$l/libLLVMPasses.a $l/libLLVMAggressiveInstCombine.a
$l/libLLVMCoroutines.a
$l/libLLVMipo.a $l/libLLVMTransformUtils.a $l/libLLVMVectorize.a
$l/libLLVMCodeGen.a
$l/libLLVMCodeGenTypes.a $l/libLLVMTarget.a $l/libLLVMTargetParser.a
$l/libLLVMSelectionDAG.a $l/libLLVMAsmPrinter.a $l/libLLVMMC.a
$l/libLLVMObject.a $l/libLLVMProfileData.a $l/libLLVMExecutionEngine.a
$l/libLLVMOrcJIT.a $l/libLLVMOrcShared.a $l/libLLVMJITLink.a
$l/libLLVMRemarks.a $l/libLLVMOption.a $l/libLLVMMCDisassembler.a
$l/libLLVMIRPrinter.a $l/libLLVMCFGuard.a
$l/libLLVMInstrumentation.a
$l/libLLVMDebugInfoCodeView.a $l/libLLVMDebugInfoDWARF.a $l/libLLVMDebugInfoDWARFLowLevel.a $l/libLLVMMCParser.a
$l/libLLVMCGData.a $l/libLLVMObjCARCOpts.a
$l/libLLVMGlobalISel.a
$l/libLLVMOrcTargetProcess.a $l/libLLVMRuntimeDyld.a
$l/libLLVMBitWriter.a
$l/libLLVMX86CodeGen.a $l/libLLVMX86Desc.a $l/libLLVMX86Info.a
$l/libLLVMX86AsmParser.a"
INC="-I$ROOT/llvm/include -I$B/include -I$ROOT/ejit_test"
if [[ "${1:-}" == "aot" ]]; then
  # AOT assembly of the same source (wrapper + fallback body), unstripped.
  $B/bin/clang -O2 $INC -S -o $D/demo_aot.s $S/demo.c
  echo "AOT asm -> $D/demo_aot.s"
  exit 0
fi
for n in demo probe; do
  $B/bin/clang -O2 $INC -c $S/$n.c -o $D/$n.o
  $B/bin/clang++ -fuse-ld=$B/bin/ld.lld -Os -Wl,--gc-sections \
    -Wl,--whole-archive $l/libLLVMEJIT.a -Wl,--no-whole-archive \
    $(echo "$LIBS" | tr '\n' ' ') \
    -lz -lpthread -ldl -Wl,--defsym,__stack_chk_guard=0 \
    $D/$n.o -o $D/$n
done
ls -la $D/demo $D/probe
