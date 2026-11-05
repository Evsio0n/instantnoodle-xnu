// M5c: argument buffers, a fragment or vertex shader's stores, arrays of descriptors indexed at run
// time and function constants. tools/air/m5c_rows.h draws these and judges the pictures, and the same
// rows run on real Metal (tools/air/mtlref) and on the phone (metal_drv_test --m5c), from this one
// source: this Mac's Metal compiler makes the library (tools/air/mtlref/airdump.m).
#include <metal_stdlib>
using namespace metal;

struct V { float4 pos [[position]]; };

// The whole 16 x 16 attachment, by vertex_id, as a strip.
vertex V m5c_vs(uint vid [[vertex_id]])
{
   const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
   V o;
   o.pos = float4(p[vid], 0.5, 1);
   return o;
}

fragment float4 m5c_fsolid(V in [[stage_in]]) { return float4(0.5, 0.25, 0.75, 1); }

// ---- argument buffers ----

struct A1 {
   texture2d<float> t [[id(0)]];
   sampler s [[id(1)]];
   constant float4 *b [[id(2)]];
};
fragment float4 m5c_fa1(V in [[stage_in]], constant A1 &a [[buffer(0)]])
{
   return a.t.sample(a.s, in.pos.xy / 16) + a.b[1];
}

// two sets, and a direct texture and buffer beside them
struct A2 { texture2d<float> t [[id(0)]]; sampler s [[id(1)]]; };
struct A3 { constant float4 *b [[id(0)]]; };
fragment float4 m5c_fa2(V in [[stage_in]], constant A2 &x [[buffer(1)]], constant A3 &y [[buffer(2)]],
                        constant float4 &u [[buffer(0)]], texture2d<float> d [[texture(0)]])
{
   return x.t.sample(x.s, in.pos.xy / 16) + y.b[0] + u + d.read(uint2(in.pos.xy)) * 0.5;
}

// a member nothing reads, which an application need not encode
struct A4 { texture2d<float> unused [[id(0)]]; constant float4 *b [[id(1)]]; };
fragment float4 m5c_fa4(V in [[stage_in]], constant A4 &a [[buffer(0)]]) { return a.b[0]; }

// the vertex stage's own set
struct AV { constant float2 *pos [[id(0)]]; constant float *scale [[id(1)]]; };
vertex V m5c_va(uint vid [[vertex_id]], constant AV &a [[buffer(0)]])
{
   V o;
   o.pos = float4(a.pos[vid] * a.scale[0], 0.5, 1);
   return o;
}

// a kernel's
struct AK { device float4 *out [[id(0)]]; constant float4 *in [[id(1)]]; texture2d<float> t [[id(2)]]; };
kernel void m5c_ka(uint2 g [[thread_position_in_grid]], constant AK &a [[buffer(0)]])
{
   a.out[g.y * 16 + g.x] = a.in[g.y * 16 + g.x] * 2 + a.t.read(g);
}

// more than a push set holds: 34 members
struct AM {
   constant float4 *b0 [[id(0)]], *b1 [[id(1)]], *b2 [[id(2)]], *b3 [[id(3)]], *b4 [[id(4)]], *b5 [[id(5)]],
      *b6 [[id(6)]], *b7 [[id(7)]], *b8 [[id(8)]], *b9 [[id(9)]], *b10 [[id(10)]], *b11 [[id(11)]],
      *b12 [[id(12)]], *b13 [[id(13)]], *b14 [[id(14)]], *b15 [[id(15)]], *b16 [[id(16)]], *b17 [[id(17)]],
      *b18 [[id(18)]], *b19 [[id(19)]], *b20 [[id(20)]], *b21 [[id(21)]], *b22 [[id(22)]], *b23 [[id(23)]],
      *b24 [[id(24)]], *b25 [[id(25)]], *b26 [[id(26)]], *b27 [[id(27)]], *b28 [[id(28)]], *b29 [[id(29)]],
      *b30 [[id(30)]], *b31 [[id(31)]], *b32 [[id(32)]], *b33 [[id(33)]];
};
fragment float4 m5c_fam(V in [[stage_in]], constant AM &a [[buffer(0)]])
{
   return *a.b0 + *a.b1 + *a.b2 + *a.b3 + *a.b4 + *a.b5 + *a.b6 + *a.b7 + *a.b8 + *a.b9 + *a.b10 + *a.b11 +
          *a.b12 + *a.b13 + *a.b14 + *a.b15 + *a.b16 + *a.b17 + *a.b18 + *a.b19 + *a.b20 + *a.b21 + *a.b22 +
          *a.b23 + *a.b24 + *a.b25 + *a.b26 + *a.b27 + *a.b28 + *a.b29 + *a.b30 + *a.b31 + *a.b32 + *a.b33;
}

