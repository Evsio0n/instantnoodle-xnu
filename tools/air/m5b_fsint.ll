; M5b: int4(x - 8, y - 8, x + y - 16, -1) of the pixel, into an integer attachment.;
; The buffer, the same in every m5b_ module, so one C struct (struct pix_u in air_metal_test.c)
; fills them all: u[0] u[1] u[2] float4s, u[3] = (lod, bias, ref, 0), and at offset 64 a uint index
; and a uint select. A plain coordinate is pos.xy * u[0].xy + u[0].zw.
;
; The call and the metadata are the corpus's (texturedFragment2DFloat, llvm-dis of its module) where
; the corpus has one (2D, 3D, depth2d, and the _ms read); for the kinds no system module uses (1D,
; arrays, cubes) they are the argument order air_intrinsics.c reads, which it took from Apple's
; metal_texture header. Written by hand-sized generator, one straight line of code each.

source_filename = "m5b_fsint"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x i32> }> @m5b_fsint(<4 x float> %pos) local_unnamed_addr {
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %ix = fptoui float %x to i32
  %iy = fptoui float %y to i32
  %xy = add i32 %ix, %iy
  %a = add i32 %ix, -8
  %b = add i32 %iy, -8
  %d = add i32 %xy, -16
  %e0 = insertelement <4 x i32> undef, i32 %a, i64 0
  %e1 = insertelement <4 x i32> %e0, i32 %b, i64 1
  %e2 = insertelement <4 x i32> %e1, i32 %d, i64 2
  %e = insertelement <4 x i32> %e2, i32 -1, i64 3
  %ret = insertvalue <{ <4 x i32> }> undef, <4 x i32> %e, 0
  ret <{ <4 x i32> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5b_fsint, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"int4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
