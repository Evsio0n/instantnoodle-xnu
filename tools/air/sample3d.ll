; A kernel that samples a texture3d through its own samplers in pixel coordinates, and
; writes every sample, so that a 3D sample in pixel coordinates can be checked by value.
;
; WHY it is hand-written and not taken from the corpus: the six kernels of the corpus that
; do this -- PortraitFilters' MetalFaceMask_DilateCube{Luma,Chroma1,Chroma2} and
; ErodeCube{Luma,Chroma1,Chroma2}, sampler(coord::pixel) on a texture3d<half> -- cannot
; say whether the right texel was read. air_metal_test seeds every texture with air_run's
; texels, 100 and up: Dilate adds three of them and saturate()s, which is 1.0 whatever was
; read, and Erode's sum overflows half to inf. And the Vulkan back end does not sample
; them the way the Gallium one does: Vulkan has unnormalizedCoordinates on 1D and 2D views
; alone, so tools/mesa/air/air_vk.c makes such a sampler a NORMALISED one and multiplies
; every coordinate sampled through it by 1 / textureSize in NIR (pixel_to_normalized,
; header section "3D TEXTURES IN PIXEL COORDINATES": no half-texel offset, r must clamp).
; A scale that is missing, a half texel too many or too few, an r that wraps, a filter or
; an address mode that is lost on the way -- each of these leaves the corpus's six results
; as they were. This kernel's output changes under every one of them:
;
;     kernel void sample3d(texture3d<float, access::sample> t [[texture(0)]],
;                          device float4 *out                 [[buffer(0)]],
;                          uint3 gid                          [[thread_position_in_grid]])
;     {
;         constexpr sampler near(coord::pixel, address::clamp_to_edge, filter::nearest);
;         constexpr sampler lin (coord::pixel, address::clamp_to_edge, filter::linear);
;         constexpr sampler zero(coord::pixel, address::clamp_to_zero, filter::nearest);
;         float3 p = float3(gid);
;         // a point outside the texture: below it on r in an even row, above it on x in
;         // an odd one, and inside on the other two axes (off the centre on one of them)
;         float3 q = (gid.y & 1) ? p + float3(4.5, 0.5, 0.25)
;                                : p * float3(1, 1, -1) + float3(0.25, 0.5, -0.5);
;         device float4 *o = out + 4 * (gid.x + 4 * gid.y + 16 * gid.z);
;         o[0] = t.sample(near, p + 0.5);   // the texel's centre: exactly texel gid
;         o[1] = t.sample(lin,  p + 1.0);   // between texels on all three axes: 8 taps
;         o[2] = t.sample(near, q);         // outside: the edge texel
;         o[3] = t.sample(zero, q);         // outside: transparent black
;     }
;
; Metal's pixel coordinates (MSL, and tools/air/sampcheck.py, which checks them in 2D):
; texel i spans [i, i + 1) and is centred at i + 0.5; nearest picks floor(u); linear
; weights frac(u - 0.5), so the integer coordinate gid + 1 is exactly halfway between
; texels gid and gid + 1 on each axis and the result is the mean of eight texels;
; clamp_to_edge reads the edge texel outside [0, 4), clamp_to_zero reads (0, 0, 0, 0).
;
; THE SAMPLERS are the corpus's own words, as plan_globals and air_decode_sampler_state
; (air_to_nir.c) read them; three globals named __air_sampler_state*, listed in
; !air.sampler_states, get inline samplers 0, 1 and 2 in this order:
;     near 0x807bff0000088049  sampler(coord::pixel): Dilate/Erode's word, eight of the
;                              ten corpus modules that carry one
;     lin  0x807bff0000088a49  sampler(coord::pixel, filter::linear): in the corpus
;     zero 0x807bff0000088000  sampler(coord::pixel, address::clamp_to_zero): the corpus's
;                              0x807bff0000088a00 (the same with filter::linear) with
;                              the four filter bits cleared, and nothing else changed
; All three sample a texture3d, so air_vk.c makes all three normalised samplers.
; The call is copied from MetalFaceMask_DilateCubeLuma (llvm-dis of its module out of
; PortraitFilters.cifilter/default.metallib), with v4f32 for v4f16 -- the form
; AppleMetalGLRenderer's texturedFragment3DFloat declares -- and the texture's
; air.arg_type_name is that shader's "texture3d<float, sample>".
;
; THE TEXTURE has to be small, distinct per texel and exact under any blend, so
; air_metal_test's seventh argument TEXELS = index seeds it with
;     texel (x, y, z) = (x + 4y + 16z, 1, 0, 1)        (RGBA32Float, 4x4x4)
; R names the texel, and every blend here is a multiple of 0.5 below 64: exact in binary32
; and in half, so a texture unit that filters a 32-bit float at reduced precision gives the
; same number. G = 1 says the weights summed to one; A = 1 inside and 0 at the border says
; clamp_to_zero apart from clamp_to_edge and from an opaque-black border.
;
; Dispatched as one group of 4x4x4 threads it writes all 4096 bytes of buffer 0 -- 64
; threads, four float4s each -- so the fill does not reach the result (0xa5, so a slot
; left unwritten is not the zero slot 3 expects):
;
;     /hackios/bin/air_metal_test_gl --gpu /hackios/share/probes/sample3d.bc sample3d 125:7 0xa5 1x1x1 4x4x4 index
;     /hackios/bin/air_metal_test_vk --gpu /hackios/share/probes/sample3d.bc sample3d 125:7 0xa5 1x1x1 4x4x4 index
;
; and the result line of both has to read
;
;     air_metal_test: compute-result BACKEND sample3d ran b0 064eaea90e929282
;
; which is FNV-1a over the 1024 words computed from the rules above on the host. Thread
; (x, y, z) = gid writes, with i = x + 4y + 16z and m(a) = a + 0.5 below 3 and 3 at 3:
;     o[0] = (i, 1, 0, 1)
;     o[1] = (m(x) + 4 m(y) + 16 m(z), 1, 0, 1)      thread (0,0,0): 10.5 1 0 1
;     o[2] = (x + 4y, 1, 0, 1) in an even row, (3 + 4y + 16z, 1, 0, 1) in an odd one
;     o[3] = (0, 0, 0, 0)
; Each way of being wrong, through the same model, and the threads it changes in each of
; the four slots (of 64):
;     no scale (pixels handed to a normalised sampler)   aea7ed8ca1baec45  63 63 60  0
;     no scale on r alone (a depth of 1)                 5b812023aed1534d  48 48 24  0
;     scaled twice                                       4c61dc49e7c6b3e5  63 64 60 32
;     half a texel added, (p + 0.5) / size               ce6d30a991d0735d  63 63 48  8
;     half a texel taken away, (p - 0.5) / size          4e3c48ebf8d46c55   0 63 48  0
;     r repeats (addressModeW REPEAT)                    1857725349c1c32c   0 16 24 32
;     r mirrors                                          93c124207e0e49c6   0  0 24 32
;     linear served as nearest                           bc5a17bd8cce7720   0 63  0  0
;     nearest served as linear                           ccdb67fefd302cfa   0  0 48  0
;     clamp_to_zero served as clamp_to_edge              13f7b30cad4c5962   0  0  0 64
;     ... on r alone / on s alone                        526a84338ba4b626 / e5bb09c22d46f8c6
;     border opaque black / opaque white                 c948b9fe1258bee2 / 3cef2c1d91702702
; (a half texel taken away leaves o[0] alone: the centre moves onto the integer below it,
; which nearest still floors to the same texel -- o[1] and o[2] are the ones that see it).
; The expected values come from Metal's rules and not from either back end, so a Gallium
; result that differs is a finding about the Gallium path too.
;
; No sample here carries a texel offset; tools/air/sample3d_off.ll is the same texture and
; samplers with four (constant on x, constant on x and r, through the linear sampler, and
; computed per thread), in a module of its own because a module has one entry point and
; these four slots fill the buffer.
;
; Written here rather than generated because it is one function of straight-line code;
; the metadata keys are the AIR interface (docs/air.md section 3) and nothing in this file
; comes out of Apple's libraries but the three sampler words and the call's shape, which
; are the interface itself.
;
;   llvm-as-18 -o userland/obj/sample3d.bc tools/air/sample3d.ll

