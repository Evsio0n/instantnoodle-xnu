; M5a's vertex stage for the instance step: a per-vertex corner plus a per-instance offset and
; colour, so an instance's stream step decides where its quad lands and what colour it is.
;
;     struct in  { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]];
;                  float2 off [[attribute(2)]]; };
;     vertex out m5a_vinst(in v [[stage_in]])
;     {
;         return { float4(v.pos.xy + v.off, v.pos.zw), v.col };
;     }
;
;   llvm-as-18 -o m5a_vinst.bc tools/air/m5a_vinst.ll

source_filename = "m5a_vinst"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @m5a_vinst(<4 x float> %pos, <4 x float> %col, <2 x float> %off) local_unnamed_addr {
  %px = extractelement <4 x float> %pos, i64 0
  %py = extractelement <4 x float> %pos, i64 1
  %ox = extractelement <2 x float> %off, i64 0
  %oy = extractelement <2 x float> %off, i64 1
  %x = fadd float %px, %ox
  %y = fadd float %py, %oy
  %p0 = insertelement <4 x float> %pos, float %x, i64 0
  %p1 = insertelement <4 x float> %p0, float %y, i64 1
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %p1, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %col, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_vinst, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{!"air.vertex_output", !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!6 = !{!7, !8, !9}
!7 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!8 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!9 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"float2", !"air.arg_name", !"off"}
