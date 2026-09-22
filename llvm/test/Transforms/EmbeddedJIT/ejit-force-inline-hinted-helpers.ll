; UNSUPPORTED: asserts
; RUN: rm -rf %t.off %t.on && mkdir -p %t.off %t.on
; RUN: opt -passes=ejit-register-bitcode -ejit-externalize-min-insts=10000 -ejit-dump-bitcode-dir=%t.off -S %s -o %t.aot.off.ll
; RUN: opt -passes=ejit-register-bitcode -ejit-externalize-min-insts=10000 -ejit-force-inline-hinted-helpers -ejit-dump-bitcode-dir=%t.on -S %s -o %t.aot.on.ll
; RUN: opt -S %t.off/*.bc | FileCheck %s --check-prefix=OFF
; RUN: opt -S %t.on/*.bc | FileCheck %s --check-prefix=ON
; RUN: FileCheck %s --check-prefix=AOT < %t.aot.on.ll

; AOT-LABEL: define i32 @root(
; AOT: call i32 @hint_large(
; AOT-LABEL: define internal i32 @hint_large(
; AOT-SAME: #[[HINT:[0-9]+]]
; AOT: attributes #[[HINT]] = { cold inlinehint }

; OFF-LABEL: define i32 @root(
; OFF: call i32 @hint_large(
; ON-LABEL: define i32 @root(
; ON-NOT: call i32 @hint_large(
; ON: call i32 @unhinted(
; ON: call i32 @noinline_helper(
; ON: call i32 @entry_helper(
; ON: call i32 @lifecycle_helper(
; ON: call i32 @recursive_helper(
; ON: call i32 @decl_only(

declare i32 @opaque(i32)
declare i32 @decl_only(i32) #0

define i32 @root(i32 %x) !ejit.metadata !0 {
entry:
  %a = call i32 @hint_large(i32 %x)
  %b = call i32 @unhinted(i32 %a)
  %c = call i32 @noinline_helper(i32 %b)
  %d = call i32 @entry_helper(i32 %c)
  %e = call i32 @lifecycle_helper(i32 %d)
  %f = call i32 @recursive_helper(i32 %e)
  %g = call i32 @decl_only(i32 %f)
  ret i32 %g
}

define internal i32 @hint_large(i32 %v0) #4 {
entry:
  %v1 = call i32 @opaque(i32 %v0)
  %v2 = call i32 @opaque(i32 %v1)
  %v3 = call i32 @opaque(i32 %v2)
  %v4 = call i32 @opaque(i32 %v3)
  %v5 = call i32 @opaque(i32 %v4)
  %v6 = call i32 @opaque(i32 %v5)
  %v7 = call i32 @opaque(i32 %v6)
  %v8 = call i32 @opaque(i32 %v7)
  %v9 = call i32 @opaque(i32 %v8)
  %v10 = call i32 @opaque(i32 %v9)
  %v11 = call i32 @opaque(i32 %v10)
  %v12 = call i32 @opaque(i32 %v11)
  %v13 = call i32 @opaque(i32 %v12)
  %v14 = call i32 @opaque(i32 %v13)
  %v15 = call i32 @opaque(i32 %v14)
  %v16 = call i32 @opaque(i32 %v15)
  %v17 = call i32 @opaque(i32 %v16)
  %v18 = call i32 @opaque(i32 %v17)
  %v19 = call i32 @opaque(i32 %v18)
  %v20 = call i32 @opaque(i32 %v19)
  %v21 = call i32 @opaque(i32 %v20)
  %v22 = call i32 @opaque(i32 %v21)
  %v23 = call i32 @opaque(i32 %v22)
  %v24 = call i32 @opaque(i32 %v23)
  %v25 = call i32 @opaque(i32 %v24)
  %v26 = call i32 @opaque(i32 %v25)
  %v27 = call i32 @opaque(i32 %v26)
  %v28 = call i32 @opaque(i32 %v27)
  %v29 = call i32 @opaque(i32 %v28)
  %v30 = call i32 @opaque(i32 %v29)
  %v31 = call i32 @opaque(i32 %v30)
  %v32 = call i32 @opaque(i32 %v31)
  %v33 = call i32 @opaque(i32 %v32)
  %v34 = call i32 @opaque(i32 %v33)
  %v35 = call i32 @opaque(i32 %v34)
  %v36 = call i32 @opaque(i32 %v35)
  %v37 = call i32 @opaque(i32 %v36)
  %v38 = call i32 @opaque(i32 %v37)
  %v39 = call i32 @opaque(i32 %v38)
  %v40 = call i32 @opaque(i32 %v39)
  %v41 = call i32 @opaque(i32 %v40)
  %v42 = call i32 @opaque(i32 %v41)
  %v43 = call i32 @opaque(i32 %v42)
  %v44 = call i32 @opaque(i32 %v43)
  %v45 = call i32 @opaque(i32 %v44)
  %v46 = call i32 @opaque(i32 %v45)
  %v47 = call i32 @opaque(i32 %v46)
  %v48 = call i32 @opaque(i32 %v47)
  %v49 = call i32 @opaque(i32 %v48)
  %v50 = call i32 @opaque(i32 %v49)
  %v51 = call i32 @opaque(i32 %v50)
  %v52 = call i32 @opaque(i32 %v51)
  %v53 = call i32 @opaque(i32 %v52)
  %v54 = call i32 @opaque(i32 %v53)
  %v55 = call i32 @opaque(i32 %v54)
  %v56 = call i32 @opaque(i32 %v55)
  %v57 = call i32 @opaque(i32 %v56)
  %v58 = call i32 @opaque(i32 %v57)
  %v59 = call i32 @opaque(i32 %v58)
  %v60 = call i32 @opaque(i32 %v59)
  %v61 = call i32 @opaque(i32 %v60)
  %v62 = call i32 @opaque(i32 %v61)
  %v63 = call i32 @opaque(i32 %v62)
  %v64 = call i32 @opaque(i32 %v63)
  %v65 = call i32 @opaque(i32 %v64)
  %v66 = call i32 @opaque(i32 %v65)
  %v67 = call i32 @opaque(i32 %v66)
  %v68 = call i32 @opaque(i32 %v67)
  %v69 = call i32 @opaque(i32 %v68)
  %v70 = call i32 @opaque(i32 %v69)
  %v71 = call i32 @opaque(i32 %v70)
  %v72 = call i32 @opaque(i32 %v71)
  %v73 = call i32 @opaque(i32 %v72)
  %v74 = call i32 @opaque(i32 %v73)
  %v75 = call i32 @opaque(i32 %v74)
  %v76 = call i32 @opaque(i32 %v75)
  %v77 = call i32 @opaque(i32 %v76)
  %v78 = call i32 @opaque(i32 %v77)
  %v79 = call i32 @opaque(i32 %v78)
  %v80 = call i32 @opaque(i32 %v79)
  ret i32 %v80
}

define internal i32 @unhinted(i32 %x) #1 {
  %r = add i32 %x, 1
  ret i32 %r
}

define internal i32 @noinline_helper(i32 %x) #2 {
  %r = add i32 %x, 2
  ret i32 %r
}

define internal i32 @entry_helper(i32 %x) #3 !ejit.metadata !0 {
  %r = add i32 %x, 3
  ret i32 %r
}

define internal i32 @lifecycle_helper(i32 %x) #3 !ejit.metadata !1 {
  %r = add i32 %x, 4
  ret i32 %r
}

define internal i32 @recursive_helper(i32 %x) #0 {
entry:
  %done = icmp eq i32 %x, 0
  br i1 %done, label %ret, label %recur
recur:
  %next = sub i32 %x, 1
  %r = call i32 @recursive_helper(i32 %next)
  br label %ret
ret:
  %out = phi i32 [ 0, %entry ], [ %r, %recur ]
  ret i32 %out
}

attributes #0 = { inlinehint }
attributes #1 = { noinline }
attributes #2 = { inlinehint noinline }
attributes #3 = { inlinehint noinline }
attributes #4 = { cold inlinehint }

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_period_lc", !"cell"}}
