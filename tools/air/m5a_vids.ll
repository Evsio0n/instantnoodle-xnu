; M5a's probe of [[vertex_id]] and [[instance_id]] with base values: the corner comes out of a
; buffer BY the vertex id, moved across by the instance id, and both ids are the colour. Metal
; includes the base vertex in [[vertex_id]] and the base instance in [[instance_id]], so a back
; end that loses either draws somewhere else AND in the wrong colour.
;
;     struct out { float4 pos [[position]]; float4 col; };
;     vertex out m5a_vids(uint vid [[vertex_id]], uint iid [[instance_id]],
;                         constant float4 *pos [[buffer(0)]])
;     {
;         float4 p = pos[vid];
;         p.x += float(iid) * 0.25;
;         return { p, float4(float(vid) / 64, float(iid) / 16, 0, 1) };
;     }
;
;   llvm-as-18 -o m5a_vids.bc tools/air/m5a_vids.ll

source_filename = "m5a_vids"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @m5a_vids(i32 %vid, i32 %iid, ptr addrspace(2) noalias nocapture readonly %pos) local_unnamed_addr {
  %i = zext i32 %vid to i64
  %at = getelementptr inbounds <4 x float>, ptr addrspace(2) %pos, i64 %i
  %p = load <4 x float>, ptr addrspace(2) %at, align 16
  %vf = uitofp i32 %vid to float
  %jf = uitofp i32 %iid to float
  %dx = fmul float %jf, 2.500000e-01
  %x0 = extractelement <4 x float> %p, i64 0
  %x = fadd float %x0, %dx
  %q = insertelement <4 x float> %p, float %x, i64 0
  %cr = fmul float %vf, 1.562500e-02
  %cg = fmul float %jf, 6.250000e-02
  %c0 = insertelement <4 x float> undef, float %cr, i64 0
  %c1 = insertelement <4 x float> %c0, float %cg, i64 1
  %c2 = insertelement <4 x float> %c1, float 0.000000e+00, i64 2
  %c3 = insertelement <4 x float> %c2, float 1.000000e+00, i64 3
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %q, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %c3, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_vids, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{!"air.vertex_output", !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!6 = !{!7, !8, !9}
!7 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!8 = !{i32 1, !"air.instance_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"iid"}
!9 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
