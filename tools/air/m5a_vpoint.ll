; M5a's vertex stage for points: m5a_vattr with a [[point_size]] from attribute(2), so the
; size is the application's per vertex, which is what "the size from the shader" means.
;
;     struct in  { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]];
;                  float size [[attribute(2)]]; };
;     struct out { float4 pos [[position]]; float size [[point_size]]; float4 col; };
;     vertex out m5a_vpoint(in v [[stage_in]]) { return { v.pos, v.size, v.col }; }
;
;   llvm-as-18 -o m5a_vpoint.bc tools/air/m5a_vpoint.ll

source_filename = "m5a_vpoint"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, float, <4 x float> }> @m5a_vpoint(<4 x float> %pos, <4 x float> %col, float %size) local_unnamed_addr {
  %r0 = insertvalue <{ <4 x float>, float, <4 x float> }> undef, <4 x float> %pos, 0
  %r1 = insertvalue <{ <4 x float>, float, <4 x float> }> %r0, float %size, 1
  %r2 = insertvalue <{ <4 x float>, float, <4 x float> }> %r1, <4 x float> %col, 2
  ret <{ <4 x float>, float, <4 x float> }> %r2
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_vpoint, !3, !7}
!3 = !{!4, !5, !6}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{!"air.point_size", !"air.arg_type_name", !"float", !"air.arg_name", !"size"}
!6 = !{!"air.vertex_output", !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!7 = !{!8, !9, !10}
!8 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!9 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!10 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"float", !"air.arg_name", !"size"}