// ---- arrays of descriptors ----

struct U { uint i; uint j; };

fragment float4 m5c_fix_const(V in [[stage_in]], array<texture2d<float>, 3> t [[texture(0)]],
                              sampler s [[sampler(0)]])
{
   return t[1].sample(s, in.pos.xy / 16);
}
fragment float4 m5c_fix_run(V in [[stage_in]], array<texture2d<float>, 3> t [[texture(0)]],
                            sampler s [[sampler(0)]], constant U &u [[buffer(0)]])
{
   return t[u.i].sample(s, in.pos.xy / 16);
}
fragment float4 m5c_fix_read(V in [[stage_in]], array<texture2d<float>, 3> t [[texture(0)]],
                             constant U &u [[buffer(0)]])
{
   return t[u.i].read(uint2(in.pos.xy));
}
fragment float4 m5c_fix_size(V in [[stage_in]], array<texture2d<float>, 3> t [[texture(0)]],
                             constant U &u [[buffer(0)]])
{
   return float4(t[u.i].get_width(), t[u.i].get_height(), t[u.i].get_num_mip_levels(), 1);
}
fragment float4 m5c_fix_sampler(V in [[stage_in]], texture2d<float> t [[texture(0)]],
                                array<sampler, 2> s [[sampler(0)]], constant U &u [[buffer(0)]])
{
   return t.sample(s[u.i], in.pos.xy / 8 - 0.5);
}
// a different element for each pixel, which is not one descriptor for the draw
fragment float4 m5c_fix_pixel(V in [[stage_in]], array<texture2d<float>, 3> t [[texture(0)]],
                              sampler s [[sampler(0)]])
{
   return t[uint(in.pos.x) % 3].sample(s, in.pos.xy / 16);
}

struct AI {
   array<texture2d<float>, 3> t [[id(0)]];
   array<sampler, 2> s [[id(3)]];
   array<constant float4 *, 3> b [[id(5)]];
};
fragment float4 m5c_fib_tex(V in [[stage_in]], constant AI &a [[buffer(1)]], constant U &u [[buffer(0)]])
{
   return a.t[u.i].sample(a.s[0], in.pos.xy / 16);
}
fragment float4 m5c_fib_samp(V in [[stage_in]], constant AI &a [[buffer(1)]], constant U &u [[buffer(0)]])
{
   return a.t[0].sample(a.s[u.i], in.pos.xy / 8 - 0.5);
}
fragment float4 m5c_fib_buf(V in [[stage_in]], constant AI &a [[buffer(1)]], constant U &u [[buffer(0)]])
{
   return a.b[u.i][0];
}
fragment float4 m5c_fib_const(V in [[stage_in]], constant AI &a [[buffer(1)]])
{
   return a.t[2].sample(a.s[1], in.pos.xy / 16) + a.b[2][0];
}
struct AIW { array<device float4 *, 2> b [[id(0)]]; };
fragment float4 m5c_fib_store(V in [[stage_in]], constant AIW &a [[buffer(1)]], constant U &u [[buffer(0)]])
{
   a.b[u.i][uint(in.pos.y) * 16 + uint(in.pos.x)] = float4(in.pos.xy, 1, 1);
   return float4(0);
}
kernel void m5c_kib(uint2 g [[thread_position_in_grid]], constant AIW &a [[buffer(1)]],
                    constant U &u [[buffer(0)]])
{
   a.b[u.i][g.y * 16 + g.x] = float4(float2(g), 2, 1);
}
kernel void m5c_kimg(uint2 g [[thread_position_in_grid]],
                     array<texture2d<float, access::write>, 2> t [[texture(0)]], constant U &u [[buffer(0)]])
{
   t[u.i].write(float4(float2(g), 3, 1), g);
}

// ---- a fragment or vertex shader's stores ----

