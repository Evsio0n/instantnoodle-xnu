; M5a's format sweep over the signed integer vertex formats: attribute(1) is an int4, and the
; colour the fragment stage sees is that int4 as floats, so the fetch's own number comes back.
;
;     struct in { float4 pos [[attribute(0)]]; int4 col [[attribute(1)]]; };
;     vertex out m5a_vint(in v [[stage_in]]) { return { v.pos, float4(v.col) }; }
;
;   llvm-as-18 -o m5a_vint.bc tools/air/m5a_vint.ll

source_filename = "m5a_vint"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float>, <4 x float> }> @m5a_vint(<4 x float> %pos, <4 x i32> %col) local_unnamed_addr {
  %f = sitofp <4 x i32> %col to <4 x float>
  %r0 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %pos, 0
  %r1 = insertvalue <{ <4 x float>, <4 x float> }> %r0, <4 x float> %f, 1
  ret <{ <4 x float>, <4 x float> }> %r1
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_vint, !3, !6}
!3 = !{!4, !5}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{!"air.vertex_output", !"generated(3colDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
!6 = !{!7, !8}
!7 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!8 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"int4", !"air.arg_name", !"col"}
