; A kernel that writes every system value a thread has, and a threadgroup round trip.
;
; WHY it is hand-written and not taken from the corpus: of the 47 kernels of AV1SW and
; PortraitFilters, 43 attributes are [[thread_position_in_grid]], three
; [[thread_position_in_threadgroup]] and one [[threads_per_grid]], and every job line
; dispatches ONE group of 4x4x1 -- so [[threadgroup_position_in_grid]] is zero there,
; [[threadgroups_per_grid]] is one, and nothing tells a back end that adds the group's
; base wrongly, or reads the grid's size from the wrong place, from one that is right.
; The Vulkan back end (tools/mesa/air/air_vk.c, COMPUTE, SYSTEM VALUES) hands these to
; turnip as NIR's own intrinsics and turnip lowers them (load_num_workgroups and the base
; workgroup id are driver parameters there), so they need a measurement on the phone, on
; both back ends, of a grid of more than one group of a group that is not square:
;
;     kernel void sysvals(device uint *out              [[buffer(0)]],
;                         uint3 gid                     [[thread_position_in_grid]],
;                         uint3 tgid                    [[threadgroup_position_in_grid]],
;                         uint3 tid                     [[thread_position_in_threadgroup]],
;                         uint3 tptg                    [[threads_per_threadgroup]],
;                         uint3 tgpg                    [[threadgroups_per_grid]],
;                         uint  lidx                    [[thread_index_in_threadgroup]],
;                         uint3 tpg                     [[threads_per_grid]])
;     {
;         threadgroup uint tg[256];
;         uint n = tptg.x * tptg.y * tptg.z;
;         tg[lidx] = lidx * 3 + 1;
;         threadgroup_barrier(mem_flags::mem_threadgroup);
;         device uint *o = out + 16 * (gid.x + tpg.x * (gid.y + tpg.y * gid.z));
;         o[0] = gid.x;  o[1] = gid.y;  o[2] = gid.z;
;         o[3] = tgid.x; o[4] = tgid.y; o[5] = tgid.z;
;         o[6] = tid.x;  o[7] = tid.y;  o[8] = tid.z;
;         o[9] = tptg.x; o[10] = tptg.y; o[11] = tptg.z;
;         o[12] = tgpg.x | tgpg.y << 8 | tgpg.z << 16;
;         o[13] = lidx;
;         o[14] = tpg.x | tpg.y << 8 | tpg.z << 16;
;         o[15] = tg[n - 1 - lidx];        // the mirror thread's word, across the barrier
;     }
;
; Dispatched as 2x2x1 groups of 4x2x2 threads (air_metal_test's GROUPS and THREADS) it
; writes all 4096 bytes of buffer 0 -- 64 threads, 16 words each -- so the fill does not
; reach the result, and every word is a function of the grid alone:
;
;     air_metal_test_gl --gpu sysvals.bc sysvals 125 0 2x2x1 4x2x2
;     air_metal_test_vk --gpu sysvals.bc sysvals 125 0 2x2x1 4x2x2
;
; and the result line of both has to read
;
;     air_metal_test: compute-result BACKEND sysvals ran b0 877fc70d892fc565
;
; which is FNV-1a over the 1024 words computed from the formula above on the host (the
; first record, thread (0,0,0), is 0 0 0  0 0 0  0 0 0  4 2 2  0x10202  0  0x20408  46;
; word 15 is (15 - lidx) * 3 + 1). A back end that reads tgid as zero, or tgpg/tpg from
; the group instead of the grid, or loses the barrier, changes words and the digest.
; Written here rather than generated because it is one function of straight-line code;
; the metadata keys are the AIR interface (docs/air.md section 3, air_to_nir.c's sysvals
; table) and nothing in this file comes out of Apple's libraries.
;
;   llvm-as -o sysvals.bc tools/air/sysvals.ll

source_filename = "sysvals"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

@sysvals.tg = internal unnamed_addr addrspace(3) global [256 x i32] undef, align 4

declare void @air.wg.barrier(i32, i32) local_unnamed_addr

define void @sysvals(ptr addrspace(1) noalias nocapture %out, <3 x i32> %gid,
                     <3 x i32> %tgid, <3 x i32> %tid, <3 x i32> %tptg, <3 x i32> %tgpg,
                     i32 %lidx, <3 x i32> %tpg) local_unnamed_addr {
  ; the threadgroup round trip: this thread's word in, the mirror thread's out
  %mine = mul i32 %lidx, 3
  %mine1 = add i32 %mine, 1
  %lidx64 = zext i32 %lidx to i64
  %slot = getelementptr inbounds [256 x i32], ptr addrspace(3) @sysvals.tg, i64 0, i64 %lidx64
  store i32 %mine1, ptr addrspace(3) %slot, align 4
  tail call void @air.wg.barrier(i32 2, i32 1)
  %nx = extractelement <3 x i32> %tptg, i64 0
  %ny = extractelement <3 x i32> %tptg, i64 1
  %nz = extractelement <3 x i32> %tptg, i64 2
  %nxy = mul i32 %nx, %ny
  %n = mul i32 %nxy, %nz
  %last = add i32 %n, -1
  %mirror = sub i32 %last, %lidx
  %mirror64 = zext i32 %mirror to i64
  %oslot = getelementptr inbounds [256 x i32], ptr addrspace(3) @sysvals.tg, i64 0, i64 %mirror64
  %other = load i32, ptr addrspace(3) %oslot, align 4

  ; where this thread's sixteen words go
  %gx = extractelement <3 x i32> %gid, i64 0
  %gy = extractelement <3 x i32> %gid, i64 1
  %gz = extractelement <3 x i32> %gid, i64 2
  %px = extractelement <3 x i32> %tpg, i64 0
  %py = extractelement <3 x i32> %tpg, i64 1
  %pz = extractelement <3 x i32> %tpg, i64 2
  %rz = mul i32 %py, %gz
  %row = add i32 %gy, %rz
  %rx = mul i32 %px, %row
  %linear = add i32 %gx, %rx
  %base = shl i32 %linear, 4
  %base64 = zext i32 %base to i64
  %o0 = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %base64

  ; gid, tgid, tid, tptg
  store i32 %gx, ptr addrspace(1) %o0, align 4
  %o1 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 1
  store i32 %gy, ptr addrspace(1) %o1, align 4
  %o2 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 2
  store i32 %gz, ptr addrspace(1) %o2, align 4
  %tgx = extractelement <3 x i32> %tgid, i64 0
  %tgy = extractelement <3 x i32> %tgid, i64 1
  %tgz = extractelement <3 x i32> %tgid, i64 2
  %o3 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 3
  store i32 %tgx, ptr addrspace(1) %o3, align 4
  %o4 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 4
  store i32 %tgy, ptr addrspace(1) %o4, align 4
  %o5 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 5
  store i32 %tgz, ptr addrspace(1) %o5, align 4
  %tx = extractelement <3 x i32> %tid, i64 0
  %ty = extractelement <3 x i32> %tid, i64 1
  %tz = extractelement <3 x i32> %tid, i64 2
  %o6 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 6
  store i32 %tx, ptr addrspace(1) %o6, align 4
  %o7 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 7
  store i32 %ty, ptr addrspace(1) %o7, align 4
  %o8 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 8
  store i32 %tz, ptr addrspace(1) %o8, align 4
  %o9 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 9
  store i32 %nx, ptr addrspace(1) %o9, align 4
  %o10 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 10
  store i32 %ny, ptr addrspace(1) %o10, align 4
  %o11 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 11
  store i32 %nz, ptr addrspace(1) %o11, align 4

  ; tgpg packed, lidx, tpg packed, the mirror's word
  %gpx = extractelement <3 x i32> %tgpg, i64 0
  %gpy = extractelement <3 x i32> %tgpg, i64 1
  %gpz = extractelement <3 x i32> %tgpg, i64 2
  %gpy8 = shl i32 %gpy, 8
  %gpz16 = shl i32 %gpz, 16
  %gp1 = or i32 %gpx, %gpy8
  %gp = or i32 %gp1, %gpz16
  %o12 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 12
  store i32 %gp, ptr addrspace(1) %o12, align 4
  %o13 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 13
  store i32 %lidx, ptr addrspace(1) %o13, align 4
  %py8 = shl i32 %py, 8
  %pz16 = shl i32 %pz, 16
  %pp1 = or i32 %px, %py8
  %pp = or i32 %pp1, %pz16
  %o14 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 14
  store i32 %pp, ptr addrspace(1) %o14, align 4
  %o15 = getelementptr inbounds i32, ptr addrspace(1) %o0, i64 15
  store i32 %other, ptr addrspace(1) %o15, align 4
  ret void
}

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @sysvals, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8, !9, !10, !11, !12}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
!7 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tgid"}
!8 = !{i32 3, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tid"}
!9 = !{i32 4, !"air.threads_per_threadgroup", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tptg"}
!10 = !{i32 5, !"air.threadgroups_per_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tgpg"}
!11 = !{i32 6, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint", !"air.arg_name", !"lidx"}
!12 = !{i32 7, !"air.threads_per_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tpg"}
