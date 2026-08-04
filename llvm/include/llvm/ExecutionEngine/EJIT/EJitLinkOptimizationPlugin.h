//===-- EJitLinkOptimizationPlugin.h - EJIT JITLink optimizations ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITLINKOPTIMIZATIONPLUGIN_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITLINKOPTIMIZATIONPLUGIN_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"

namespace llvm {
namespace ejit {

/// Retarget AArch64 B/BL edges from standard JITLink pointer-jump stubs to
/// their resolved destination when that destination is directly reachable.
LLVM_ABI Error relaxAArch64BranchStubs(jitlink::LinkGraph &G);

class EJitLinkOptimizationPlugin : public orc::LinkGraphLinkingLayer::Plugin {
public:
  void modifyPassConfig(orc::MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override;

  Error notifyFailed(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyRemovingResources(orc::JITDylib &JD,
                                orc::ResourceKey K) override {
    return Error::success();
  }
  void notifyTransferringResources(orc::JITDylib &JD, orc::ResourceKey DstKey,
                                   orc::ResourceKey SrcKey) override {}
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITLINKOPTIMIZATIONPLUGIN_H
