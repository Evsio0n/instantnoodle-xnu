; M5b: the shader's own samplers with a border colour, a module of its own because the Gallium library
; refuses the word by name and the rest of the module with it:
;   0 clamp_to_border, transparent black   1 opaque black   2 opaque white;
; The buffer, the same in every m5b_ module, so one C struct (struct pix_u in air_metal_test.c)
; fills them all: u[0] u[1] u[2] float4s, u[3] = (lod, bias, ref, 0), and at offset 64 a uint index
; and a uint select. A plain coordinate is pos.xy * u[0].xy + u[0].zw.
;
; The call and the metadata are the corpus's (texturedFragment2DFloat, llvm-dis of its module) where
; the corpus has one (2D, 3D, depth2d, and the _ms read); for the kinds no system module uses (1D,
; arrays, cubes) they are the argument order air_intrinsics.c reads, which it took from Apple's
; metal_texture header. Written by hand-sized generator, one straight line of code each.

source_filename = "m5b_fownb"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

; border transparent black: 0x807bff0000080124
@__air_sampler_state = internal addrspace(2) constant i64 -9188470239253757660, align 8
; border opaque black: 0x817bff0000080124
@__air_sampler_state.1 = internal addrspace(2) constant i64 -9116412645215829724, align 8
; border opaque white: 0x827bff0000080124
@__air_sampler_state.2 = internal addrspace(2) constant i64 -9044355051177901788, align 8

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr

define <{ <4 x float> }> @m5b_fownb(<4 x float> %pos, ptr addrspace(2) nocapture readonly %u, ptr addrspace(1) nocapture readonly %t) local_unnamed_addr {
  %pu0 = getelementptr inbounds <4 x float>, ptr addrspace(2) %u, i64 0
  %u0 = load <4 x float>, ptr addrspace(2) %pu0, align 16
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %sx = extractelement <4 x float> %u0, i64 0
  %sy = extractelement <4 x float> %u0, i64 1
  %ox = extractelement <4 x float> %u0, i64 2
  %oy = extractelement <4 x float> %u0, i64 3
  %cx0 = fmul float %x, %sx
  %cx = fadd float %cx0, %ox
  %cy0 = fmul float %y, %sy
  %cy = fadd float %cy0, %oy
  %c0 = insertelement <2 x float> undef, float %cx, i64 0
  %c = insertelement <2 x float> %c0, float %cy, i64 1
  %pu3 = getelementptr inbounds <4 x float>, ptr addrspace(2) %u, i64 3
  %u3 = load <4 x float>, ptr addrspace(2) %pu3, align 16
  %lod = extractelement <4 x float> %u3, i64 0
  %bias = extractelement <4 x float> %u3, i64 1
  %ref = extractelement <4 x float> %u3, i64 2
  %ps4 = getelementptr inbounds <4 x float>, ptr addrspace(2) %u, i64 4
  %ps = getelementptr inbounds i32, ptr addrspace(2) %ps4, i64 1
  %sel = load i32, ptr addrspace(2) %ps, align 4
  %r0 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <2 x float> %c, i1 true, <2 x i32> zeroinitializer, i1 true, float %lod, float 0.000000e+00, i32 0)
  %v0 = extractvalue { <4 x float>, i8 } %r0, 0
  %r1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state.1, <2 x float> %c, i1 true, <2 x i32> zeroinitializer, i1 true, float %lod, float 0.000000e+00, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %r1, 0
  %is1 = icmp eq i32 %sel, 1
  %m1 = select i1 %is1, <4 x float> %v1, <4 x float> %v0
  %r2 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state.2, <2 x float> %c, i1 true, <2 x i32> zeroinitializer, i1 true, float %lod, float 0.000000e+00, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %r2, 0
  %is2 = icmp eq i32 %sel, 2
  %m2 = select i1 %is2, <4 x float> %v2, <4 x float> %m1
  %ret = insertvalue <{ <4 x float> }> undef, <4 x float> %m2, 0
  ret <{ <4 x float> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}
!air.sampler_states = !{!9, !10, !11}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5b_fownb, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6, !7, !8}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!7 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"u"}
!8 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!9 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
!10 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.1}
!11 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.2}
