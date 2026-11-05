; M5a's checkerboard (tools/mesa/air/draw_model.h, dm_check): two-pixel squares, for the pass
; test that a Load keeps what the pass before it drew.
;
;     fragment float4 m5a_fcheck(float4 pos [[position]])
;     {
;         uint c = ((uint(pos.x) >> 1) ^ (uint(pos.y) >> 1)) & 1;
;         return float4(float(c), 1 - float(c), 0.25, 1);
;     }
;
;   llvm-as-18 -o m5a_fcheck.bc tools/air/m5a_fcheck.ll

source_filename = "m5a_fcheck"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

define <{ <4 x float> }> @m5a_fcheck(<4 x float> %pos) local_unnamed_addr {
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %xi = fptoui float %x to i32
  %yi = fptoui float %y to i32
  %xs = lshr i32 %xi, 1
  %ys = lshr i32 %yi, 1
  %e = xor i32 %xs, %ys
  %c = and i32 %e, 1
  %cf = uitofp i32 %c to float
  %nf = fsub float 1.000000e+00, %cf
  %v0 = insertelement <4 x float> undef, float %cf, i64 0
  %v1 = insertelement <4 x float> %v0, float %nf, i64 1
  %v2 = insertelement <4 x float> %v1, float 2.500000e-01, i64 2
  %v3 = insertelement <4 x float> %v2, float 1.000000e+00, i64 3
  %ret = insertvalue <{ <4 x float> }> undef, <4 x float> %v3, 0
  ret <{ <4 x float> }> %ret
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @m5a_fcheck, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{!6}
!6 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
