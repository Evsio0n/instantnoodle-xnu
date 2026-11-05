; M5b: a texture2d<float> read at (x, y), level u.index. No sampler, no filter: the texel.;
; The buffer, the same in every m5b_ module, so one C struct (struct pix_u in air_metal_test.c)
; fills them all: u[0] u[1] u[2] float4s, u[3] = (lod, bias, ref, 0), and at offset 64 a uint index
; and a uint select. A plain coordinate is pos.xy * u[0].xy + u[0].zw.
;
; The call and the metadata are the corpus's (texturedFragment2DFloat, llvm-dis of its module) where
; the corpus has one (2D, 3D, depth2d, and the _ms read); for the kinds no system module uses (1D,
; arrays, cubes) they are the argument order air_intrinsics.c reads, which it took from Apple's
; metal_texture header. Written by hand-sized generator, one straight line of code each.

source_filename = "m5b_fread"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, <2 x i32>, i32, i32) local_unnamed_addr

define <{ <4 x float> }> @m5b_fread(<4 x float> %pos, ptr addrspace(2) nocapture readonly %u, ptr addrspace(1) nocapture readonly %t) local_unnamed_addr {
  %pu0 = getelementptr inbounds <4 x float>, ptr addrspace(2) %u, i64 0
  %u0 = load <4 x float>, ptr addrspace(2) %pu0, align 16
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %ix = fptoui float %x to i32
  %iy = fptoui float %y to i32
  %c0 = insertelement <2 x i32> undef, i32 %ix, i64 0
  %c = insertelement <2 x i32> %c0, i32 %iy, i64 1
  %pi = getelementptr inbounds <4 x float>, ptr addrspace(2) %u, i64 4
  %idx = load i32, ptr addrspace(2) %pi, align 4
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %t, <2 x i32> %c, i32 %idx, i32 0)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %ret = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5b_fread, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"u"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"t"}
