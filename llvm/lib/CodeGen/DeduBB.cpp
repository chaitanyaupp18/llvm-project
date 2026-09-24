//===- DeduBB.cpp - DeduBB tail-call basic block deduplication ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass applies DeduBB tail-call deduplication directives produced by
// Propeller (`--tail_call_profile=dedubb.txt`). The directive file marks
// byte-identical basic blocks that end in a tail call or return:
//
//   bbm <id> (DeduBB.master.K)  -- block <id> is the master copy K. The
//                                  AsmPrinter emits a global symbol
//                                  `DeduBB.master.K` at the block start (see
//                                  AsmPrinter::emitBasicBlockStart).
//   bbf <id> (DeduBB.master.K)  -- block <id> is a duplicate. This pass deletes
//                                  the block body and replaces it with a single
//                                  jump to `DeduBB.master.K`.
//
// Folding is realized as a tail branch (the same instruction the
// MachineOutliner uses for tail-called outlined functions) so that the current
// stack frame is preserved: control jumps into the identical master block,
// whose tail call or return then transfers control exactly as the original
// block would have.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/DeduBBDirectives.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/UniqueBBID.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>

using namespace llvm;

#define DEBUG_TYPE "dedubb"

static cl::opt<std::string> DeduBBDirectivesFile(
    "dedubb-directives", cl::Hidden, cl::init(""),
    cl::desc("Path to a DeduBB tail-call deduplication directive file "
             "(m/f/bbm/bbf directives) to apply during code generation."));

//===----------------------------------------------------------------------===//
// Directive file parsing.
//===----------------------------------------------------------------------===//

// Parses the master id K out of a "(DeduBB.master.K)" token.
static bool parseMasterID(StringRef S, uint64_t &K) {
  constexpr StringRef kPrefix = "DeduBB.master.";
  size_t Pos = S.find(kPrefix);
  if (Pos == StringRef::npos)
    return false;
  StringRef Rest = S.substr(Pos + kPrefix.size());
  size_t End = 0;
  while (End < Rest.size() && Rest[End] >= '0' && Rest[End] <= '9')
    ++End;
  return End != 0 && !Rest.substr(0, End).getAsInteger(10, K);
}

bool DeduBBDirectives::parse(StringRef Text) {
  StringRef CurFunc;
  bool Ok = true;
  SmallVector<StringRef> Lines;
  Text.split(Lines, '\n');
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (Line.empty() || Line.starts_with("#"))
      continue;
    auto [Kw, Rest] = Line.split(' ');
    Rest = Rest.trim();
    if (Kw == "m") {
      // Module header. Matching is by function name, so this is informational.
      continue;
    }
    if (Kw == "f") {
      CurFunc = Rest.split(".llvm.").first;
      continue;
    }
    if (Kw == "bbm" || Kw == "bbf") {
      if (CurFunc.empty()) {
        errs() << "dedubb: directive before any 'f' line: '" << Line << "'\n";
        Ok = false;
        continue;
      }
      auto [IdStr, Tail] = Rest.split(' ');
      unsigned BBID;
      uint64_t K;
      if (IdStr.getAsInteger(10, BBID) || !parseMasterID(Tail, K)) {
        errs() << "dedubb: malformed directive: '" << Line << "'\n";
        Ok = false;
        continue;
      }
      Map[CurFunc][BBID] =
          Directive{Kw == "bbm" ? Master : Fold, K};
      continue;
    }
    errs() << "dedubb: unknown directive '" << Kw << "'\n";
    Ok = false;
  }
  return Ok;
}

const DeduBBDirectives::Directive *
DeduBBDirectives::lookup(StringRef FuncName, unsigned BBID) const {
  auto It = Map.find(FuncName.split(".llvm.").first);
  if (It == Map.end())
    return nullptr;
  auto J = It->second.find(BBID);
  return J == It->second.end() ? nullptr : &J->second;
}

const DeduBBDirectives &DeduBBDirectives::get() {
  static DeduBBDirectives Directives;
  static bool Initialized = [] {
    if (DeduBBDirectivesFile.empty())
      return true;
    ErrorOr<std::unique_ptr<MemoryBuffer>> Buf =
        MemoryBuffer::getFile(DeduBBDirectivesFile);
    if (!Buf) {
      errs() << "dedubb: cannot open directive file '" << DeduBBDirectivesFile
             << "': " << Buf.getError().message() << "\n";
      return true;
    }
    Directives.parse((*Buf)->getBuffer());
    return true;
  }();
  (void)Initialized;
  return Directives;
}

//===----------------------------------------------------------------------===//
// The pass.
//===----------------------------------------------------------------------===//

namespace {
class DeduBB : public MachineFunctionPass {
public:
  static char ID;
  DeduBB() : MachineFunctionPass(ID) {
    initializeDeduBBPass(*PassRegistry::getPassRegistry());
  }
  StringRef getPassName() const override {
    return "DeduBB tail-call deduplication";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char DeduBB::ID = 0;
INITIALIZE_PASS(DeduBB, DEBUG_TYPE, "DeduBB tail-call deduplication",
                /*cfg=*/false, /*is_analysis=*/false)

bool DeduBB::runOnMachineFunction(MachineFunction &MF) {
  const DeduBBDirectives &Directives = DeduBBDirectives::get();
  if (Directives.empty())
    return false;

  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  if (!TII->supportsDeduBB())
    return false;

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    std::optional<UniqueBBID> ID = MBB.getBBID();
    // Only original (non-cloned) blocks carry directives.
    if (!ID || ID->CloneID != 0)
      continue;
    const DeduBBDirectives::Directive *D =
        Directives.lookup(MF.getName(), ID->BaseID);
    if (!D) {
      for (const GlobalAlias &A : MF.getFunction().getParent()->aliases()) {
        if (A.getAliaseeObject() == &MF.getFunction()) {
          if ((D = Directives.lookup(A.getName(), ID->BaseID)))
            break;
        }
      }
    }
    if (!D || D->K != DeduBBDirectives::Fold)
      continue;

    // The profiler may have marked blocks as identical even if their relocation targets differ.
    // Refuse to fold any block containing relocation-like operands to prevent bad merges.
    bool HasRelocation = false;
    for (const MachineInstr &MI : MBB) {
      for (const MachineOperand &MO : MI.operands()) {
        if (MO.isGlobal() || MO.isSymbol() || MO.isCPI() || MO.isJTI() || MO.isBlockAddress() || MO.isMCSymbol()) {
          HasRelocation = true;
          break;
        }
      }
      if (HasRelocation) break;
    }
    if (HasRelocation)
      continue;

    // Replace the entire block with a single tail branch to the master copy.
    // Jumping (rather than calling) preserves the current stack frame; the
    // master's identical tail call/return then exits as the original block did.
    std::string MasterSym = ("DeduBB.master." + Twine(D->MasterID)).str();
    MBB.erase(MBB.begin(), MBB.end());
    TII->insertDeduBBTailBranch(MBB, MasterSym);
    while (!MBB.succ_empty())
      MBB.removeSuccessor(MBB.succ_begin());
    Changed = true;
  }
  return Changed;
}

MachineFunctionPass *llvm::createDeduBBPass() { return new DeduBB(); }