fragment float4 m5c_fs_buf(V in [[stage_in]], device float4 *out [[buffer(0)]])
{
   out[uint(in.pos.y) * 16 + uint(in.pos.x)] = float4(in.pos.xy, 1, 1);
   return float4(1, 0, 0, 1);
}
fragment float4 m5c_fs_tex(V in [[stage_in]], texture2d<float, access::write> w [[texture(0)]])
{
   w.write(float4(in.pos.xy, 2, 1), uint2(in.pos.xy));
   return float4(0, 1, 0, 1);
}
fragment float4 m5c_fs_rw(V in [[stage_in]], texture2d<float, access::read_write> w [[texture(0)]])
{
   uint2 c = uint2(in.pos.xy);
   float v = w.read(c).r;
   w.write(float4(v + 1), c);
   return float4(v, 0, 0, 1);
}
fragment float4 m5c_fs_atomic(V in [[stage_in]], device atomic_uint *c [[buffer(0)]])
{
   atomic_fetch_add_explicit(&c[uint(in.pos.y) * 16 + uint(in.pos.x)], 1, memory_order_relaxed);
   return float4(0, 0, 1, 1);
}
[[early_fragment_tests]]
fragment float4 m5c_fs_early(V in [[stage_in]], device atomic_uint *c [[buffer(0)]])
{
   atomic_fetch_add_explicit(&c[uint(in.pos.y) * 16 + uint(in.pos.x)], 1, memory_order_relaxed);
   return float4(0, 0, 1, 1);
}
vertex V m5c_vs_atomic(uint vid [[vertex_id]], uint iid [[instance_id]], device atomic_uint *c [[buffer(0)]])
{
   const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
   atomic_fetch_add_explicit(&c[iid * 4 + vid], 1, memory_order_relaxed);
   V o;
   o.pos = float4(p[vid], 0.5, 1);
   return o;
}
vertex V m5c_vs_buf(uint vid [[vertex_id]], device float4 *out [[buffer(0)]])
{
   const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
   out[vid] = float4(p[vid], float(vid), 1);
   V o;
   o.pos = float4(p[vid], 0.5, 1);
   return o;
}
struct AS { device float4 *out [[id(0)]]; };
fragment float4 m5c_fs_argbuf(V in [[stage_in]], constant AS &a [[buffer(0)]])
{
   a.out[uint(in.pos.y) * 16 + uint(in.pos.x)] = float4(in.pos.xy, 3, 1);
   return float4(0);
}

// ---- max3, min3 and median3, which chronod's widgets asked for at half (run 125612) ----

fragment float4 m5c_three_f32(V in [[stage_in]], constant float4 &u [[buffer(0)]])
{
   return float4(fmax3(u.x, u.y, u.z), fmin3(u.x, u.y, u.z), fmedian3(u.x, u.y, u.z), 1);
}
fragment float4 m5c_three_f16(V in [[stage_in]], constant float4 &u [[buffer(0)]])
{
   half a = half(u.x), b = half(u.y), c = half(u.z);
   return float4(float(fmax3(a, b, c)), float(fmin3(a, b, c)), float(fmedian3(a, b, c)), 1);
}
fragment float4 m5c_three_int(V in [[stage_in]], constant float4 &u [[buffer(0)]])
{
   int a = int(u.x * 4), b = int(u.y * 4), c = int(u.z * 4);
   return float4(float(max3(a, b, c)), float(min3(a, b, c)), float(median3(a, b, c)), 1);
}
fragment float4 m5c_three_uint(V in [[stage_in]], constant float4 &u [[buffer(0)]])
{
   uint a = uint(u.x * 4), b = uint(u.y * 4), c = uint(u.z * 4);
   return float4(float(max3(a, b, c)), float(min3(a, b, c)), float(median3(a, b, c)), 1);
}

// Metal's pack and unpack, by a number in a buffer so that nothing folds: a word comes out as its low and
// high sixteen bits, as floats that hold them exactly, and an unpack as what it unpacks to.
fragment float4 m5c_pack_all(V in [[stage_in]], constant float4 &u [[buffer(0)]], constant uint &which [[buffer(1)]],
                             constant uint &word [[buffer(2)]])
{
   uint w = 0;
   half4 h = half4(u);
   switch (which) {
   case 0: w = pack_float_to_unorm4x8(u); break;
   case 1: w = pack_float_to_snorm4x8(u); break;
   case 2: w = pack_float_to_unorm2x16(u.xy); break;
   case 3: w = pack_float_to_snorm2x16(u.xy); break;
   case 4: w = pack_float_to_unorm10a2(u); break;
   case 6: w = pack_float_to_unorm565(u.xyz); break;
   case 7: w = pack_half_to_unorm4x8(h); break;
   case 8: w = pack_half_to_unorm2x16(h.xy); break;
   case 9: w = pack_half_to_unorm10a2(h); break;
   case 10: return unpack_unorm4x8_to_float(word);
   case 11: return unpack_snorm4x8_to_float(word);
   case 12: return float4(unpack_unorm2x16_to_float(word), 0, 1);
   case 13: return float4(unpack_snorm2x16_to_float(word), 0, 1);
   case 14: return unpack_unorm10a2_to_float(word);
   case 16: return float4(unpack_unorm565_to_float(ushort(word)), 1);
   case 17: return float4(unpack_unorm2x16_to_half(word).xyxy);
   case 18: return float4(unpack_unorm10a2_to_half(word));
   default: break;
   }
   return float4(float(w & 0xffff), float(w >> 16), 0, 1);
}
fragment float4 m5c_pack565(V in [[stage_in]], constant float4 &u [[buffer(0)]])
{
   ushort v = pack_half_to_unorm565(half3(half(u.x), half(u.y), half(u.z)));
   return float4(float(v), 0, 0, 1);
}

