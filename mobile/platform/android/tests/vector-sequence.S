/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone ARM64 instruction-sequence diagnostic. Never linked into the
 * application or engine. See android/NATIVE_ENGINE_TESTS.md. */
.text
.p2align 2
.global vector_sequence
.type vector_sequence,%function
vector_sequence:
  stp x19,x30,[sp,#-16]!
  mov x19,x0
  mov x8,#65535
  mov x12,#0
// Preserve this contiguous sequence from an NDK 27.2 vectorized size_t loop.
// Adding bounds checks inside it changes the emulator translation being tested.
// Constants are the original uint64 pairs {0,1} and {2,3}.
  add x8,x8,#1
  mov x10,x12
  sub x9,x8,x12
  cmp x9,#8
  b.lo .Lscalar
  dup v1.2d,x12
  adrp x10,.LC23
  adrp x11,.LC01
  ldr q0,[x10,:lo12:.LC23]
  ldr q2,[x11,:lo12:.LC01]
  mov w10,#4
  mov w11,#8
  add x13,x19,x12,lsl#2
  add v0.2d,v1.2d,v0.2d
  add v1.2d,v1.2d,v2.2d
  dup v2.2d,x10
  mov w10,#0x3ac80000
  dup v4.2d,x11
  and x11,x9,#0xfffffffffffffff8
  dup v3.4s,w10
  add x10,x12,x11
  add x12,x13,#0x10
  mov x13,x11
.Lvector:
  add v5.2d,v1.2d,v2.2d
  ucvtf v6.2d,v1.2d
  subs x13,x13,#8
  add v7.2d,v0.2d,v2.2d
  ucvtf v16.2d,v0.2d
  add v0.2d,v0.2d,v4.2d
  add v1.2d,v1.2d,v4.2d
  ucvtf v5.2d,v5.2d
  fcvtn v6.2s,v6.2d
  ucvtf v7.2d,v7.2d
  fcvtn v5.2s,v5.2d
  fcvtn2 v6.4s,v16.2d
  fcvtn2 v5.4s,v7.2d
  fmul v6.4s,v6.4s,v3.4s
  fmul v5.4s,v5.4s,v3.4s
  stp q6,q5,[x12,#-0x10]
  add x12,x12,#0x20
  b.ne .Lvector
  cmp x9,x11
  b.eq .Ldone
.Lscalar:
  mov w9,#0x3ac80000
  fmov s0,w9
.Lscalar_loop:
  ucvtf s1,x10
  fmul s1,s1,s0
  str s1,[x19,x10,lsl#2]
  add x10,x10,#1
  cmp x8,x10
  b.ne .Lscalar_loop
.Ldone:
  ldp x19,x30,[sp],#16
  ret
.size vector_sequence,.-vector_sequence
.section .rodata
.p2align 4
.LC01: .xword 0,1
.LC23: .xword 2,3
.section .note.GNU-stack,"",%progbits
