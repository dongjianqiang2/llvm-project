//===-- EJitLinkOptimizationPluginTest.cpp --------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitLinkOptimizationPlugin.h"

#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

namespace {

struct BranchStubGraph {
  std::unique_ptr<LinkGraph> G;
  Edge *Branch = nullptr;
  Symbol *Stub = nullptr;
  Symbol *Destination = nullptr;
};

void expectSuccess(Error Err) {
  if (Err)
    ADD_FAILURE() << toString(std::move(Err));
}

BranchStubGraph makeBranchStubGraph(uint64_t FixupAddr, uint64_t TargetAddr,
                                    int64_t Addend = 0,
                                    Triple::ArchType Arch = Triple::aarch64,
                                    int64_t GOTAddend = 0,
                                    bool ResolveViaSetAddress = false) {
  Triple TT;
  TT.setArch(Arch);
  auto G = std::make_unique<LinkGraph>(
      "branch-stub", std::make_shared<orc::SymbolStringPool>(), TT,
      SubtargetFeatures(), aarch64::getEdgeKindName);

  Section &Text =
      G->createSection("$__TEXT", orc::MemProt::Read | orc::MemProt::Exec);
  static const char BranchContent[4] = {};
  auto &BranchBlock = G->createContentBlock(Text, BranchContent,
                                            orc::ExecutorAddr(FixupAddr), 4, 0);

  Symbol &Destination =
      ResolveViaSetAddress
          ? G->addExternalSymbol("destination", 0, false)
          : G->addAbsoluteSymbol(G->intern("destination"),
                                 orc::ExecutorAddr(TargetAddr), 0,
                                 Linkage::Strong, Scope::Default, true);
  if (ResolveViaSetAddress)
    // Mirror JITLink's applyLookupResult: resolve the external symbol's
    // address via setAddress WITHOUT clearing isExternal() (makeAbsolute and
    // addAbsoluteSymbol both clear it). The pass must gate on the address, not
    // isExternal(), or it skips every resolved external stub.
    Destination.getAddressable().setAddress(orc::ExecutorAddr(TargetAddr));

  Section &GOT = G->createSection(aarch64::GOTTableManager::getSectionName(),
                                  orc::MemProt::Read);
  Symbol &GOTEntry = aarch64::createAnonymousPointer(
      *G, GOT, &Destination, static_cast<uint64_t>(GOTAddend));
  GOTEntry.getBlock().setAddress(orc::ExecutorAddr(FixupAddr + 0x2000));

  Section &Stubs = G->createSection(aarch64::PLTTableManager::getSectionName(),
                                    orc::MemProt::Read | orc::MemProt::Exec);
  Symbol &Stub = aarch64::createAnonymousPointerJumpStub(*G, Stubs, GOTEntry);
  Stub.getBlock().setAddress(orc::ExecutorAddr(FixupAddr + 0x1000));

  BranchBlock.addEdge(aarch64::Branch26PCRel, 0, Stub, Addend);
  Edge *Branch = &*BranchBlock.edges().begin();
  return {std::move(G), Branch, &Stub, &Destination};
}

TEST(EJitLinkOptimizationPlugin, RelaxesInRangeForwardBranch) {
  auto T = makeBranchStubGraph(0x10000000, 0x17fffffc);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Destination);
}

TEST(EJitLinkOptimizationPlugin, RelaxesInRangeBackwardBranch) {
  auto T = makeBranchStubGraph(0x18000000, 0x10000000);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Destination);
}

TEST(EJitLinkOptimizationPlugin, KeepsPositiveBoundaryStubbed) {
  auto T = makeBranchStubGraph(0x10000000, 0x18000000);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

TEST(EJitLinkOptimizationPlugin, KeepsOutOfRangeBranchStubbed) {
  auto T = makeBranchStubGraph(0x10000000, 0x20000000);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

TEST(EJitLinkOptimizationPlugin, KeepsMisalignedDestinationStubbed) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001002);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

TEST(EJitLinkOptimizationPlugin, AccountsForBranchAddend) {
  auto T = makeBranchStubGraph(0x10000000, 0x18000000, -4);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Destination);
}

TEST(EJitLinkOptimizationPlugin, KeepsNonstandardGOTAddendStubbed) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001000, 0, Triple::aarch64, 4);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

TEST(EJitLinkOptimizationPlugin, KeepsNonstandardStubEdgeStubbed) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001000);
  for (Edge &E : T.Stub->getBlock().edges())
    if (E.getKind() == aarch64::Page21)
      E.setAddend(4);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

TEST(EJitLinkOptimizationPlugin, SupportsBigEndianTarget) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001000, 0, Triple::aarch64_be);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Destination);
}

TEST(EJitLinkOptimizationPlugin, IgnoresOtherArchitectures) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001000, 0, Triple::x86_64);
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

// C3 regression: applyLookupResult resolves externals via setAddress, which
// keeps isExternal()=true (makeAbsolute and addAbsoluteSymbol both clear it).
// The pass must gate on the address, not isExternal(), or it skips every
// resolved external stub. These mirror the real resolution flow.
TEST(EJitLinkOptimizationPlugin, RelaxesSetAddressResolvedExternal) {
  auto T = makeBranchStubGraph(0x10000000, 0x10001000, 0, Triple::aarch64, 0,
                               /*ResolveViaSetAddress=*/true);
  EXPECT_TRUE(T.Destination->isExternal()); // setAddress did not clear it
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Destination);
}

TEST(EJitLinkOptimizationPlugin, KeepsUnresolvedExternalStubbed) {
  // setAddress(0): genuinely unresolved (e.g. weak external with no defn).
  auto T = makeBranchStubGraph(0x10000000, 0, 0, Triple::aarch64, 0,
                               /*ResolveViaSetAddress=*/true);
  EXPECT_TRUE(T.Destination->isExternal());
  expectSuccess(relaxAArch64BranchStubs(*T.G));
  EXPECT_EQ(&T.Branch->getTarget(), T.Stub);
}

} // namespace
