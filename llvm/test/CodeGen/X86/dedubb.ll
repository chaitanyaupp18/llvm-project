; Tests the DeduBB tail-call deduplication pass applying bbm/bbf directives.
;   bbm <id> (DeduBB.master.K) -> emit a global `DeduBB.master.K` label at the
;                                 start of the master block.
;   bbf <id> (DeduBB.master.K) -> replace the block with a jump to the master.
; Both functions are single-block (entry block has bb_id 0), so the directives
; are deterministic.
;
; NOTE: exact assembly spelling (e.g. an @PLT suffix under PIC) may vary; the
; checks below are intentionally loose around the symbol reference.

; RUN: printf 'm test.c\nf master_fn\nbbm 0 (DeduBB.master.7)\nf fold_fn\nbbf 0 (DeduBB.master.7)\n' > %t.dir
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -basic-block-address-map \
; RUN:     -dedubb-directives=%t.dir < %s | FileCheck %s

define i32 @master_fn() {
entry:
  ret i32 42
}

; The master block gets a global symbol so other modules can branch to it.
; CHECK-LABEL: master_fn:
; CHECK: .globl DeduBB.master.7
; CHECK: DeduBB.master.7:

define i32 @fold_fn() {
entry:
  ret i32 42
}

; The folded block is replaced by a single jump to the master copy; its original
; `ret` is gone.
; CHECK-LABEL: fold_fn:
; CHECK: jmp DeduBB.master.7
; CHECK-NOT: retq
