//===-- CodeGen.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the common initialization routines for the
// CodeGen library.
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

/// initializeCodeGen - Initialize all passes linked into the CodeGen library.
void llvm::initializeCodeGen(PassRegistry &Registry) {
  initializeAssignmentTrackingAnalysisPass(Registry);
  initializeAtomicExpandLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeBasicBlockPathCloningPass(Registry);
  initializeBasicBlockSectionsPass(Registry);
#endif
  initializeBranchFolderLegacyPass(Registry);
  initializeBranchRelaxationLegacyPass(Registry);
  initializeBreakFalseDepsPass(Registry);
  initializeCallBrPreparePass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeCFGuardLongjmpPass(Registry);
#endif
  initializeCFIFixupPass(Registry);
  initializeCFIInstrInserterPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeCheckDebugMachineModulePass(Registry);
#endif
  initializeCodeGenPrepareLegacyPassPass(Registry);
  initializeDeadMachineInstructionElimPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeDebugifyMachineModulePass(Registry);
#endif
  initializeDetectDeadLanesLegacyPass(Registry);
  initializeDwarfEHPrepareLegacyPassPass(Registry);
  initializeEarlyIfConverterLegacyPass(Registry);
  initializeEarlyIfPredicatorPass(Registry);
  initializeEarlyMachineLICMPass(Registry);
  initializeEarlyTailDuplicateLegacyPass(Registry);
  initializeExpandLargeDivRemLegacyPassPass(Registry);
  initializeExpandFpLegacyPassPass(Registry);
  initializeExpandMemCmpLegacyPassPass(Registry);
  initializeExpandPostRALegacyPass(Registry);
  initializeFEntryInserterLegacyPass(Registry);
  initializeFinalizeISelPass(Registry);
  initializeFinalizeMachineBundlesPass(Registry);
  initializeFixupStatepointCallerSavedLegacyPass(Registry);
  initializeFuncletLayoutPass(Registry);
  initializeGCMachineCodeAnalysisPass(Registry);
  initializeGCModuleInfoPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeHardwareLoopsLegacyPass(Registry);
#endif
  initializeIfConverterPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeImplicitNullChecksPass(Registry);
#endif
  initializeIndirectBrExpandLegacyPassPass(Registry);
  initializeInitUndefLegacyPass(Registry);
  initializeInterleavedLoadCombinePass(Registry);
  initializeInterleavedAccessPass(Registry);
  initializeJMCInstrumenterPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeLiveDebugValuesLegacyPass(Registry);
#endif
  initializeLiveDebugVariablesWrapperLegacyPass(Registry);
  initializeLiveIntervalsWrapperPassPass(Registry);
  initializeLiveRangeShrinkPass(Registry);
  initializeLiveStacksWrapperLegacyPass(Registry);
  initializeLiveVariablesWrapperPassPass(Registry);
  initializeLocalStackSlotPassPass(Registry);
  initializeLowerGlobalDtorsLegacyPassPass(Registry);
  initializeLowerIntrinsicsPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMIRAddFSDiscriminatorsPass(Registry);
#endif
  initializeMIRCanonicalizerPass(Registry);
  initializeMIRNamerPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMIRProfileLoaderPassPass(Registry);
#endif
  initializeMachineBlockFrequencyInfoWrapperPassPass(Registry);
  initializeMachineBlockPlacementLegacyPass(Registry);
  initializeMachineBlockPlacementStatsLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachineCFGPrinterPass(Registry);
#endif
  initializeMachineCSELegacyPass(Registry);
  initializeMachineCombinerPass(Registry);
  initializeMachineCopyPropagationLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachineCycleInfoPrinterLegacyPass(Registry);
#endif
  initializeMachineCycleInfoWrapperPassPass(Registry);
  initializeMachineDominatorTreeWrapperPassPass(Registry);
  initializeMachineFunctionPrinterPassPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachineFunctionSplitterPass(Registry);
#endif
  initializeMachineLateInstrsCleanupLegacyPass(Registry);
  initializeMachineLICMPass(Registry);
  initializeMachineLoopInfoWrapperPassPass(Registry);
  initializeMachineModuleInfoWrapperPassPass(Registry);
  initializeMachineOptimizationRemarkEmitterPassPass(Registry);
  initializeMachineOutlinerPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachinePipelinerPass(Registry);
#endif
  initializeMachineSanitizerBinaryMetadataLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeModuloScheduleTestPass(Registry);
#endif
  initializeMachinePostDominatorTreeWrapperPassPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachineRegionInfoPassPass(Registry);
#endif
  initializeMachineSchedulerLegacyPass(Registry);
  initializeMachineSinkingLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeMachineUniformityAnalysisPassPass(Registry);
  initializeMachineUniformityInfoPrinterPassPass(Registry);
  initializeMachineVerifierLegacyPassPass(Registry);
#endif
  initializeObjCARCContractLegacyPassPass(Registry);
  initializeOptimizePHIsLegacyPass(Registry);
  initializePEILegacyPass(Registry);
  initializePHIEliminationPass(Registry);
  initializePatchableFunctionLegacyPass(Registry);
  initializePeepholeOptimizerLegacyPass(Registry);
  initializePostMachineSchedulerLegacyPass(Registry);
  initializePostRAMachineSinkingLegacyPass(Registry);
  initializePostRAHazardRecognizerLegacyPass(Registry);
  initializePostRASchedulerLegacyPass(Registry);
  initializePreISelIntrinsicLoweringLegacyPassPass(Registry);
  initializeProcessImplicitDefsPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeRABasicPass(Registry);
  initializeRegAllocFastPass(Registry);
#endif // EJIT_TRIM_LLVM_BACKEND
  initializeRAGreedyLegacyPass(Registry);
  initializeRegUsageInfoCollectorLegacyPass(Registry);
  initializeRegUsageInfoPropagationLegacyPass(Registry);
  initializeRegisterCoalescerLegacyPass(Registry);
  initializeRemoveLoadsIntoFakeUsesLegacyPass(Registry);
  initializeRemoveRedundantDebugValuesLegacyPass(Registry);
  initializeRenameIndependentSubregsLegacyPass(Registry);
  initializeSafeStackLegacyPassPass(Registry);
  initializeSelectOptimizePass(Registry);
  initializeShadowStackGCLoweringPass(Registry);
  initializeShrinkWrapLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeSjLjEHPreparePass(Registry);
#endif
  initializeSlotIndexesWrapperPassPass(Registry);
  initializeStackColoringLegacyPass(Registry);
  initializeStackFrameLayoutAnalysisLegacyPass(Registry);
  initializeStackMapLivenessPass(Registry);
  initializeStackProtectorPass(Registry);
  initializeStackSlotColoringLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeStaticDataSplitterPass(Registry);
  initializeStaticDataAnnotatorPass(Registry);
  initializeStripDebugMachineModulePass(Registry);
#endif
  initializeTailDuplicateLegacyPass(Registry);
  initializeTargetPassConfigPass(Registry);
  initializeTwoAddressInstructionLegacyPassPass(Registry);
  initializeTypePromotionLegacyPass(Registry);
  initializeUnpackMachineBundlesPass(Registry);
  initializeUnreachableBlockElimLegacyPassPass(Registry);
  initializeUnreachableMachineBlockElimLegacyPass(Registry);
  initializeVirtRegMapWrapperLegacyPass(Registry);
  initializeVirtRegRewriterLegacyPass(Registry);
#ifndef EJIT_TRIM_LLVM_BACKEND
  initializeWasmEHPreparePass(Registry);
  initializeWinEHPreparePass(Registry);
#endif
  initializeXRayInstrumentationLegacyPass(Registry);
}
