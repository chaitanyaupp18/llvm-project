; AArch64 version of the DeduBB tail-call deduplication test. The fold is
; realized as a TCRETURNdi (tail branch), which the assembler emits as `b`.
;
; NOTE: exact assembly spelling may vary; checks are intentionally loose around
; the symbol reference.

; RUN: printf 'm test.c\nf master_fn\nbbm 0 (DeduBB.master.7)\nf fold_fn\nbbf 0 (DeduBB.master.7)\n' > %t.dir
; RUN: llc -mtriple=aarch64-unknown-linux-gnu -basic-block-address-map \
; RUN:     -dedubb-directives=%t.dir < %s | FileCheck %s

define i32 @master_fn() {
entry:
  ret i32 42
}

; CHECK-LABEL: master_fn:
; CHECK: .globl DeduBB.master.7
; CHECK: DeduBB.master.7:

define i32 @fold_fn() {
entry:
  ret i32 42
}

; CHECK-LABEL: fold_fn:
; CHECK: b DeduBB.master.7
; CHECK-NOT: ret