// ---- a colour matrix of half4 rows in a constant buffer, as CoreAnimation's filters read one ----

struct HalfRows { float4 pad[8]; half4 row[5]; };
fragment half4 m5c_half_rows(V in [[stage_in]], constant HalfRows &u [[buffer(1)]])
{
   half4 c = half4(0.5h, 0.25h, 0.75h, 1);
   half4 r = c.x * u.row[0] + c.y * u.row[1] + c.z * u.row[2] + c.w * u.row[3] + u.row[4];
   return r;
}

// ---- a half between the stages, which is how CoreAnimation's colour arrives ----

struct VH { float4 pos [[position]]; half4 c; };
vertex VH m5c_vs_half(uint vid [[vertex_id]])
{
   const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
   VH o;
   o.pos = float4(p[vid], 0.5, 1);
   o.c = half4(0.25h, 0.5h, 0.75h, 1);
   return o;
}
fragment half4 m5c_fs_half(VH in [[stage_in]]) { return in.c; }

// ---- framebuffer fetch: what the compositor's own shaders read ----

fragment float4 m5c_fetch(V in [[stage_in]], float4 dst [[color(0)]])
{
   return dst.bgra * 0.5 + float4(0.25);
}
fragment float4 m5c_fetch_add(V in [[stage_in]], float4 dst [[color(0)]])
{
   return dst + float4(0.125, 0.125, 0.125, 0);
}

// and the second attachment: CoreAnimation draws a masked layer into [[color(1)]] and reads it back
struct TwoOut { float4 a [[color(0)]]; float4 b [[color(1)]]; };
fragment TwoOut m5c_fetch_w1(V in [[stage_in]])
{
   TwoOut o;
   o.a = float4(1, 0, 0, 1);    // masked off by the pipeline
   o.b = float4(0.25, 0.5, 0.75, 1);
   return o;
}
fragment TwoOut m5c_fetch_r1(V in [[stage_in]], float4 d1 [[color(1)]])
{
   TwoOut o;
   o.a = d1;
   o.b = d1;
   return o;
}

// ---- function constants ----

constant bool fc_b [[function_constant(0)]];
constant float fc_f [[function_constant(1)]];
constant int fc_i [[function_constant(2)]];
constant float4 fc_v [[function_constant(3)]];
constant float2 fc_n [[function_constant(4)]];
constant uchar fc_c [[function_constant(5)]];
constant ushort fc_s [[function_constant(6)]];
constant bool fc_fd = is_function_constant_defined(fc_f);
constant int fc_or = is_function_constant_defined(fc_i) ? fc_i : 7;
constant bool fc_nb = !fc_b;

