; tools/air/sample3d.ll's second kernel: the same texture3d and the same samplers in pixel
; coordinates, sampled with TEXEL OFFSETS, so that an offset can be checked by value.
;
; WHY a kernel of its own: MetalFaceMask_ErodeCube{Luma,Chroma1,Chroma2} come back
; different from Gallium on the Vulkan back end (t1 891419c8 on Gallium, a8786e22 on vk),
; and Erode samples at offsets -- ErodeCubeLuma at 0, <-1,0,0> and <1,0,0>, ErodeCubeChroma1
; at 0 and +-1 on y and on z, ErodeCubeChroma2 at a computed one, its loop variable -- but
; its result is a product of halves of air_run's texels, 100 and more, so it cannot say
; whether an offset was applied, ignored or applied wrongly on air_vk.c's normalised 3D path
; (pixel_to_normalized:
; "the offsets ... are in texels in either space, applied by the texture unit after the
; coordinate is scaled, so they stay as they are"). sample3d.ll samples with none. Here is
; one of each kind, in single precision and on air_metal_test's small texels. A module has
; ONE entry point (air_to_nir takes the first !air.kernel and refuses another name), and
; sample3d's four slots already fill air_metal_test's 4096-byte buffer, so this is a second
; module and not two more slots:
;
;     kernel void sample3d_off(texture3d<float, access::sample> t [[texture(0)]],
;                              device float4 *out                 [[buffer(0)]],
;                              uint3 gid                          [[thread_position_in_grid]])
;     {
;         constexpr sampler near(coord::pixel, address::clamp_to_edge, filter::nearest);
;         constexpr sampler lin (coord::pixel, address::clamp_to_edge, filter::linear);
;         float3 p = float3(gid);
;         int3 d = 1 - 2 * int3(gid & 1);                   // +1 on an even axis, -1 on an odd
;         device float4 *o = out + 4 * (gid.x + 4 * gid.y + 16 * gid.z);
;         o[0] = t.sample(near, p + 0.5, int3(-1, 0, 0));   // Erode's own constant offset
;         o[1] = t.sample(near, p + 0.5, int3(1, 0, 1));    // ... and one on r too
;         o[2] = t.sample(lin,  p + 1.0, int3(-1, 1, 1));   // through the linear sampler
;         o[3] = t.sample(near, p + 0.5, d);                // computed, as Chroma2's is
;     }
;
; Metal's rule (and Vulkan's, OpImageSample*'s ConstOffset/Offset): the offset is in texels
; and is added to the texel-space position before each tap is addressed, so with
; clamp_to_edge an offset past the edge reads the edge texel. The samplers are sample3d's
; words, 0x807bff0000088049 and 0x807bff0000088a49; the call is ErodeCubeLuma's own --
; `i1 true, <3 x i32> <i32 -1, i32 0, i32 0>` before the level pair -- with v4f32 for v4f16,
; and the computed offset is a <3 x i32> SSA value in the same operand, as Chroma2's phi is.
;
; Seeded and dispatched as sample3d is, texel (x, y, z) = (x + 4y + 16z, 1, 0, 1):
;
;     /hackios/bin/air_metal_test_gl --gpu /hackios/share/probes/sample3d_off.bc sample3d_off 125:7 0xa5 1x1x1 4x4x4 index
;     /hackios/bin/air_metal_test_vk --gpu /hackios/share/probes/sample3d_off.bc sample3d_off 125:7 0xa5 1x1x1 4x4x4 index
;
; and the result line of both has to read
;
;     air_metal_test: compute-result BACKEND sample3d_off ran b0 caf0c000f2f22a7c
;
; FNV-1a over the 1024 words computed on the host from the rule above. Thread (x, y, z)
; writes, with c(a) = min(max(a, 0), 3) and every R exact in binary32 and in half:
;     o[0] = texel (c(x - 1), y, z)                      thread (0,0,0): 0 1 0 1
;     o[1] = texel (c(x + 1), y, c(z + 1))               thread (0,0,0): 17 1 0 1
;     o[2] = the linear sample at (x, y + 2, z + 2)      thread (0,0,0): 30 1 0 1
;     o[3] = texel (x ^ 1, y ^ 1, z ^ 1)                 thread (0,0,0): 21 1 0 1
; Each way of being wrong, through the same model, and the threads it changes in each of
; the four slots (of 64):
;     offset ignored                                     32bb83682c3a4dbe  48 60 64 64
;     offset's sign flipped                              7bfc02bf67f8b3e4  64 64 64 64
;     offset in normalised units (x 1/size twice: o/4)   94242edf56ddc40c  48 60 64 64
;     offset added to the normalised coordinate (4o)     be518c06b39c2c25  32 48 60 56
;     offset's r dropped (a 2D offset)                   09b55609cd7ec601   0 48 48 64
;     the computed offset ignored, the constants not     f1a1113f05563fcc   0  0  0 64
; (o/4 leaves the two nearest constant slots where an ignored offset leaves them -- a
; quarter texel from the centre is still the same texel -- and the linear slot and the
; digest tell the two apart.)
;
;   llvm-as-18 -o userland/obj/sample3d_off.bc tools/air/sample3d_off.ll

source_filename = "sample3d_off"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

; near: sampler(coord::pixel), 0x807bff0000088049
@__air_sampler_state = internal addrspace(2) constant i64 -9188470239253725111, align 8
; lin: sampler(coord::pixel, filter::linear), 0x807bff0000088a49
@__air_sampler_state.1 = internal addrspace(2) constant i64 -9188470239253722551, align 8

declare { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <3 x float>, i1, <3 x i32>, i1, float, float, i32) local_unnamed_addr

declare <3 x float> @air.convert.f.v3f32.u.v3i32(<3 x i32>) local_unnamed_addr

define void @sample3d_off(ptr addrspace(1) nocapture readonly %t, ptr addrspace(1) noalias nocapture %out,
                          <3 x i32> %gid) local_unnamed_addr {
  %p = tail call <3 x float> @air.convert.f.v3f32.u.v3i32(<3 x i32> %gid)
  %centre = fadd <3 x float> %p, <float 5.000000e-01, float 5.000000e-01, float 5.000000e-01>
  %between = fadd <3 x float> %p, <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>

  ; the computed offset: 1 - 2 * (gid & 1), +1 on an even axis and -1 on an odd one
  %par = and <3 x i32> %gid, <i32 1, i32 1, i32 1>
  %par2 = shl <3 x i32> %par, <i32 1, i32 1, i32 1>
  %d = sub <3 x i32> <i32 1, i32 1, i32 1>, %par2

  %s0 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <3 x float> %centre, i1 true, <3 x i32> <i32 -1, i32 0, i32 0>, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v0 = extractvalue { <4 x float>, i8 } %s0, 0
  %s1 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <3 x float> %centre, i1 true, <3 x i32> <i32 1, i32 0, i32 1>, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %s1, 0
  %s2 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state.1, <3 x float> %between, i1 true, <3 x i32> <i32 -1, i32 1, i32 1>, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %s2, 0
  %s3 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <3 x float> %centre, i1 true, <3 x i32> %d, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %s3, 0

  ; where this thread's four float4s go: out + 4 * (x + 4y + 16z)
  %gx = extractelement <3 x i32> %gid, i64 0
  %gy = extractelement <3 x i32> %gid, i64 1
  %gz = extractelement <3 x i32> %gid, i64 2
  %y4 = shl i32 %gy, 2
  %z16 = shl i32 %gz, 4
  %xy = add i32 %gx, %y4
  %linear = add i32 %xy, %z16
  %base = shl i32 %linear, 2
  %base64 = zext i32 %base to i64
  %o0 = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %base64
  store <4 x float> %v0, ptr addrspace(1) %o0, align 16
  %o1 = getelementptr inbounds <4 x float>, ptr addrspace(1) %o0, i64 1
  store <4 x float> %v1, ptr addrspace(1) %o1, align 16
  %o2 = getelementptr inbounds <4 x float>, ptr addrspace(1) %o0, i64 2
  store <4 x float> %v2, ptr addrspace(1) %o2, align 16
  %o3 = getelementptr inbounds <4 x float>, ptr addrspace(1) %o0, i64 3
  store <4 x float> %v3, ptr addrspace(1) %o3, align 16
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}
!air.sampler_states = !{!8, !9}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @sample3d_off, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture3d<float, sample>", !"air.arg_name", !"t"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
!8 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
!9 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.1}
