; M5a's per-pixel blend source (tools/mesa/air/draw_model.h, dm_src): each pixel of a 16x16
; attachment its own colour and its own alpha, all of them dyadic, so a blend of it over a
; per-pixel destination is exact in binary32 and a CPU can compute it with ==.
;
;     fragment float4 m5a_fsrc(float4 pos [[position]])
;     {
;         return float4(pos.x / 16, pos.y / 16, (pos.x + pos.y) / 32,
;                       (3 * pos.x + 5 * pos.y) / 128);
;     }
;
; Hand-written because nothing in the corpus reads its position (tools/air/fragpos.ll says so).
;   llvm-as-18 -o m5a_fsrc.bc tools/air/m5a_fsrc.ll

source_filename = "m5a_fsrc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @m5a_fsrc(<4 x float> %pos) local_unnamed_addr {
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %r = fmul float %x, 6.250000e-02
  %g = fmul float %y, 6.250000e-02
  %s = fadd float %x, %y
  %b = fmul float %s, 3.125000e-02
  %x3 = fmul float %x, 3.000000e+00
  %y5 = fmul float %y, 5.000000e+00
  %t = fadd float %x3, %y5
  %a = fmul float %t, 7.812500e-03
  %v0 = insertelement <4 x float> undef, float %r, i64 0
  %v1 = insertelement <4 x float> %v0, float %g, i64 1
  %v2 = insertelement <4 x float> %v1, float %b, i64 2
  %v3 = insertelement <4 x float> %v2, float %a, i64 3
  %ret = insertvalue <{ <4 x float> }> undef, <4 x float> %v3, 0
  ret <{ <4 x float> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_fsrc, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