fragment float4 m5c_fc1(V in [[stage_in]])
{
   return float4(fc_fd ? fc_f : -1.0, float(fc_or), fc_b ? 1.0 : 0.0, 1);
}
fragment float4 m5c_fc2(V in [[stage_in]]) { return fc_v + float4(fc_n, 0, 0); }
fragment float4 m5c_fc3(V in [[stage_in]], texture2d<float> t [[texture(0), function_constant(fc_b)]],
                        sampler s [[sampler(0), function_constant(fc_b)]],
                        constant float4 &u [[buffer(0), function_constant(fc_nb)]])
{
   if (fc_b)
      return t.sample(s, in.pos.xy / 16);
   return u;
}
fragment float4 m5c_fc4(V in [[stage_in]]) { return float4(float(fc_c), float(fc_s), 0, 1); }
vertex V m5c_vc(uint vid [[vertex_id]])
{
   const float2 p[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
   V o;
   o.pos = float4(p[vid] * fc_f, 0.5, 1);
   return o;
}
kernel void m5c_kc(device float4 *out [[buffer(0)]])
{
   out[0] = float4(fc_fd ? fc_f : -1.0, float(fc_or), fc_b ? 1.0 : 0.0, 1);
}

/*
 * M5e, kernels as VideoToolbox's scale writes them (vt_test --pixels: a status of 0 over zeros, run 160351). What a
 * kernel's sample answers, through an API sampler, for a mipmapped texture and in pixel coordinates; half colours kept in
 * a private array and packed by hand; a table of RGBA32Float read texel by texel with a loop counter of 8 bits.
 */
kernel void m5c_ks(texture2d<float, access::sample> t [[texture(0)]], sampler s [[sampler(0)]],
                   device float4 *out [[buffer(0)]])
{
   out[0] = t.sample(s, float2(0.5, 0.5));
}

kernel void m5c_kh(texture2d<half, access::sample> t [[texture(0)]], sampler s [[sampler(0)]],
                   device float4 *out [[buffer(0)]])
{
   out[0] = float4(t.sample(s, float2(2.5, 2.5)));
}

kernel void m5c_kha(texture2d<half, access::sample> t [[texture(0)]], sampler s [[sampler(0)]],
                    device float4 *out [[buffer(0)]], constant uint &n [[buffer(1)]])
{
   half4 a[8];
   for (uint i = 0; i < 8; i++)
      a[i] = half4(0, 0, 0, 1);
   for (uint i = 0; i < n; i++) {
      a[i] = t.sample(s, float2(2.5 + i, 2.5));
      a[i] = a[i].zyxw;
   }
   for (uint i = 0; i < n; i++) {
      float4 f = saturate(float4(a[i]));
      uint p = uint(floor(f.x * 255 + 0.5)) | uint(floor(f.y * 255 + 0.5)) << 8 |
               uint(floor(f.z * 255 + 0.5)) << 16 | uint(floor(f.w * 255 + 0.5)) << 24;
      out[i] = float4(p & 255, (p >> 8) & 255, (p >> 16) & 255, p >> 24) / 255;
   }
}

kernel void m5c_ktb(texture2d<float, access::read> kern [[texture(2)]], device float4 *out [[buffer(0)]],
                    constant uint &n [[buffer(1)]])
{
   float4 sum = 0;
   for (uint k = 0; k < n; k += 4) {
      float4 w = kern.read(uint2(k >> 2, 5), 0);
      for (uchar j = 0; j < 4 && k + j < n; j++)
         sum.x += w[j];
   }
   out[0] = float4(sum.x, kern.read(uint2(1, 63), 0).y, 0, 1);
}

/* M5e: a float and a half written to textures of other formats: the GPU converts by the type written */
kernel void m5c_kwf(texture2d<float, access::write> o [[texture(0)]])
{
   o.write(float4(0.25, 0.5, 0.75, 1), uint2(0, 0));
}

kernel void m5c_kwh(texture2d<half, access::write> o [[texture(0)]])
{
   o.write(half4(0.25, 0.5, 0.75, 1), uint2(0, 0));
}

/* M5e, as Photos' fragment_main keeps them: an array of textures copied into a struct on the stack, read at a computed index. */
struct M5cHeld { constant float4 *cursor; uint used; array<texture2d<float, access::sample>, 3> t; };

__attribute__((noinline)) static float4
m5c_held_read(thread M5cHeld &held, sampler s, uint i)
{
   held.used += 1;
   return held.t[i].sample(s, float2(0.5, 0.5));
}

kernel void m5c_kdc(array<texture2d<float, access::sample>, 3> t [[texture(0)]], sampler s [[sampler(0)]],
                    device float4 *out [[buffer(0)]], constant uint &n [[buffer(1)]], constant float4 *c [[buffer(2)]])
{
   M5cHeld held{ c, 0, t };
   out[0] = m5c_held_read(held, s, n) * c[0];
   held.cursor = c + 1;       /* (a cursor moved on and never read, as Photos' is) */
   out[1] = m5c_held_read(held, s, n + 1) * float(held.used) * c[1] * 2;
}

/* M5e: Metal's format 540 over a 420f surface, as VideoToolbox reads one. */
kernel void m5c_k540(texture2d<float, access::sample> t [[texture(0)]], device float4 *out [[buffer(0)]])
{
   constexpr sampler s(coord::pixel, filter::linear, address::clamp_to_edge);
   out[0] = t.read(uint2(10, 9)) * 255;
   out[1] = t.sample(s, float2(10.5, 9.5)) * 255;
   out[2] = t.sample(s, float2(11.0, 10.0)) * 255;
}