source_filename = "sample3d"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

; near: sampler(coord::pixel), 0x807bff0000088049
@__air_sampler_state = internal addrspace(2) constant i64 -9188470239253725111, align 8
; lin: sampler(coord::pixel, filter::linear), 0x807bff0000088a49
@__air_sampler_state.1 = internal addrspace(2) constant i64 -9188470239253722551, align 8
; zero: sampler(coord::pixel, address::clamp_to_zero), 0x807bff0000088000
@__air_sampler_state.2 = internal addrspace(2) constant i64 -9188470239253725184, align 8

declare { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <3 x float>, i1, <3 x i32>, i1, float, float, i32) local_unnamed_addr

declare <3 x float> @air.convert.f.v3f32.u.v3i32(<3 x i32>) local_unnamed_addr

define void @sample3d(ptr addrspace(1) nocapture readonly %t, ptr addrspace(1) noalias nocapture %out,
                      <3 x i32> %gid) local_unnamed_addr {
  %p = tail call <3 x float> @air.convert.f.v3f32.u.v3i32(<3 x i32> %gid)

  ; the texel's centre, and the point halfway to the next texel on all three axes
  %centre = fadd <3 x float> %p, <float 5.000000e-01, float 5.000000e-01, float 5.000000e-01>
  %between = fadd <3 x float> %p, <float 1.000000e+00, float 1.000000e+00, float 1.000000e+00>

  ; the point outside: (x + 0.25, y + 0.5, -0.5 - z) in an even row, (x + 4.5, y + 0.5,
  ; z + 0.25) in an odd one
  %pr = fmul <3 x float> %p, <float 1.000000e+00, float 1.000000e+00, float -1.000000e+00>
  %below_r = fadd <3 x float> %pr, <float 2.500000e-01, float 5.000000e-01, float -5.000000e-01>
  %above_x = fadd <3 x float> %p, <float 4.500000e+00, float 5.000000e-01, float 2.500000e-01>
  %gy = extractelement <3 x i32> %gid, i64 1
  %gy1 = and i32 %gy, 1
  %odd = icmp ne i32 %gy1, 0
  %q = select i1 %odd, <3 x float> %above_x, <3 x float> %below_r

  ; the four samples
  %s0 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <3 x float> %centre, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v0 = extractvalue { <4 x float>, i8 } %s0, 0
  %s1 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state.1, <3 x float> %between, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %s1, 0
  %s2 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state, <3 x float> %q, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %s2, 0
  %s3 = tail call { <4 x float>, i8 } @air.sample_texture_3d.v4f32(ptr addrspace(1) nocapture readonly %t, ptr addrspace(2) nocapture readonly @__air_sampler_state.2, <3 x float> %q, i1 true, <3 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %s3, 0

  ; where this thread's four float4s go: out + 4 * (x + 4y + 16z)
  %gx = extractelement <3 x i32> %gid, i64 0
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
!air.sampler_states = !{!8, !9, !10}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @sample3d, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture3d<float, sample>", !"air.arg_name", !"t"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!7 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
!8 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
!9 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.1}
!10 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.2}
