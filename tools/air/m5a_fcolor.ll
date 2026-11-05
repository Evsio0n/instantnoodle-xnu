; M5a's fragment stage for everything geometric: the interpolated colour of the vertex stage,
; to the first attachment, so a pixel says both that it was covered and which barycentrics it
; was covered with.
;
;     struct in { float4 col [[stage_in]]; };
;     fragment float4 m5a_fcolor(float4 col) { return col; }
;
;   llvm-as-18 -o m5a_fcolor.bc tools/air/m5a_fcolor.ll

source_filename = "m5a_fcolor"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @m5a_fcolor(<4 x float> %col) local_unnamed_addr {
  %ret = insertvalue <{ <4 x float> }> undef, <4 x float> %col, 0
  ret <{ <4 x float> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_fcolor, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.fragment_input", !"generated(3colDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"col"}
