//===- llvm/CodeGen/DeduBBDirectives.h - DeduBB directive reader -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reads the DeduBB tail-call deduplication directive file produced by Propeller
// (the `--tail_call_profile=dedubb.txt` output) and answers per-block queries
// used by the DeduBB CodeGen pass and the AsmPrinter.
//
// The directive file groups directives by module and function:
//
//     m <module>
//     f <function>
//     bbm <bb_id> (DeduBB.master.<K>)   ; this block is the master copy K
//     bbf <bb_id> (DeduBB.master.<K>)   ; fold this block into master K
//
// `bbm` marks a basic block as the master copy: the compiler emits a global
// symbol `DeduBB.master.<K>` at the start of the block so that duplicates in
// this and other modules can branch to it. `bbf` marks a byte-identical
// duplicate that ends in a tail call or return: the compiler replaces the whole
// block with a single jump to `DeduBB.master.<K>`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_DEDUBBDIRECTIVES_H
#define LLVM_CODEGEN_DEDUBBDIRECTIVES_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>

namespace llvm {

class DeduBBDirectives {
public:
  enum Kind { Master, Fold };

  struct Directive {
    Kind K;
    uint64_t MasterID; // K in `DeduBB.master.K`.
  };

  // Parses directive text (the contents of a dedubb.txt file). Lines starting
  // with '#' are comments. Returns false and reports to errs() on a malformed
  // line, leaving already-parsed directives in place.
  LLVM_ABI bool parse(StringRef Text);

  // Returns the directive for basic block `BBID` of function `FuncName`, or
  // nullptr if the block has no directive.
  LLVM_ABI const Directive *lookup(StringRef FuncName, unsigned BBID) const;

  bool empty() const { return Map.empty(); }

  // Returns the process-wide directives, lazily parsed from the file named by
  // the `-dedubb-directives` command-line option (empty if the option is
  // unset). Shared by the DeduBB pass and the AsmPrinter.
  LLVM_ABI static const DeduBBDirectives &get();

private:
  // Function name -> (basic block id -> directive).
  StringMap<DenseMap<unsigned, Directive>> Map;
};

} // namespace llvm

#endif // LLVM_CODEGEN_DEDUBBDIRECTIVES_H
