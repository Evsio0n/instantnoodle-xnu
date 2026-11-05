/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * mtlref: what a real Metal does, on this Mac, against what tools/mesa/air/draw_model.h and
 * tex_model.h say it must do.
 *
 * Those two headers are the reference the phone's driver tests compare with, and in several places
 * they are the ONLY reference, written from memory of specifications. This asks a real
 * implementation the same questions, with the same vertices, the same texels and the same blends,
 * and prints the two answers side by side. It includes the two headers themselves, so a model
 * changed there is the model judged here. It judges what the API MEANS: which pixels, which texel,
 * which channel. It does not judge Adreno's rounding: the tolerances are the models' own, and Apple's
 * rounding differing inside them says nothing (`exact=` on a line says how much of it was
 * bit-for-bit, for reading and not for judging).
 *
 * BUILD (from the top of the tree; the tool is not part of any other build)
 *   clang -fobjc-arc -O1 -framework Metal -framework Foundation \
 *         -o tools/air/mtlref/mtlref tools/air/mtlref/mtlref.m
 * RUN
 *   tools/air/mtlref/mtlref                  every case, each in a process of its own, then again
 *                                            under MTL_DEBUG_LAYER=1 (a line per case, what the
 *                                            validation layer refused)
 *   tools/air/mtlref/mtlref --no-validate    without the second pass
 *   tools/air/mtlref/mtlref --list           the names
 *   tools/air/mtlref/mtlref --case NAME [N]  one case in this process
 *
 * WHY A PROCESS PER CASE: Metal refuses many things by an assertion that ends the process, and what it
 * refuses is one of the things being measured. So a refusal is a line (`mtlref: NAME ABORTED ...`),
 * and the next case still runs.
 *
 * Every line starts `mtlref: `. A verdict line says `agree` or `DIFFER`; coverage maps are one line per
 * row, row 0 first (the top of the attachment as Metal numbers it), `#` drawn, `.` not, `?` not judged.
 *
 * What is copied and not included: pix_v, pix_model_tri, pix_model_line, pix_model_point, tx_block,
 * tx_gradient and the case tables are air_metal_test.c's, static there, and are copied as they were on
 * 2026-11-05. If those change, so must these.
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../mesa/air/tex_model.h"   /* and draw_model.h */
#include "../m5c_rows.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the shaders, the phone tests' tools/air/m5a_*.ll and m5b_*.ll in the language they came from -- */

#define MSL(...) #__VA_ARGS__
static const char *msl_source = "#include <metal_stdlib>\nusing namespace metal;\n" MSL(
struct VIn { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };
struct VOut { float4 pos [[position]]; float4 col; };
struct FCol { float4 col; };
vertex VOut m5a_vattr(VIn v [[stage_in]]) { VOut o; o.pos = v.pos; o.col = v.col; return o; }
fragment float4 m5a_fcolor(FCol in [[stage_in]]) { return in.col; }
struct PIn { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]]; float size [[attribute(2)]]; };
struct POut { float4 pos [[position]]; float size [[point_size]]; float4 col; };
vertex POut m5a_vpoint(PIn v [[stage_in]]) { POut o; o.pos = v.pos; o.size = v.size; o.col = v.col; return o; }
vertex VOut m5a_vids(uint vid [[vertex_id]], uint iid [[instance_id]], constant float4 *pos [[buffer(0)]]) {
   VOut o; float4 p = pos[vid]; p.x += float(iid) * 0.25; o.pos = p;
   o.col = float4(float(vid) / 64.0, float(iid) / 16.0, 0.0, 1.0); return o; }
vertex void vrec(uint vid [[vertex_id]], uint iid [[instance_id]], device atomic_uint *n [[buffer(0)]],
                 device uint2 *rec [[buffer(1)]], constant uint &cap [[buffer(2)]]) {
   uint i = atomic_fetch_add_explicit(n, 1u, memory_order_relaxed); if (i < cap) rec[i] = uint2(vid, iid); }
vertex POut vrestart(uint vid [[vertex_id]], constant float4 *pos [[buffer(0)]], constant uint &n [[buffer(1)]]) {
   POut o; o.pos = vid < n ? pos[vid] : pos[n - 1]; o.size = 3.0; o.col = float4(1.0, 0.0, 0.0, 1.0); return o; }
struct SIn { uint e [[attribute(0)]]; };
vertex void vstep(SIn v [[stage_in]], uint iid [[instance_id]], device atomic_uint *n [[buffer(0)]],
                  device uint2 *rec [[buffer(1)]], constant uint &cap [[buffer(2)]]) {
   uint i = atomic_fetch_add_explicit(n, 1u, memory_order_relaxed); if (i < cap) rec[i] = uint2(iid, v.e); }
struct QOut { float4 pos [[position]]; };
vertex QOut vquad(uint vid [[vertex_id]]) { QOut o;
   o.pos = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0, 0.0, 1.0); return o; }
struct U { float4 u0, u1, u2; float lod, bias, ref, ref_base; uint index, select; uint pad[2]; };
fragment float4 m5a_fsrc(float4 pos [[position]]) {
   return float4(pos.x / 16.0, pos.y / 16.0, (pos.x + pos.y) / 32.0, (3.0 * pos.x + 5.0 * pos.y) / 128.0); }
fragment uint4 m5b_fuint(float4 pos [[position]]) { uint x = uint(pos.x), y = uint(pos.y); return uint4(x, y, x + y, 3u); }
fragment int4 m5b_fsint(float4 pos [[position]]) { int x = int(pos.x), y = int(pos.y); return int4(x - 8, y - 8, x + y - 16, -1); }
fragment float4 fvals(float4 pos [[position]], constant float4 *v [[buffer(0)]]) { return v[uint(pos.x)]; }
fragment float4 m5b_f2d(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return t.sample(s, pos.xy * u.u0.xy + u.u0.zw, level(u.lod)); }
fragment float4 m5b_f2dimp(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return t.sample(s, pos.xy * u.u0.xy + u.u0.zw, bias(u.bias)); }
fragment float4 m5b_f2darr(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d_array<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return t.sample(s, pos.xy * u.u0.xy + u.u0.zw, u.index, level(u.lod)); }
fragment float4 m5b_f3d(float4 pos [[position]], constant U &u [[buffer(0)]], texture3d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return t.sample(s, float3(pos.xy * u.u0.xy + u.u0.zw, u.u1.x), level(u.lod)); }
fragment float4 m5b_f1d(float4 pos [[position]], constant U &u [[buffer(0)]], texture1d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return t.sample(s, pos.x * u.u0.x + u.u0.z); }
fragment float4 m5b_fcube(float4 pos [[position]], constant U &u [[buffer(0)]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   float3 d = u.u0.xyz + (pos.x / 8.0 - 1.0) * u.u1.xyz + (pos.y / 8.0 - 1.0) * u.u2.xyz; return t.sample(s, d, level(u.lod)); }
fragment float4 m5b_fcubearr(float4 pos [[position]], constant U &u [[buffer(0)]], texturecube_array<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   float3 d = u.u0.xyz + (pos.x / 8.0 - 1.0) * u.u1.xyz + (pos.y / 8.0 - 1.0) * u.u2.xyz; return t.sample(s, d, u.index, level(u.lod)); }
fragment float4 m5b_fread(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]]) {
   return t.read(uint2(pos.xy * u.u0.xy + u.u0.zw), u.index); }
fragment float4 m5b_freadu(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<uint> t [[texture(0)]]) {
   return float4(t.read(uint2(pos.xy * u.u0.xy + u.u0.zw), u.index)); }
fragment float4 m5b_freadi(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<int> t [[texture(0)]]) {
   return float4(t.read(uint2(pos.xy * u.u0.xy + u.u0.zw), u.index)); }
fragment float4 fsu(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<uint> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return float4(t.sample(s, pos.xy * u.u0.xy + u.u0.zw, level(u.lod))); }
fragment float4 fsi(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<int> t [[texture(0)]], sampler s [[sampler(0)]]) {
   return float4(t.sample(s, pos.xy * u.u0.xy + u.u0.zw, level(u.lod))); }
fragment float4 m5b_fmsarr(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d_ms_array<float> t [[texture(0)]]) {
   return t.read(uint2(pos.xy), u.index, u.select); }
fragment float4 m5b_fcmp(float4 pos [[position]], constant U &u [[buffer(0)]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   float r = pos.x * u.ref + u.ref_base;
   return float4(t.sample_compare(s, pos.xy * u.u0.xy + u.u0.zw, r, level(u.lod)), pos.y / 16.0, 0.25, 1.0); }
fragment float4 fdread(float4 pos [[position]], depth2d<float> t [[texture(0)]]) {
   float d = t.read(uint2(pos.xy)); return float4(d, 0.0, 0.0, 1.0); }
fragment float4 fdsample(float4 pos [[position]], constant U &u [[buffer(0)]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
   float d = t.sample(s, pos.xy * u.u0.xy + u.u0.zw, level(u.lod)); return float4(d, 0.0, 0.0, 1.0); }
struct FD { float d [[depth(any)]]; };
fragment FD fdepth(float4 pos [[position]]) { FD o; o.d = pos.x / 16.0; return o; }
constexpr sampler ob0(coord::normalized, address::clamp_to_border, border_color::transparent_black, filter::nearest);
constexpr sampler ob1(coord::normalized, address::clamp_to_border, border_color::opaque_black, filter::nearest);
constexpr sampler ob2(coord::normalized, address::clamp_to_border, border_color::opaque_white, filter::nearest);
fragment float4 m5b_fownb(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]]) {
   float2 c = pos.xy * u.u0.xy + u.u0.zw;
   float4 a = t.sample(ob0, c, level(u.lod)), b = t.sample(ob1, c, level(u.lod)), d = t.sample(ob2, c, level(u.lod));
   return u.select == 0 ? a : (u.select == 1 ? b : d); }
constexpr sampler ol(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest, lod_clamp(1.0, 2.0));
fragment float4 m5b_fownlod(float4 pos [[position]], constant U &u [[buffer(0)]], texture2d<float> t [[texture(0)]]) {
   return t.sample(ol, pos.xy * u.u0.xy + u.u0.zw); }
kernel void krd(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]], uint2 id [[thread_position_in_grid]]) {
   o[id.y * t.get_width() + id.x] = t.read(id); }
kernel void ksmp(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float *us [[buffer(1)]],
                 device float4 *o [[buffer(0)]], uint id [[thread_position_in_grid]]) {
   o[id] = t.sample(s, float2(us[id], 0.5), level(0.0)); }
kernel void kcube(texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *ds [[buffer(1)]],
                  device float4 *o [[buffer(0)]], uint id [[thread_position_in_grid]]) {
   o[id] = t.sample(s, ds[id].xyz, level(0.0)); }
kernel void kwr(texture2d<float, access::write> t [[texture(0)]], uint2 id [[thread_position_in_grid]]) {
   t.write(float4(0.25, 0.5, 0.75, 1.0), id); }
);

/* ---- the harness ---------------------------------------------------------------------------------- */

static id<MTLDevice> D;
static id<MTLCommandQueue> Q;
static id<MTLLibrary> L;
static id<MTLTexture> T16;

__attribute__((format(printf, 1, 2)))
static void
say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fputs("mtlref: ", stdout);
   vprintf(fmt, ap);
   fputc('\n', stdout);
   va_end(ap);
   fflush(stdout);
}

static id<MTLTexture>
mk(MTLTextureType type, MTLPixelFormat f, NSUInteger w, NSUInteger h, NSUInteger d, NSUInteger slices,
   NSUInteger mips, NSUInteger samples, MTLTextureUsage use, MTLStorageMode mode)
{
   MTLTextureDescriptor *td = [MTLTextureDescriptor new];
   td.textureType = type;
   td.pixelFormat = f;
   td.width = w;
   td.height = h;
   td.depth = d;
   td.arrayLength = slices;
   td.mipmapLevelCount = mips;
   td.sampleCount = samples;
   td.usage = use;
   td.storageMode = mode;
   id<MTLTexture> t = [D newTextureWithDescriptor:td];
   if (!t)
      say("texture refused: type %u format %u", (unsigned)type, (unsigned)f);
   return t;
}

#define USE_RD MTLTextureUsageShaderRead
#define USE_RT MTLTextureUsageRenderTarget
#define USE_FV MTLTextureUsagePixelFormatView
#define USE_WR MTLTextureUsageShaderWrite
#define SHARED MTLStorageModeShared
#define PRIVATE MTLStorageModePrivate

static id<MTLTexture>
mk2d(MTLPixelFormat f, NSUInteger w, NSUInteger h, NSUInteger mips, MTLTextureUsage use)
{
   return mk(MTLTextureType2D, f, w, h, 1, 1, mips, 1, use, SHARED);
}

static void
put(id<MTLTexture> t, unsigned level, unsigned slice, unsigned w, unsigned h, unsigned d, unsigned texel, const void *p)
{
   [t replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:level slice:slice withBytes:p
        bytesPerRow:w * texel bytesPerImage:d > 1 ? w * h * texel : 0];
}

static void
rd(id<MTLTexture> t, unsigned level, unsigned slice, unsigned z, unsigned w, unsigned h, unsigned texel, void *out)
{
   [t getBytes:out bytesPerRow:w * texel bytesPerImage:w * h * texel
    fromRegion:MTLRegionMake3D(0, 0, z, w, h, 1) mipmapLevel:level slice:slice];
}

static id<MTLBuffer>
buf(const void *p, size_t n)
{
   return p ? [D newBufferWithBytes:p length:n options:MTLResourceStorageModeShared]
            : [D newBufferWithLength:n options:MTLResourceStorageModeShared];
}

static id<MTLFunction>
fn(NSString *name)
{
   id<MTLFunction> f = [L newFunctionWithName:name];
   if (!f)
      say("no function %s in the library", name.UTF8String);
   return f;
}

static id<MTLRenderPipelineState>
pso2(NSString *vs, NSString *fs, MTLVertexDescriptor *vd, MTLPixelFormat color, const struct dm_blend *b,
     MTLPixelFormat depth, unsigned samples, const char *name)
{
   MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
   rp.vertexFunction = fn(vs);
   if (fs)
      rp.fragmentFunction = fn(fs);
   else
      rp.rasterizationEnabled = NO;
   rp.vertexDescriptor = vd;
   rp.colorAttachments[0].pixelFormat = color;
   rp.depthAttachmentPixelFormat = depth;
   rp.rasterSampleCount = samples;
   if (b) {
      MTLRenderPipelineColorAttachmentDescriptor *c = rp.colorAttachments[0];
      c.blendingEnabled = b->on ? YES : NO;
      c.rgbBlendOperation = (MTLBlendOperation)b->rgb_op;
      c.alphaBlendOperation = (MTLBlendOperation)b->alpha_op;
      c.sourceRGBBlendFactor = (MTLBlendFactor)b->src_rgb;
      c.destinationRGBBlendFactor = (MTLBlendFactor)b->dst_rgb;
      c.sourceAlphaBlendFactor = (MTLBlendFactor)b->src_alpha;
      c.destinationAlphaBlendFactor = (MTLBlendFactor)b->dst_alpha;
      c.writeMask = (MTLColorWriteMask)b->mask;
   }
   NSError *e = nil;
   id<MTLRenderPipelineState> p = [D newRenderPipelineStateWithDescriptor:rp error:&e];
   if (!p)
      say("%s pipeline refused: %s", name, e.localizedDescription.UTF8String);
   return p;
}

static id<MTLRenderPipelineState>
pso(NSString *vs, NSString *fs, MTLVertexDescriptor *vd, MTLPixelFormat color, const char *name)
{
   return pso2(vs, fs, vd, color, NULL, MTLPixelFormatInvalid, 1, name);
}

static MTLRenderPassDescriptor *
rpd(id<MTLTexture> t, unsigned level, unsigned slice, unsigned plane, const float *clear)
{
   MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
   MTLRenderPassColorAttachmentDescriptor *a = pd.colorAttachments[0];
   a.texture = t;
   a.level = level;
   a.slice = slice;
   a.depthPlane = plane;
   a.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
   if (clear)
      a.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
   a.storeAction = t.sampleCount > 1 ? MTLStoreActionStore : MTLStoreActionStore;
   return pd;
}

typedef void (^enc_block)(id<MTLRenderCommandEncoder> e);

static BOOL
run_pass(MTLRenderPassDescriptor *pd, enc_block f)
{
   id<MTLCommandBuffer> c = [Q commandBuffer];
   id<MTLRenderCommandEncoder> e = [c renderCommandEncoderWithDescriptor:pd];
   if (f)
      f(e);
   [e endEncoding];
   [c commit];
   [c waitUntilCompleted];
   if (c.error) {
      say("command buffer error: %s", c.error.localizedDescription.UTF8String);
      return NO;
   }
   return YES;
}

static void
run_compute(NSString *k, NSUInteger gx, NSUInteger gy, void (^bind)(id<MTLComputeCommandEncoder> e))
{
   NSError *err = nil;
   id<MTLComputePipelineState> p = [D newComputePipelineStateWithFunction:fn(k) error:&err];
   if (!p) {
      say("compute pipeline %s refused: %s", k.UTF8String, err.localizedDescription.UTF8String);
      return;
   }
   id<MTLCommandBuffer> c = [Q commandBuffer];
   id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
   [e setComputePipelineState:p];
   bind(e);
   [e dispatchThreads:MTLSizeMake(gx, gy, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
   [e endEncoding];
   [c commit];
   [c waitUntilCompleted];
}

static const float clear0[4] = { 0, 0, 0, 0 };

struct pix_u_t {
   float u0[4], u1[4], u2[4];
   float lod, bias, ref, ref_base;
   uint32_t index, select, pad[2];
};

/* A full-screen strip of this fragment function into `tgt`. */
static BOOL
probe_into(NSString *fs, id<MTLTexture> tgt, unsigned level, unsigned slice, unsigned plane, id<MTLTexture> t,
           id<MTLSamplerState> s, const struct pix_u_t *u, const float *clear, const char *name)
{
   id<MTLRenderPipelineState> p = pso(@"vquad", fs, nil, tgt.pixelFormat, name);
   if (!p)
      return NO;
   return run_pass(rpd(tgt, level, slice, plane, clear), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      if (t)
         [e setFragmentTexture:t atIndex:0];
      if (s)
         [e setFragmentSamplerState:s atIndex:0];
      if (u)
         [e setFragmentBytes:u length:sizeof(*u) atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   });
}

/* Into T16, the 16x16 RGBA32Float attachment every read of the tests goes through. */
static BOOL
probe(NSString *fs, id<MTLTexture> t, id<MTLSamplerState> s, const struct pix_u_t *u, const char *name)
{
   return probe_into(fs, T16, 0, 0, 0, t, s, u, clear0, name);
}

static struct pix_u_t
tx_u(float sx, float sy, float ox, float oy, float lod)
{
   return (struct pix_u_t){ .u0 = { sx, sy, ox, oy }, .lod = lod };
}

static id<MTLSamplerState>
smp(unsigned as, unsigned at, unsigned ar, unsigned minf, unsigned magf, unsigned mip, unsigned cmp, unsigned aniso,
    float lmin, float lmax, unsigned border)
{
   MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
   sd.sAddressMode = (MTLSamplerAddressMode)as;
   sd.tAddressMode = (MTLSamplerAddressMode)at;
   sd.rAddressMode = (MTLSamplerAddressMode)ar;
   sd.minFilter = (MTLSamplerMinMagFilter)minf;
   sd.magFilter = (MTLSamplerMinMagFilter)magf;
   sd.mipFilter = (MTLSamplerMipFilter)mip;
   sd.compareFunction = (MTLCompareFunction)cmp;
   sd.maxAnisotropy = aniso;
   sd.normalizedCoordinates = YES;
   sd.lodMinClamp = lmin;
   sd.lodMaxClamp = lmax;
   sd.borderColor = (MTLSamplerBorderColor)border;
   id<MTLSamplerState> s = [D newSamplerStateWithDescriptor:sd];
   if (!s)
      say("sampler refused");
   return s;
}

static id<MTLSamplerState>
nearest(void)
{
   return smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 1, 0, 1, 0.0f, 1000.0f, 0);
}

/* The model's verdict (draw_model.h, dm_verdict) with this tool's line: the same tolerances. */
static BOOL
judge(const char *name, unsigned fmt, const void *got, const struct dm_picture *want, float extra)
{
   unsigned texel = dm_texel_bytes(fmt), wrong = 0, judged = 0, exact = 0, fx = 0, fy = 0;
   float fg[4] = { 0 }, fw[4] = { 0 }, tol = dm_tolerance(fmt, extra), maxd = 0.0f;
   const uint8_t *g = got;
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         if (!want->care[y][x])
            continue;
         const uint8_t *at = g + ((size_t)y * DM_W + x) * texel;
         uint8_t expected[16];
         float v[4];
         dm_encode(fmt, want->px[y][x], expected);
         if (!memcmp(at, expected, texel))
            exact++;
         dm_decode(fmt, at, v);
         judged++;
         bool same = true;
         for (unsigned c = 0; c < 4; c++) {
            /* NaN against NaN and infinity against itself are the model's value, which a difference
             * cannot say (dm_verdict is never fed these: its rows are finite) */
            if ((isnan(v[c]) && isnan(want->px[y][x][c])) || v[c] == want->px[y][x][c])
               continue;
            float d = fabsf(v[c] - want->px[y][x][c]);
            if (d > maxd)
               maxd = d;
            if (dm_is_srgb(fmt) && c < 3) {
               float lo, hi;
               dm_srgb_band(want->px[y][x][c], extra, &lo, &hi);
               if (!(v[c] >= lo && v[c] <= hi))
                  same = false;
               continue;
            }
            float t = dm_is_srgb(fmt) ? extra + 1.0f / 255.0f + 1e-5f
                    : fmt == DM_RGBA16F ? extra + dm_half_step(want->px[y][x][c]) : tol;
            if (!(d <= t))
               same = false;
         }
         if (!same) {
            if (!wrong) {
               fx = x;
               fy = y;
               memcpy(fg, v, sizeof(fg));
               memcpy(fw, want->px[y][x], sizeof(fw));
            }
            wrong++;
         }
      }
   say("%s %s wrong=%u/%u exact=%u/%u maxdiff=%.6f", name, wrong ? "DIFFER" : "agree", wrong, judged, exact, judged,
       (double)maxd);
   if (wrong)
      say("%s first (%u,%u) metal %.6f %.6f %.6f %.6f model %.6f %.6f %.6f %.6f", name, fx, fy, (double)fg[0],
          (double)fg[1], (double)fg[2], (double)fg[3], (double)fw[0], (double)fw[1], (double)fw[2], (double)fw[3]);
   return !wrong;
}

static BOOL
judge16(const char *name, const struct dm_picture *want, float extra)
{
   static float got[DM_H][DM_W][4];
   rd(T16, 0, 0, 0, DM_W, DM_H, 16, got);
   return judge(name, DM_RGBA32F, got, want, extra);
}

static char
cov(unsigned fmt, const void *got, unsigned x, unsigned y)
{
   float v[4];
   dm_decode(fmt, (const uint8_t *)got + ((size_t)y * DM_W + x) * dm_texel_bytes(fmt), v);
   return (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f) ? '#' : '.';
}

/* One line per row: Metal's, the model's, and a third map if given. */
static void
print_map(const char *name, unsigned fmt, const void *got, const struct dm_picture *want, const char *third_name,
          const char (*third)[DM_W + 1])
{
   for (unsigned y = 0; y < DM_H; y++) {
      char a[DM_W + 1], b[DM_W + 1];
      for (unsigned x = 0; x < DM_W; x++) {
         a[x] = cov(fmt, got, x, y);
         const float *v = want->px[y][x];
         b[x] = !want->care[y][x] ? '?' : (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f) ? '#' : '.';
      }
      a[DM_W] = b[DM_W] = '\0';
      if (third)
         say("%s map row=%02u metal=%s model=%s %s=%s", name, y, a, b, third_name, third[y]);
      else
         say("%s map row=%02u metal=%s model=%s", name, y, a, b);
   }
}

static void
tbytes(const char *name, const void *got, const void *want, unsigned count, unsigned texel, unsigned w)
{
   const uint8_t *g = got, *e = want;
   unsigned wrong = 0, first = 0;
   int maxd = 0;
   for (unsigned i = 0; i < count; i++) {
      if (memcmp(g + (size_t)i * texel, e + (size_t)i * texel, texel)) {
         if (!wrong)
            first = i;
         wrong++;
      }
      for (unsigned b = 0; b < texel; b++) {
         int d = abs((int)g[(size_t)i * texel + b] - (int)e[(size_t)i * texel + b]);
         maxd = d > maxd ? d : maxd;
      }
   }
   say("%s %s wrong=%u/%u maxstep=%d", name, wrong ? "DIFFER" : "agree", wrong, count, maxd);
   if (wrong) {
      char a[64] = "", b[64] = "";
      for (unsigned k = 0; k < texel && k < 16; k++) {
         sprintf(a + strlen(a), " %02x", g[(size_t)first * texel + k]);
         sprintf(b + strlen(b), " %02x", e[(size_t)first * texel + k]);
      }
      say("%s first (%u,%u) metal%s model%s", name, first % w, first / w, a, b);
   }
}

/* ---- air_metal_test.c's geometry ------------------------------------------------------------------ */

struct pix_vertex {
   float pos[4];
   float col[4];
};

static struct pix_vertex
pix_v(float wx, float wy, float r0, float g0, float b0, float a0)
{
   struct pix_vertex v = {
      { wx * 2.0f / DM_W - 1.0f, 1.0f - wy * 2.0f / DM_H, 0.0f, 1.0f }, { r0, g0, b0, a0 },
   };
   return v;
}

static const float pix_tri_win[3][2] = { { 2.0625f, 1.0625f }, { 4.0625f, 12.0625f }, { 13.0625f, 3.0625f } };
static const float pix_tri_col[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
static const struct dm_viewport whole = { 0, 0, DM_W, DM_H };

static void
pix_model_tri(struct dm_picture *p, const struct pix_vertex *a, const struct pix_vertex *b, const struct pix_vertex *c,
              const struct dm_viewport *vp, struct dm_rect rect, unsigned cull, unsigned winding, bool shared)
{
   float w[3][2], col[3][4];
   const struct pix_vertex *v[3] = { a, b, c };
   for (unsigned i = 0; i < 3; i++) {
      dm_window(v[i]->pos, vp, w[i]);
      memcpy(col[i], v[i]->col, sizeof(col[i]));
   }
   if (dm_culled(w, cull, winding))
      return;
   dm_triangle(p, w, col, rect, shared);
}

/* air_metal_test.c, pix_model_line, with its comments there. */
static void
pix_model_line(struct dm_picture *p, const float a[2], const float b[2], const float col[4])
{
   float dx = b[0] - a[0], dy = b[1] - a[1];
   bool xmajor = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
   bool plain = dx == 0.0f || dy == 0.0f || dx == dy || dx == -dy;
   float from = xmajor ? a[0] : a[1], to = xmajor ? b[0] : b[1];
   int lo = (int)(from < to ? from : to) - 1, hi = (int)(from < to ? to : from) + 1;
   for (int m = lo; m <= hi; m++) {
      float at = (float)m + 0.5f;
      float t = (at - from) / (to - from);
      if (t < 0.0f || t >= 1.0f)
         continue;
      float other = (xmajor ? a[1] : a[0]) + t * (xmajor ? dy : dx);
      int cell = (int)other;
      float frac = other - (float)cell;
      int x = xmajor ? m : cell, y = xmajor ? cell : m;
      if (x < 0 || y < 0 || x >= DM_W || y >= DM_H)
         continue;
      float span = (to - from) < 0 ? from - to : to - from;
      bool end = !plain && (t * span < 1.0f || (1.0f - t) * span < 1.0f);
      if (frac == 0.0f || end) {
         p->care[y][x] = 0;
         if (end && xmajor && y + 1 < DM_H)
            p->care[y + 1][x] = 0;
         if (end && xmajor && y > 0)
            p->care[y - 1][x] = 0;
         if (end && !xmajor && x + 1 < DM_W)
            p->care[y][x + 1] = 0;
         if (end && !xmajor && x > 0)
            p->care[y][x - 1] = 0;
         continue;
      }
      memcpy(p->px[y][x], col, 4 * sizeof(float));
   }
   if (!xmajor) {
      const float ends[2] = { a[1], b[1] };
      const float xs[2] = { a[0], b[0] };
      for (unsigned e = 0; e < 2; e++) {
         int row = (int)ends[e], cell = (int)xs[e];
         if ((float)row == ends[e] && row >= 1 && row <= DM_H && cell >= 0 && cell < DM_W)
            p->care[row - 1][cell] = 0;
      }
   }
}

static void
pix_model_point(struct dm_picture *p, const float c[2], float size, const float col[4])
{
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         float px = (float)x + 0.5f, py = (float)y + 0.5f;
         if (px > c[0] - size / 2.0f && px < c[0] + size / 2.0f && py > c[1] - size / 2.0f && py < c[1] + size / 2.0f)
            memcpy(p->px[y][x], col, 4 * sizeof(float));
      }
}

/* The diamond-exit rule, as a third opinion on a line (this tool's, no header has it): a pixel is the
 * line's if the segment passes through the open diamond |dx| + |dy| < 1/2 around its centre and leaves it
 * (reaches its edge) by the segment's end. */
static bool
diamond_covers(const float a[2], const float b[2], int x, int y)
{
   double ax = (double)a[0] - (x + 0.5), ay = (double)a[1] - (y + 0.5);
   double dx = (double)b[0] - (double)a[0], dy = (double)b[1] - (double)a[1];
   double lo = -INFINITY, hi = INFINITY;
   static const int sg[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
   for (unsigned i = 0; i < 4; i++) {
      double p = sg[i][0] * dx + sg[i][1] * dy, q = 0.5 - (sg[i][0] * ax + sg[i][1] * ay);
      if (p == 0.0) {
         if (q <= 0.0)
            return false;
      } else if (p > 0.0) {
         hi = fmin(hi, q / p);
      } else {
         lo = fmax(lo, q / p);
      }
   }
   if (!(lo < hi))
      return false;
   if (!(fmax(lo, 0.0) < fmin(hi, 1.0)))
      return false;
   return hi <= 1.0;
}

static MTLVertexDescriptor *
vd_vattr(void)
{
   MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
   vd.attributes[0].format = MTLVertexFormatFloat4;
   vd.attributes[0].offset = 0;
   vd.attributes[0].bufferIndex = 0;
   vd.attributes[1].format = MTLVertexFormatFloat4;
   vd.attributes[1].offset = 16;
   vd.attributes[1].bufferIndex = 0;
   vd.layouts[0].stride = sizeof(struct pix_vertex);
   vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
   vd.layouts[0].stepRate = 1;
   return vd;
}

static BOOL
draw_mesh(const struct pix_vertex *m, unsigned n, MTLPrimitiveType prim, int cull, int wind, const MTLViewport *vp,
          const MTLScissorRect *sc, const char *name)
{
   id<MTLRenderPipelineState> p = pso(@"m5a_vattr", @"m5a_fcolor", vd_vattr(), MTLPixelFormatRGBA32Float, name);
   if (!p)
      return NO;
   id<MTLBuffer> vb = buf(m, n * sizeof(*m));
   return run_pass(rpd(T16, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      [e setVertexBuffer:vb offset:0 atIndex:0];
      if (cull >= 0)
         [e setCullMode:(MTLCullMode)cull];
      if (wind >= 0)
         [e setFrontFacingWinding:(MTLWinding)wind];
      if (vp)
         [e setViewport:*vp];
      if (sc)
         [e setScissorRect:*sc];
      [e drawPrimitives:prim vertexStart:0 vertexCount:n];
   });
}

/* ---- the GPU, and Apple's numbers against the models' --------------------------------------------- */

static void
case_gpu(int arg)
{
   (void)arg;
   say("gpu name=\"%s\" os=\"%s\"", D.name.UTF8String, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
   static const struct { const char *n; MTLGPUFamily f; } fam[] = {
      { "Apple1", MTLGPUFamilyApple1 }, { "Apple2", MTLGPUFamilyApple2 }, { "Apple3", MTLGPUFamilyApple3 },
      { "Apple4", MTLGPUFamilyApple4 }, { "Apple5", MTLGPUFamilyApple5 }, { "Apple6", MTLGPUFamilyApple6 },
      { "Apple7", MTLGPUFamilyApple7 }, { "Apple8", MTLGPUFamilyApple8 }, { "Apple9", MTLGPUFamilyApple9 },
      { "Metal3", MTLGPUFamilyMetal3 },
   };
   char line[256] = "";
   for (unsigned i = 0; i < COUNT(fam); i++)
      if ([D supportsFamily:fam[i].f])
         sprintf(line + strlen(line), " %s", fam[i].n);
   say("gpu families%s", line);
   say("gpu unified=%d bc=%d rwtier=%d floatfilter32=%d msaa32=%d depth24s8=%d lowpower=%d", D.hasUnifiedMemory,
       D.supportsBCTextureCompression, (int)D.readWriteTextureSupport, D.supports32BitFloatFiltering,
       D.supports32BitMSAA, D.isDepth24Stencil8PixelFormatSupported, D.isLowPower);
}

static void
case_enums(int arg)
{
   (void)arg;
#define F(x) { #x, MTLPixelFormat##x }
   static const struct { const char *n; unsigned v; } pf[] = {
      F(A8Unorm), F(R8Unorm), F(R8Unorm_sRGB), F(R8Snorm), F(R8Uint), F(R8Sint), F(R16Unorm), F(R16Snorm), F(R16Uint),
      F(R16Sint), F(R16Float), F(RG8Unorm), F(RG8Unorm_sRGB), F(RG8Snorm), F(RG8Uint), F(RG8Sint), F(B5G6R5Unorm),
      F(A1BGR5Unorm), F(ABGR4Unorm), F(BGR5A1Unorm), F(R32Uint), F(R32Sint), F(R32Float), F(RG16Unorm), F(RG16Snorm),
      F(RG16Uint), F(RG16Sint), F(RG16Float), F(RGBA8Unorm), F(RGBA8Unorm_sRGB), F(RGBA8Snorm), F(RGBA8Uint), F(RGBA8Sint),
      F(BGRA8Unorm), F(BGRA8Unorm_sRGB), F(RGB10A2Unorm), F(RGB10A2Uint), F(RG11B10Float), F(RGB9E5Float), F(BGR10A2Unorm),
      F(RG32Uint), F(RG32Sint), F(RG32Float), F(RGBA16Unorm), F(RGBA16Snorm), F(RGBA16Uint), F(RGBA16Sint), F(RGBA16Float),
      F(RGBA32Uint), F(RGBA32Sint), F(RGBA32Float),
   };
#undef F
   unsigned checked = 0, wrong = 0;
   for (unsigned i = 0; i < COUNT(tm_formats); i++) {
      bool found = false;
      for (unsigned k = 0; k < COUNT(pf); k++)
         if (!strcmp(pf[k].n, tm_formats[i].name)) {
            found = true;
            checked++;
            if (pf[k].v != tm_formats[i].mtl) {
               wrong++;
               say("enums DIFFER format %s model %u metal %u", pf[k].n, tm_formats[i].mtl, pf[k].v);
            }
         }
      if (!found)
         say("enums no-such-name %s", tm_formats[i].name);
   }
#define E(m, a) do { checked++; if ((unsigned)(m) != (unsigned)(a)) { wrong++; say("enums DIFFER %s model %u metal %u", #a, (unsigned)(m), (unsigned)(a)); } } while (0)
   E(DM_ZERO, MTLBlendFactorZero); E(DM_ONE, MTLBlendFactorOne); E(DM_SC, MTLBlendFactorSourceColor);
   E(DM_ISC, MTLBlendFactorOneMinusSourceColor); E(DM_SA, MTLBlendFactorSourceAlpha);
   E(DM_ISA, MTLBlendFactorOneMinusSourceAlpha); E(DM_DC, MTLBlendFactorDestinationColor);
   E(DM_IDC, MTLBlendFactorOneMinusDestinationColor); E(DM_DA, MTLBlendFactorDestinationAlpha);
   E(DM_IDA, MTLBlendFactorOneMinusDestinationAlpha); E(DM_SAS, MTLBlendFactorSourceAlphaSaturated);
   E(DM_BC, MTLBlendFactorBlendColor); E(DM_IBC, MTLBlendFactorOneMinusBlendColor); E(DM_BA, MTLBlendFactorBlendAlpha);
   E(DM_IBA, MTLBlendFactorOneMinusBlendAlpha);
   E(DM_ADD, MTLBlendOperationAdd); E(DM_SUB, MTLBlendOperationSubtract); E(DM_RSUB, MTLBlendOperationReverseSubtract);
   E(DM_MIN, MTLBlendOperationMin); E(DM_MAX, MTLBlendOperationMax);
   E(DM_R, MTLColorWriteMaskRed); E(DM_G, MTLColorWriteMaskGreen); E(DM_B, MTLColorWriteMaskBlue); E(DM_A, MTLColorWriteMaskAlpha);
   E(DM_CULL_NONE, MTLCullModeNone); E(DM_CULL_FRONT, MTLCullModeFront); E(DM_CULL_BACK, MTLCullModeBack);
   E(DM_CW, MTLWindingClockwise); E(DM_CCW, MTLWindingCounterClockwise);
   E(DM_RGBA8, MTLPixelFormatRGBA8Unorm); E(DM_RGBA8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB); E(DM_BGRA8, MTLPixelFormatBGRA8Unorm);
   E(DM_BGRA8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB); E(DM_RGBA16F, MTLPixelFormatRGBA16Float); E(DM_RGBA32F, MTLPixelFormatRGBA32Float);
   E(TM_EDGE, MTLSamplerAddressModeClampToEdge); E(TM_MIRROR_EDGE, MTLSamplerAddressModeMirrorClampToEdge);
   E(TM_REPEAT, MTLSamplerAddressModeRepeat); E(TM_MIRROR, MTLSamplerAddressModeMirrorRepeat);
   E(TM_ZERO, MTLSamplerAddressModeClampToZero); E(TM_BORDER, MTLSamplerAddressModeClampToBorderColor);
   E(0, MTLCompareFunctionNever); E(1, MTLCompareFunctionLess); E(2, MTLCompareFunctionEqual); E(3, MTLCompareFunctionLessEqual);
   E(4, MTLCompareFunctionGreater); E(5, MTLCompareFunctionNotEqual); E(6, MTLCompareFunctionGreaterEqual); E(7, MTLCompareFunctionAlways);
   /* air_metal_test.c's numbers */
   E(0, MTLTextureSwizzleZero); E(1, MTLTextureSwizzleOne); E(2, MTLTextureSwizzleRed); E(3, MTLTextureSwizzleGreen);
   E(4, MTLTextureSwizzleBlue); E(5, MTLTextureSwizzleAlpha);
   E(0, MTLTextureType1D); E(1, MTLTextureType1DArray); E(2, MTLTextureType2D); E(3, MTLTextureType2DArray);
   E(4, MTLTextureType2DMultisample); E(5, MTLTextureTypeCube); E(6, MTLTextureTypeCubeArray); E(7, MTLTextureType3D);
   E(8, MTLTextureType2DMultisampleArray);
   E(0, MTLSamplerBorderColorTransparentBlack); E(1, MTLSamplerBorderColorOpaqueBlack); E(2, MTLSamplerBorderColorOpaqueWhite);
   E(170, MTLPixelFormatEAC_R11Unorm); E(174, MTLPixelFormatEAC_RG11Unorm); E(178, MTLPixelFormatEAC_RGBA8);
   E(179, MTLPixelFormatEAC_RGBA8_sRGB); E(180, MTLPixelFormatETC2_RGB8); E(181, MTLPixelFormatETC2_RGB8_sRGB);
   E(182, MTLPixelFormatETC2_RGB8A1); E(183, MTLPixelFormatETC2_RGB8A1_sRGB); E(204, MTLPixelFormatASTC_4x4_LDR);
   E(186, MTLPixelFormatASTC_4x4_sRGB); E(212, MTLPixelFormatASTC_8x8_LDR); E(218, MTLPixelFormatASTC_12x12_LDR);
   E(207, MTLPixelFormatASTC_6x5_LDR);
   E(252, MTLPixelFormatDepth32Float); E(260, MTLPixelFormatDepth32Float_Stencil8); E(261, MTLPixelFormatX32_Stencil8);
   E(0, MTLPrimitiveTypePoint); E(1, MTLPrimitiveTypeLine); E(2, MTLPrimitiveTypeLineStrip); E(3, MTLPrimitiveTypeTriangle);
   E(4, MTLPrimitiveTypeTriangleStrip);
#undef E
   say("enums %s checked=%u wrong=%u", wrong ? "DIFFER" : "agree", checked, wrong);
}

/* ---- 1: the asymmetric triangle, cull and winding ------------------------------------------------- */

static void
make_tri(struct pix_vertex tri[3])
{
   for (unsigned i = 0; i < 3; i++)
      tri[i] = pix_v(pix_tri_win[i][0], pix_tri_win[i][1], pix_tri_col[i][0], pix_tri_col[i][1], pix_tri_col[i][2],
                     pix_tri_col[i][3]);
}

static void
case_orient(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static float got[DM_H][DM_W][4];
   struct pix_vertex tri[3];
   make_tri(tri);
   const float black[4] = { 0, 0, 0, 0 };
   if (draw_mesh(tri, 3, MTLPrimitiveTypeTriangle, -1, -1, NULL, NULL, "orient-asymmetric")) {
      dm_clear(&want, black);
      pix_model_tri(&want, &tri[0], &tri[1], &tri[2], &whole, dm_whole(), 0, 0, false);
      judge16("orient-asymmetric", &want, 1.0f / 512.0f);
      rd(T16, 0, 0, 0, DM_W, DM_H, 16, got);
      print_map("orient-asymmetric", DM_RGBA32F, got, &want, NULL, NULL);
      int x0 = 99, x1 = -1, y0 = 99, y1 = -1, count = 0;
      for (int y = 0; y < DM_H; y++)
         for (int x = 0; x < DM_W; x++)
            if (cov(DM_RGBA32F, got, x, y) == '#') {
               x0 = x < x0 ? x : x0;
               x1 = x > x1 ? x : x1;
               y0 = y < y0 ? y : y0;
               y1 = y > y1 ? y : y1;
               count++;
            }
      say("orient-asymmetric bbox x=%d..%d y=%d..%d pixels=%d (the model's corners: top left (2,1), down (4,12), right (13,3))",
          x0, x1, y0, y1, count);
      /* which end of the triangle is which colour: vertex 0 red at the top left, 1 green at the bottom, 2 blue at the right */
      say("orient-asymmetric colours (3,2)=%.3f %.3f %.3f (4,10)=%.3f %.3f %.3f (11,3)=%.3f %.3f %.3f", (double)got[2][3][0],
          (double)got[2][3][1], (double)got[2][3][2], (double)got[10][4][0], (double)got[10][4][1], (double)got[10][4][2],
          (double)got[3][11][0], (double)got[3][11][1], (double)got[3][11][2]);
   }
   static const char *const cn[] = { "none", "front", "back" };
   static const char *const wn[] = { "cw", "ccw" };
   const struct pix_vertex tricw[3] = { tri[0], tri[2], tri[1] };
   unsigned bad = 0;
   for (unsigned tw = 0; tw < 2; tw++) {
      const struct pix_vertex *m = tw ? tri : tricw;
      for (unsigned cull = 0; cull < 3; cull++)
         for (unsigned fw = 0; fw < 2; fw++) {
            char name[64];
            snprintf(name, sizeof(name), "orient-%stri-cull%s-front%s", wn[tw], cn[cull], wn[fw]);
            if (!draw_mesh(m, 3, MTLPrimitiveTypeTriangle, (int)cull, (int)fw, NULL, NULL, name))
               continue;
            dm_clear(&want, black);
            pix_model_tri(&want, &m[0], &m[1], &m[2], &whole, dm_whole(), cull, fw, false);
            rd(T16, 0, 0, 0, DM_W, DM_H, 16, got);
            int drawn = 0, model = 0;
            for (int y = 0; y < DM_H; y++)
               for (int x = 0; x < DM_W; x++) {
                  drawn |= cov(DM_RGBA32F, got, x, y) == '#';
                  model |= want.px[y][x][3] != 0.0f;
               }
            say("%s drawn metal=%d model=%d %s", name, drawn, model, drawn == model ? "agree" : "DIFFER");
            bad += drawn != model;
            judge(name, DM_RGBA32F, got, &want, 1.0f / 512.0f);
         }
   }
   say("orient-cull %s wrong=%u of 12", bad ? "DIFFER" : "agree", bad);
   /* A y-flipping viewport (height negative), in Metal's own API: dm_window and dm_culled's claim that
    * the winding is the one seen in window coordinates. */
   {
      MTLViewport vp = { 0, 16, 16, -16, 0, 1 };
      struct dm_viewport mv = { 0, 16, 16, -16 };
      for (unsigned cull = 0; cull < 3; cull++) {
         char name[64];
         snprintf(name, sizeof(name), "orient-negviewport-cull%s-frontcw", cn[cull]);
         if (!draw_mesh(tri, 3, MTLPrimitiveTypeTriangle, (int)cull, 0, &vp, NULL, name))
            continue;
         dm_clear(&want, black);
         pix_model_tri(&want, &tri[0], &tri[1], &tri[2], &mv, dm_whole(), cull, 0, false);
         judge16(name, &want, 1.0f / 512.0f);
      }
   }
}

/* The viewport and scissor rows of --draw-vertex --pixels: dm_window. */
static void
case_viewport(int arg)
{
   (void)arg;
   static struct dm_picture want;
   const float black[4] = { 0, 0, 0, 0 };
   struct pix_vertex quad[4];
   for (unsigned i = 0; i < 4; i++)
      quad[i] = pix_v((i & 1) ? DM_W : 0, (i & 2) ? DM_H : 0, (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.5f, 1.0f);
   static const struct { const char *name; float vp[4]; unsigned sc[4]; bool has_vp, has_sc; } rects[] = {
      { "viewport-origin", { 4, 2, 8, 8 }, { 0 }, true, false },
      { "viewport-lower-right", { 8, 8, 8, 8 }, { 0 }, true, false },
      { "viewport-wide", { 1, 11, 14, 4 }, { 0 }, true, false },
      { "scissor", { 0 }, { 3, 5, 7, 4 }, false, true },
      { "scissor-corner", { 0 }, { 0, 12, 4, 4 }, false, true },
      { "viewport-and-scissor", { 2, 2, 12, 12 }, { 6, 0, 4, 16 }, true, true },
   };
   for (unsigned c = 0; c < COUNT(rects); c++) {
      struct dm_viewport vp = whole;
      struct dm_rect sc = dm_whole();
      MTLViewport mvp = { rects[c].vp[0], rects[c].vp[1], rects[c].vp[2], rects[c].vp[3], 0, 1 };
      MTLScissorRect msc = { rects[c].sc[0], rects[c].sc[1], rects[c].sc[2], rects[c].sc[3] };
      if (rects[c].has_vp)
         vp = (struct dm_viewport){ rects[c].vp[0], rects[c].vp[1], rects[c].vp[2], rects[c].vp[3] };
      if (rects[c].has_sc)
         sc = (struct dm_rect){ (int)rects[c].sc[0], (int)rects[c].sc[1], (int)(rects[c].sc[0] + rects[c].sc[2]),
                                (int)(rects[c].sc[1] + rects[c].sc[3]) };
      if (!draw_mesh(quad, 4, MTLPrimitiveTypeTriangleStrip, -1, -1, rects[c].has_vp ? &mvp : NULL,
                     rects[c].has_sc ? &msc : NULL, rects[c].name))
         continue;
      dm_clear(&want, black);
      pix_model_tri(&want, &quad[0], &quad[1], &quad[2], &vp, sc, 0, 0, true);
      pix_model_tri(&want, &quad[2], &quad[1], &quad[3], &vp, sc, 0, 0, true);
      judge16(rects[c].name, &want, 1.0f / 512.0f);
   }
}

/* ---- 2: lines and points --------------------------------------------------------------------------- */

static void
judge_lines(const char *name, const float (*seg)[2][2], unsigned nseg, const struct dm_picture *want)
{
   static float got[DM_H][DM_W][4];
   char dia[DM_H][DM_W + 1];
   rd(T16, 0, 0, 0, DM_W, DM_H, 16, got);
   unsigned dwrong = 0;
   for (unsigned y = 0; y < DM_H; y++) {
      for (unsigned x = 0; x < DM_W; x++) {
         bool c = false;
         for (unsigned i = 0; i < nseg; i++)
            c = c || diamond_covers(seg[i][0], seg[i][1], (int)x, (int)y);
         dia[y][x] = c ? '#' : '.';
         if ((cov(DM_RGBA32F, got, x, y) == '#') != c)
            dwrong++;
      }
      dia[y][DM_W] = '\0';
   }
   judge(name, DM_RGBA32F, got, want, 1.0f / 512.0f);
   say("%s against-diamond-exit %s wrong=%u/256", name, dwrong ? "DIFFER" : "agree", dwrong);
   print_map(name, DM_RGBA32F, got, want, "diamond", dia);
}

static void
case_lines(int arg)
{
   (void)arg;
   static struct dm_picture want;
   const float black[4] = { 0, 0, 0, 0 };
   static const struct { float a[2], b[2], c[4]; } ls[4] = {
      { { 2.0f, 2.5f }, { 11.0f, 2.5f }, { 1, 0, 0, 1 } },
      { { 14.5f, 3.0f }, { 14.5f, 15.0f }, { 0, 1, 0, 1 } },
      { { 2.0f, 5.0f }, { 10.0f, 13.0f }, { 0, 0, 1, 1 } },
      { { 1.0f, 15.0f }, { 14.0f, 10.625f }, { 1, 1, 0, 1 } },
   };
   static const char *const ln[4] = { "horizontal", "vertical", "diagonal", "shallow" };
   struct pix_vertex v[8];
   float seg[4][2][2];
   for (unsigned i = 0; i < 4; i++) {
      v[i * 2] = pix_v(ls[i].a[0], ls[i].a[1], ls[i].c[0], ls[i].c[1], ls[i].c[2], ls[i].c[3]);
      v[i * 2 + 1] = pix_v(ls[i].b[0], ls[i].b[1], ls[i].c[0], ls[i].c[1], ls[i].c[2], ls[i].c[3]);
      memcpy(seg[i][0], ls[i].a, 8);
      memcpy(seg[i][1], ls[i].b, 8);
   }
   if (draw_mesh(v, 8, MTLPrimitiveTypeLine, -1, -1, NULL, NULL, "prim-linelist")) {
      dm_clear(&want, black);
      for (unsigned i = 0; i < 4; i++)
         pix_model_line(&want, ls[i].a, ls[i].b, ls[i].c);
      judge_lines("prim-linelist", seg, 4, &want);
   }
   /* each by itself, so that a line's ends are its own */
   for (unsigned i = 0; i < 4; i++) {
      char name[64];
      snprintf(name, sizeof(name), "prim-line-%s", ln[i]);
      if (draw_mesh(v + i * 2, 2, MTLPrimitiveTypeLine, -1, -1, NULL, NULL, name)) {
         dm_clear(&want, black);
         pix_model_line(&want, ls[i].a, ls[i].b, ls[i].c);
         judge_lines(name, seg + i, 1, &want);
      }
      /* and the same line the other way round: an end that moves with the direction is a rule of ends */
      struct pix_vertex rev[2] = { v[i * 2 + 1], v[i * 2] };
      snprintf(name, sizeof(name), "prim-line-%s-reversed", ln[i]);
      float rseg[1][2][2];
      memcpy(rseg[0][0], ls[i].b, 8);
      memcpy(rseg[0][1], ls[i].a, 8);
      if (draw_mesh(rev, 2, MTLPrimitiveTypeLine, -1, -1, NULL, NULL, name)) {
         dm_clear(&want, black);
         pix_model_line(&want, ls[i].b, ls[i].a, ls[i].c);
         judge_lines(name, rseg, 1, &want);
      }
   }
   /* not the test's: lines exactly between two rows or two columns, and ends on a pixel's edge, where a
    * rule of corners is all that decides. The model leaves each of these unjudged. */
   static const struct { const char *n; float a[2], b[2]; } ties[] = {
      { "tie-horizontal-y3", { 2, 3 }, { 11, 3 } },
      { "tie-vertical-x3", { 3, 2 }, { 3, 11 } },
      { "tie-diagonal", { 2, 2 }, { 9, 9 } },
      { "tie-antidiagonal", { 2, 9 }, { 9, 2 } },
   };
   for (unsigned i = 0; i < COUNT(ties); i++)
      for (unsigned dir = 0; dir < 2; dir++) {
         char name[64];
         const float *from = dir ? ties[i].b : ties[i].a, *to = dir ? ties[i].a : ties[i].b;
         snprintf(name, sizeof(name), "prim-line-%s%s", ties[i].n, dir ? "-reversed" : "");
         struct pix_vertex tv[2] = { pix_v(from[0], from[1], 1, 1, 1, 1), pix_v(to[0], to[1], 1, 1, 1, 1) };
         float tseg[1][2][2];
         memcpy(tseg[0][0], from, 8);
         memcpy(tseg[0][1], to, 8);
         if (draw_mesh(tv, 2, MTLPrimitiveTypeLine, -1, -1, NULL, NULL, name)) {
            dm_clear(&want, black);
            pix_model_line(&want, from, to, (const float[4]){ 1, 1, 1, 1 });
            judge_lines(name, tseg, 1, &want);
         }
      }
   static const float st[4][2] = { { 2, 3.5f }, { 12, 3.5f }, { 12.5f, 13 }, { 3, 13.5f } };
   struct pix_vertex lv[4];
   static const float one[4] = { 1, 0, 1, 1 };
   float sseg[3][2][2];
   for (unsigned i = 0; i < 4; i++)
      lv[i] = pix_v(st[i][0], st[i][1], one[0], one[1], one[2], one[3]);
   for (unsigned i = 0; i < 3; i++) {
      memcpy(sseg[i][0], st[i], 8);
      memcpy(sseg[i][1], st[i + 1], 8);
   }
   if (draw_mesh(lv, 4, MTLPrimitiveTypeLineStrip, -1, -1, NULL, NULL, "prim-linestrip")) {
      dm_clear(&want, black);
      for (unsigned i = 0; i < 3; i++)
         pix_model_line(&want, st[i], st[i + 1], one);
      judge_lines("prim-linestrip", sseg, 3, &want);
   }
}

static void
draw_points(const char *name, const float (*at)[2], const float *size, const float (*col)[4], unsigned n)
{
   static struct dm_picture want;
   static float got[DM_H][DM_W][4];
   struct { struct pix_vertex v; float size, pad[3]; } pv[8];
   for (unsigned i = 0; i < n; i++) {
      pv[i].v = pix_v(at[i][0], at[i][1], col[i][0], col[i][1], col[i][2], col[i][3]);
      pv[i].size = size[i];
      pv[i].pad[0] = pv[i].pad[1] = pv[i].pad[2] = 0.0f;
   }
   MTLVertexDescriptor *vd = vd_vattr();
   vd.attributes[2].format = MTLVertexFormatFloat;
   vd.attributes[2].offset = 32;
   vd.attributes[2].bufferIndex = 0;
   vd.layouts[0].stride = sizeof(pv[0]);
   id<MTLRenderPipelineState> p = pso(@"m5a_vpoint", @"m5a_fcolor", vd, MTLPixelFormatRGBA32Float, name);
   if (!p)
      return;
   id<MTLBuffer> vb = buf(pv, n * sizeof(pv[0]));
   if (!run_pass(rpd(T16, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
          [e setRenderPipelineState:p];
          [e setVertexBuffer:vb offset:0 atIndex:0];
          [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:n];
       }))
      return;
   dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned i = 0; i < n; i++)
      pix_model_point(&want, at[i], size[i], col[i]);
   rd(T16, 0, 0, 0, DM_W, DM_H, 16, got);
   judge(name, DM_RGBA32F, got, &want, 1.0f / 512.0f);
   print_map(name, DM_RGBA32F, got, &want, NULL, NULL);
}

static void
case_points(int arg)
{
   (void)arg;
   static const float at[3][2] = { { 2.5f, 2.5f }, { 7.5f, 12.5f }, { 12.5f, 5.5f } };
   static const float size[3] = { 1.0f, 3.0f, 5.0f };
   static const float col[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
   draw_points("prim-points", at, size, col, 3);
   /* not the test's: an even size, where a centre is exactly on the square's edge, which is where the
    * model's two strict comparisons are a claim */
   static const float at2[3][2] = { { 3.5f, 3.5f }, { 8.0f, 8.0f }, { 12.0f, 3.0f } };
   static const float size2[3] = { 2.0f, 2.0f, 4.0f };
   draw_points("prim-points-even", at2, size2, col, 3);
}

/* ---- 3: primitive restart --------------------------------------------------------------------------- */

static id<MTLBuffer> rs_pos;

static void
rs_draw(id<MTLTexture> t, MTLPrimitiveType prim, const uint32_t *const *lists, const unsigned *counts, unsigned nlists,
        unsigned width, id<MTLRenderPipelineState> p)
{
   const uint32_t n = 65536;
   NSMutableArray *bufs = [NSMutableArray array];
   for (unsigned l = 0; l < nlists; l++) {
      uint16_t i16[16];
      for (unsigned i = 0; i < counts[l]; i++)
         i16[i] = (uint16_t)lists[l][i];
      [bufs addObject:width == 16 ? buf(i16, counts[l] * 2) : buf(lists[l], counts[l] * 4)];
   }
   run_pass(rpd(t, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      [e setVertexBuffer:rs_pos offset:0 atIndex:0];
      [e setVertexBytes:&n length:4 atIndex:1];
      for (unsigned l = 0; l < nlists; l++)
         [e drawIndexedPrimitives:prim indexCount:counts[l]
                        indexType:width == 16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:bufs[l] indexBufferOffset:0];
   });
}

static int
pair_cmp(const void *a, const void *b)
{
   return memcmp(a, b, 8) == 0 ? 0 : (((const uint32_t *)a)[0] != ((const uint32_t *)b)[0]
          ? (((const uint32_t *)a)[0] < ((const uint32_t *)b)[0] ? -1 : 1)
          : (((const uint32_t *)a)[1] < ((const uint32_t *)b)[1] ? -1 : 1));
}

/* A vertex stage with no rasterizer that writes down what it was given; `out` gets the distinct pairs, sorted. */
static unsigned
record(NSString *vs, MTLVertexDescriptor *vd, enc_block draw, uint32_t (*out)[2], unsigned cap, const char *name)
{
   static id<MTLTexture> rt;
   if (!rt)
      rt = mk2d(MTLPixelFormatRGBA8Unorm, 1, 1, 1, USE_RT);
   id<MTLRenderPipelineState> p = pso2(vs, nil, vd, MTLPixelFormatRGBA8Unorm, NULL, MTLPixelFormatInvalid, 1, name);
   if (!p)
      return 0;
   id<MTLBuffer> nb = buf(NULL, 4), rb = buf(NULL, cap * 8);
   memset(nb.contents, 0, 4);
   uint32_t c = cap;
   run_pass(rpd(rt, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      [e setVertexBuffer:nb offset:0 atIndex:0];
      [e setVertexBuffer:rb offset:0 atIndex:1];
      [e setVertexBytes:&c length:4 atIndex:2];
      draw(e);
   });
   unsigned total = *(uint32_t *)nb.contents;
   unsigned m = total < cap ? total : cap;
   memcpy(out, rb.contents, m * 8);
   qsort(out, m, 8, pair_cmp);
   unsigned u = 0;
   for (unsigned i = 0; i < m; i++)
      if (!u || memcmp(out[u - 1], out[i], 8))
         memcpy(out[u++], out[i], 8);
   return u;
}

static void
case_restart(int arg)
{
   (void)arg;
   float (*pos)[4] = calloc(65536, 16);
   static const float corner[8][2] = { { 1, 1 }, { 6, 1 }, { 1, 6 }, { 6, 6 }, { 9, 9 }, { 15, 9 }, { 9, 14 }, { 15, 14 } };
   for (unsigned i = 0; i < 65536; i++) {
      pos[i][0] = 2.0f;   /* off the attachment */
      pos[i][1] = 2.0f;
      pos[i][3] = 1.0f;
   }
   for (unsigned i = 0; i < 8; i++)
      memcpy(pos[i], pix_v(corner[i][0], corner[i][1], 0, 0, 0, 0).pos, 16);
   /* the vertex an ordinary reading of the index would be, where a restart has nothing */
   memcpy(pos[65535], pix_v(15.5f, 1.5f, 0, 0, 0, 0).pos, 16);
   rs_pos = buf(pos, 65536 * 16);
   free(pos);
   id<MTLRenderPipelineState> p = pso(@"vrestart", @"m5a_fcolor", nil, MTLPixelFormatRGBA8Unorm, "restart");
   if (!p)
      return;
   id<MTLTexture> t = mk2d(MTLPixelFormatRGBA8Unorm, DM_W, DM_H, 1, USE_RT | USE_RD);
   const uint32_t R = 0xdeadbeef;   /* where the restart index goes */
   static const struct {
      const char *name;
      MTLPrimitiveType prim;
      uint32_t all[9];
      unsigned n;
      uint32_t a[4], b[4];
      unsigned na, nb;
   } T[] = {
      { "trilist", MTLPrimitiveTypeTriangle, { 0, 1, 2, 0xdeadbeef, 4, 5, 6 }, 7, { 0, 1, 2 }, { 4, 5, 6 }, 3, 3 },
      { "tristrip", MTLPrimitiveTypeTriangleStrip, { 0, 1, 2, 3, 0xdeadbeef, 4, 5, 6, 7 }, 9, { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, 4, 4 },
      { "linelist", MTLPrimitiveTypeLine, { 0, 1, 0xdeadbeef, 4, 5 }, 5, { 0, 1 }, { 4, 5 }, 2, 2 },
      { "linestrip", MTLPrimitiveTypeLineStrip, { 0, 1, 2, 0xdeadbeef, 4, 5, 6 }, 7, { 0, 1, 2 }, { 4, 5, 6 }, 3, 3 },
      { "pointlist", MTLPrimitiveTypePoint, { 0, 0xdeadbeef, 4 }, 3, { 0 }, { 4 }, 1, 1 },
   };
   static uint8_t restart_px[DM_W * DM_H * 4], two_px[DM_W * DM_H * 4], ord_px[DM_W * DM_H * 4];
   for (unsigned i = 0; i < COUNT(T); i++)
      for (unsigned width = 16; width <= 32; width += 16) {
         char name[64];
         snprintf(name, sizeof(name), "restart-%s-%u", T[i].name, width);
         uint32_t idx[9], ord[9];
         for (unsigned k = 0; k < T[i].n; k++) {
            idx[k] = T[i].all[k] == R ? (width == 16 ? 0xffffu : 0xffffffffu) : T[i].all[k];
            ord[k] = T[i].all[k] == R ? 0xffffu : T[i].all[k];   /* in 32 bits 0xffff is a vertex like any other */
         }
         const uint32_t *l1[1] = { idx };
         unsigned c1[1] = { T[i].n };
         rs_draw(t, T[i].prim, l1, c1, 1, width, p);
         rd(t, 0, 0, 0, DM_W, DM_H, 4, restart_px);
         const uint32_t *l2[2] = { T[i].a, T[i].b };
         unsigned c2[2] = { T[i].na, T[i].nb };
         rs_draw(t, T[i].prim, l2, c2, 2, width, p);
         rd(t, 0, 0, 0, DM_W, DM_H, 4, two_px);
         const uint32_t *l3[1] = { ord };
         rs_draw(t, T[i].prim, l3, c1, 1, 32, p);
         rd(t, 0, 0, 0, DM_W, DM_H, 4, ord_px);
         int same2 = !memcmp(restart_px, two_px, sizeof(two_px)), sameo = !memcmp(restart_px, ord_px, sizeof(ord_px));
         int distinct = memcmp(two_px, ord_px, sizeof(two_px)) != 0;
         say("%s index=0x%x same-as-two-draws=%d same-as-ordinary-vertex=%d two-readings-differ=%d verdict=%s", name,
             width == 16 ? 0xffffu : 0xffffffffu, same2, sameo, distinct,
             same2 && distinct ? "RESTARTS" : sameo && distinct ? "NO-RESTART" : "other");
         for (unsigned y = 0; y < DM_H; y++) {
            char a[DM_W + 1];
            for (unsigned x = 0; x < DM_W; x++)
               a[x] = restart_px[(y * DM_W + x) * 4 + 3] ? '#' : '.';
            a[DM_W] = 0;
            say("%s map row=%02u metal=%s", name, y, a);
         }
         /* and what a vertex stage is given */
         static uint32_t rec[256][2];
         id<MTLBuffer> ib;
         if (width == 16) {
            uint16_t i16[9];
            for (unsigned k = 0; k < T[i].n; k++)
               i16[k] = (uint16_t)idx[k];
            ib = buf(i16, T[i].n * 2);
         } else {
            ib = buf(idx, T[i].n * 4);
         }
         MTLPrimitiveType prim = T[i].prim;
         unsigned cnt = T[i].n;
         unsigned u = record(@"vrec", nil, ^(id<MTLRenderCommandEncoder> e) {
            [e drawIndexedPrimitives:prim indexCount:cnt indexType:width == 16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                         indexBuffer:ib indexBufferOffset:0];
         }, rec, 256, name);
         char line[256] = "";
         for (unsigned k = 0; k < u && k < 16; k++)
            sprintf(line + strlen(line), " %x", rec[k][0]);
         say("%s vertex-ids%s", name, line);
      }
   /* the test's own: index-restart-16 and -32, on a [[stage_in]] pipeline, against the model */
   static struct dm_picture want;
   struct pix_vertex strip[8];
   for (unsigned i = 0; i < 8; i++)
      strip[i] = pix_v(corner[i][0], corner[i][1], i < 4 ? 1 : 0, i < 4 ? 0 : 1, (i & 1) ? 1.0f : 0.0f, 1);
   dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned q = 0; q < 2; q++) {
      pix_model_tri(&want, &strip[q * 4], &strip[q * 4 + 1], &strip[q * 4 + 2], &whole, dm_whole(), 0, 0, true);
      pix_model_tri(&want, &strip[q * 4 + 2], &strip[q * 4 + 1], &strip[q * 4 + 3], &whole, dm_whole(), 0, 0, true);
   }
   static const uint16_t r16[9] = { 0, 1, 2, 3, 0xffff, 4, 5, 6, 7 };
   static const uint32_t r32[9] = { 0, 1, 2, 3, 0xffffffffu, 4, 5, 6, 7 };
   id<MTLRenderPipelineState> pv = pso(@"m5a_vattr", @"m5a_fcolor", vd_vattr(), MTLPixelFormatRGBA32Float, "index-restart");
   id<MTLBuffer> vb = buf(strip, sizeof(strip)), b16 = buf(r16, sizeof(r16)), b32 = buf(r32, sizeof(r32));
   for (unsigned w = 0; w < 2; w++) {
      run_pass(rpd(T16, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
         [e setRenderPipelineState:pv];
         [e setVertexBuffer:vb offset:0 atIndex:0];
         [e drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:9
                        indexType:w ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:w ? b32 : b16 indexBufferOffset:0];
      });
      judge16(w ? "index-restart-32" : "index-restart-16", &want, 1.0f / 512.0f);
   }
}

/* ---- 4: vertex_id and instance_id ------------------------------------------------------------------- */

static void
print_pairs(const char *name, const char *what, uint32_t (*metal)[2], unsigned nm, const uint32_t (*model)[2], unsigned nw)
{
   char a[512] = "", b[512] = "";
   for (unsigned i = 0; i < nm && i < 24; i++)
      sprintf(a + strlen(a), " %u,%u", metal[i][0], metal[i][1]);
   for (unsigned i = 0; i < nw && i < 24; i++)
      sprintf(b + strlen(b), " %u,%u", model[i][0], model[i][1]);
   bool same = nm == nw && !memcmp(metal, model, nw * 8);
   say("%s %s metal%s model%s %s", name, what, a, b, same ? "agree" : "DIFFER");
}

static void
case_ids(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static const struct { const char *name; bool indexed; int first_or_base; unsigned instances, base_instance; } cases[] = {
      { "ids-indexed-basevertex", true, 10, 1, 0 },
      { "ids-indexed-instances", true, 10, 3, 0 },
      { "ids-indexed-baseinstance", true, 10, 2, 3 },
      { "ids-nonindexed-start", false, 10, 1, 0 },
      { "ids-nonindexed-baseinstance", false, 10, 3, 1 },
   };
   static const uint16_t idx[3] = { 0, 1, 2 };
   id<MTLBuffer> ib = buf(idx, sizeof(idx));
   float posb[16][4];
   for (unsigned i = 0; i < 16; i++) {
      posb[i][0] = -0.875f + 0.0625f * (float)(i % 3 == 1 ? 8 : 0);
      posb[i][1] = 0.875f - 0.0625f * (float)(i % 3 == 2 ? 8 : 0);
      posb[i][2] = 0.0f;
      posb[i][3] = 1.0f;
   }
   for (unsigned i = 0; i < 10; i++)
      posb[i][1] -= 1.0f;
   id<MTLBuffer> pb = buf(posb, sizeof(posb));
   id<MTLRenderPipelineState> p = pso(@"m5a_vids", @"m5a_fcolor", nil, MTLPixelFormatRGBA32Float, "ids");
   for (unsigned c = 0; c < COUNT(cases); c++) {
      static uint32_t rec[64][2];
      uint32_t model[64][2];
      unsigned nw = 0;
      for (unsigned i = 0; i < cases[c].instances; i++)
         for (unsigned k = 0; k < 3; k++) {
            model[nw][0] = (uint32_t)cases[c].first_or_base + k;
            model[nw][1] = cases[c].base_instance + i;
            nw++;
         }
      qsort(model, nw, 8, pair_cmp);
      bool indexed = cases[c].indexed;
      int first = cases[c].first_or_base;
      unsigned inst = cases[c].instances, base = cases[c].base_instance;
      void (^issue)(id<MTLRenderCommandEncoder>) = ^(id<MTLRenderCommandEncoder> e) {
         if (indexed)
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:ib
                   indexBufferOffset:0 instanceCount:inst baseVertex:first baseInstance:base];
         else
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(NSUInteger)first vertexCount:3 instanceCount:inst baseInstance:base];
      };
      unsigned nm = record(@"vrec", nil, issue, rec, 64, cases[c].name);
      print_pairs(cases[c].name, "vid,iid", rec, nm, (const uint32_t (*)[2])model, nw);
      /* the picture of the test */
      if (!p)
         continue;
      run_pass(rpd(T16, 0, 0, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
         [e setRenderPipelineState:p];
         [e setVertexBuffer:pb offset:0 atIndex:0];
         issue(e);
      });
      dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
      for (unsigned i = 0; i < inst; i++) {
         unsigned iid = base + i;
         struct pix_vertex tri3[3];
         for (unsigned k = 0; k < 3; k++) {
            unsigned vid = (unsigned)first + k;
            tri3[k].pos[0] = posb[vid][0] + (float)iid * 0.25f;
            tri3[k].pos[1] = posb[vid][1];
            tri3[k].pos[2] = 0.0f;
            tri3[k].pos[3] = 1.0f;
            tri3[k].col[0] = (float)vid / 64.0f;
            tri3[k].col[1] = (float)iid / 16.0f;
            tri3[k].col[2] = 0.0f;
            tri3[k].col[3] = 1.0f;
         }
         pix_model_tri(&want, &tri3[0], &tri3[1], &tri3[2], &whole, dm_whole(), 0, 0, false);
      }
      judge16(cases[c].name, &want, 1.0f / 512.0f);
   }
   /* index-basevertex-neg: indices 7, 8, 9, base -7, and a big one: the id is the sum */
   {
      static uint32_t rec[16][2];
      static const uint16_t i2[3] = { 7, 8, 9 };
      id<MTLBuffer> ib2 = buf(i2, sizeof(i2));
      static const uint32_t model[3][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
      unsigned nm = record(@"vrec", nil, ^(id<MTLRenderCommandEncoder> e) {
         [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:ib2
                indexBufferOffset:0 instanceCount:1 baseVertex:-7 baseInstance:0];
      }, rec, 16, "ids-basevertex-negative");
      print_pairs("ids-basevertex-negative", "vid,iid", rec, nm, model, 3);
   }
}

/* The instance step of a [[stage_in]] attribute with a base instance: which element is fetched
 * (air_metal_test.c's attrib-rate2-base: `base + i / rate`). */
static void
step_case(const char *name, MTLVertexStepFunction fnk, unsigned rate, unsigned stride, unsigned instances, unsigned base)
{
   static uint32_t rec[64][2];
   uint32_t identity[32], model[64][2];
   for (unsigned i = 0; i < 32; i++)
      identity[i] = i;
   id<MTLBuffer> ib = buf(identity, sizeof(identity));
   MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
   vd.attributes[0].format = MTLVertexFormatUInt;
   vd.attributes[0].offset = 0;
   vd.attributes[0].bufferIndex = 3;
   vd.layouts[3].stride = stride;
   vd.layouts[3].stepFunction = fnk;
   vd.layouts[3].stepRate = rate;
   unsigned nw = 0;
   for (unsigned i = 0; i < instances; i++) {
      model[nw][0] = base + i;
      model[nw][1] = fnk == MTLVertexStepFunctionConstant ? 0 : base + i / (rate ? rate : 1);
      nw++;
   }
   qsort(model, nw, 8, pair_cmp);
   unsigned nm = record(@"vstep", vd, ^(id<MTLRenderCommandEncoder> e) {
      [e setVertexBuffer:ib offset:0 atIndex:3];
      [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1 instanceCount:instances baseInstance:base];
   }, rec, 64, name);
   print_pairs(name, "iid,element", rec, nm, (const uint32_t (*)[2])model, nw);
}

static void
case_steps(int arg)
{
   (void)arg;
   step_case("attrib-instance", MTLVertexStepFunctionPerInstance, 1, 4, 4, 0);
   step_case("attrib-rate2", MTLVertexStepFunctionPerInstance, 2, 4, 4, 0);
   step_case("attrib-baseinstance", MTLVertexStepFunctionPerInstance, 1, 4, 3, 2);
   step_case("attrib-rate2-base", MTLVertexStepFunctionPerInstance, 2, 4, 4, 1);
   step_case("attrib-rate3-base5", MTLVertexStepFunctionPerInstance, 3, 4, 7, 5);
   step_case("attrib-constant-rate0", MTLVertexStepFunctionConstant, 0, 4, 4, 0);
}

/* The test asks for a constant step at rate 1 (its row attrib-constant): whether Metal itself takes that. */
static void
case_step_constant1(int arg)
{
   (void)arg;
   step_case("attrib-constant-rate1", MTLVertexStepFunctionConstant, 1, 4, 4, 0);
}

/* ---- 5: views ---------------------------------------------------------------------------------------- */

static void
want_texels(struct dm_picture *want, unsigned level, unsigned slice, unsigned w, unsigned h)
{
   dm_clear(want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++)
         tm_texelf(level, slice, x * w / DM_W, y * h / DM_H, want->px[y][x]);
}

#define TX_EXACT (1.0f / 2048.0f)
#define TX_FILTERED (1.0f / 255.0f + 1.0f / 2048.0f)
#define FMT_RGBA8 MTLPixelFormatRGBA8Unorm

static id<MTLTexture>
parent5(MTLTextureUsage use)
{
   static uint8_t mem[16 * 16 * 4];
   id<MTLTexture> p = mk2d(FMT_RGBA8, 16, 16, 5, use);
   for (unsigned l = 0; l < 5; l++) {
      tm_fill(mem, l, 0, 16 >> l, 16 >> l);
      put(p, l, 0, 16 >> l, 16 >> l, 1, 4, mem);
   }
   return p;
}

static const struct tm_format *
tm_find(unsigned mtl)
{
   for (unsigned i = 0; i < COUNT(tm_formats); i++)
      if (tm_formats[i].mtl == mtl)
         return &tm_formats[i];
   return NULL;
}

/* A view of RGBA8 as another format of the size: what the model's own table reads from those bytes. */
static const unsigned view_formats[] = { 80, 71, 72, 73, 74, 53, 54, 55, 60, 62, 63, 65, 90, 91, 92, 94, 93 };

static void
case_view_format(int arg)
{
   const struct tm_format *f = tm_find(view_formats[arg]);
   if (!f) {
      say("view-format %u is not in tm_formats", view_formats[arg]);
      return;
   }
   static struct dm_picture want;
   char name[64];
   snprintf(name, sizeof(name), "view-format-%s", f->name);
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV);
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:(MTLPixelFormat)f->mtl];
   if (!v) {
      say("%s refused", name);
      return;
   }
   dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         uint8_t b[4];
         tm_texel(0, 0, x, y, b);
         tm_format_read(f, b, want.px[y][x]);
      }
   struct pix_u_t u = tx_u(1, 1, 0, 0, 0);
   NSString *fs = f->kind == TM_UINT ? @"m5b_freadu" : f->kind == TM_SINT ? @"m5b_freadi" : @"m5b_fread";
   if (probe(fs, v, nil, &u, name))
      judge16(name, &want, tm_format_slop(f));
}

static void
case_views(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static uint8_t mem[16 * 16 * 4 * 8];
   id<MTLSamplerState> near = nearest();
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV);
   const struct pix_u_t top = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);

   /* the test's two with a sampler */
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
   if (v && probe(@"m5b_f2d", v, near, &top, "view-format-bgra8-sample")) {
      want_texels(&want, 0, 0, 16, 16);
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            float t = want.px[y][x][0];
            want.px[y][x][0] = want.px[y][x][2];
            want.px[y][x][2] = t;
         }
      judge16("view-format-bgra8-sample", &want, TX_EXACT);
   }
   v = [p newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
   if (v && probe(@"m5b_f2d", v, near, &top, "view-format-srgb-sample")) {
      want_texels(&want, 0, 0, 16, 16);
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++)
            for (unsigned c = 0; c < 3; c++)
               want.px[y][x][c] = dm_srgb_decode(want.px[y][x][c]);
      judge16("view-format-srgb-sample", &want, 1.0f / 1024.0f);
   }
   /* one level: its level 0 is the parent's level 2 */
   v = [p newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(2, 1) slices:NSMakeRange(0, 1)];
   if (v && probe(@"m5b_f2d", v, near, &top, "view-level")) {
      want_texels(&want, 2, 0, 4, 4);
      judge16("view-level", &want, TX_EXACT);
      say("view-level size %ux%u levels=%u", (unsigned)v.width, (unsigned)v.height, (unsigned)v.mipmapLevelCount);
   }
   /* two levels, at its own level 1 */
   v = [p newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(1, 2) slices:NSMakeRange(0, 1)];
   const struct pix_u_t one = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 1.0f);
   if (v && probe(@"m5b_f2d", v, near, &one, "view-levels")) {
      want_texels(&want, 2, 0, 4, 4);
      judge16("view-levels", &want, TX_EXACT);
   }
   /* the parent goes first, and the view still reads */
   v = [p newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)];
   p = nil;
   if (v && probe(@"m5b_f2d", v, near, &top, "view-outlives-parent")) {
      want_texels(&want, 1, 0, 8, 8);
      judge16("view-outlives-parent", &want, TX_EXACT);
   }
   /* slices */
   id<MTLTexture> a = mk(MTLTextureType2DArray, FMT_RGBA8, 16, 16, 1, 8, 1, 1, USE_RD, SHARED);
   for (unsigned s = 0; s < 8; s++) {
      tm_fill(mem, 0, s, 16, 16);
      put(a, 0, s, 16, 16, 1, 4, mem);
   }
   v = [a newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(3, 1)];
   if (v && probe(@"m5b_f2d", v, near, &top, "view-slice")) {
      want_texels(&want, 0, 3, 16, 16);
      judge16("view-slice", &want, TX_EXACT);
   }
   {
      id<MTLTexture> b = mk(MTLTextureType2DArray, FMT_RGBA8, 16, 16, 1, 4, 3, 1, USE_RD, SHARED);
      for (unsigned l = 0; l < 3; l++)
         for (unsigned s = 0; s < 4; s++) {
            tm_fill(mem, l, s, 16 >> l, 16 >> l);
            put(b, l, s, 16 >> l, 16 >> l, 1, 4, mem);
         }
      v = [b newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(1, 1) slices:NSMakeRange(2, 1)];
      if (v && probe(@"m5b_f2d", v, near, &top, "view-level1-slice2")) {
         want_texels(&want, 1, 2, 8, 8);
         judge16("view-level1-slice2", &want, TX_EXACT);
      }
   }
   v = [a newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(2, 3)];
   struct pix_u_t ui = top;
   ui.index = 2;
   if (v && probe(@"m5b_f2darr", v, near, &ui, "view-slices")) {
      want_texels(&want, 0, 4, 16, 16);
      judge16("view-slices", &want, TX_EXACT);
   }
   v = [a newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureTypeCube levels:NSMakeRange(0, 1) slices:NSMakeRange(2, 6)];
   struct pix_u_t uc = { .lod = 0 };
   tm_cube_axes(4, uc.u0, uc.u1, uc.u2);
   if (v && probe(@"m5b_fcube", v, near, &uc, "view-cube")) {
      want_texels(&want, 0, 6, 16, 16);
      judge16("view-cube", &want, TX_EXACT);
   }
}

/* The swizzle's words, MTLTextureSwizzle numbers in air_metal_test.c's tables. */
static const struct { const char *name; unsigned s[4]; } sw_cases[] = {
   { "abgr", { 5, 4, 3, 2 } },
   { "gbra", { 3, 4, 2, 5 } },
   { "01rg", { 0, 1, 2, 3 } },
   { "a01b", { 5, 0, 1, 4 } },
};

static MTLTextureSwizzleChannels
sw_of(const unsigned s[4])
{
   return MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)s[0], (MTLTextureSwizzle)s[1], (MTLTextureSwizzle)s[2],
                                        (MTLTextureSwizzle)s[3]);
}

static id<MTLTexture>
sw_view(id<MTLTexture> p, const unsigned s[4])
{
   return [p newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                    slices:NSMakeRange(0, 1) swizzle:sw_of(s)];
}

static void
want_swizzled(struct dm_picture *want, const unsigned s[4])
{
   dm_clear(want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         float t[4];
         tm_texelf(0, 0, x, y, t);
         for (unsigned c = 0; c < 4; c++)
            want->px[y][x][c] = s[c] == 0 ? 0.0f : s[c] == 1 ? 1.0f : t[s[c] - 2];
      }
}

static void
case_swizzle(int arg)
{
   (void)arg;
   static struct dm_picture want;
   id<MTLSamplerState> near = nearest();
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV);
   const struct pix_u_t top = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f), un = tx_u(1, 1, 0, 0, 0);
   for (unsigned i = 0; i < COUNT(sw_cases); i++) {
      char name[64];
      id<MTLTexture> v = sw_view(p, sw_cases[i].s);
      if (!v) {
         say("view-swizzle-%s refused", sw_cases[i].name);
         continue;
      }
      say("view-swizzle-%s usage: parent 0x%x view 0x%x", sw_cases[i].name, (unsigned)p.usage, (unsigned)v.usage);
      want_swizzled(&want, sw_cases[i].s);
      snprintf(name, sizeof(name), "view-swizzle-%s-sample", sw_cases[i].name);
      if (probe(@"m5b_f2d", v, near, &top, name))
         judge16(name, &want, TX_EXACT);
      snprintf(name, sizeof(name), "view-swizzle-%s-read-fragment", sw_cases[i].name);
      if (probe(@"m5b_fread", v, nil, &un, name))
         judge16(name, &want, TX_EXACT);
      /* read() from a compute kernel */
      snprintf(name, sizeof(name), "view-swizzle-%s-read-compute", sw_cases[i].name);
      id<MTLBuffer> o = buf(NULL, 16 * 16 * 16);
      run_compute(@"krd", 16, 16, ^(id<MTLComputeCommandEncoder> e) {
         [e setTexture:v atIndex:0];
         [e setBuffer:o offset:0 atIndex:0];
      });
      judge(name, DM_RGBA32F, o.contents, &want, TX_EXACT);
      /* getBytes through it: bytes, or swizzled bytes? */
      uint8_t raw[4], plain[4];
      [v getBytes:raw bytesPerRow:64 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
      tm_texel(0, 0, 5, 3, plain);
      say("view-swizzle-%s getbytes (5,3) %02x %02x %02x %02x parent's own %02x %02x %02x %02x: %s", sw_cases[i].name, raw[0],
          raw[1], raw[2], raw[3], plain[0], plain[1], plain[2], plain[3], memcmp(raw, plain, 4) ? "swizzled" : "not swizzled");
   }
   /* A swizzle of a swizzle: does the second see through the first? */
   {
      id<MTLTexture> v1 = sw_view(p, sw_cases[0].s);   /* abgr */
      id<MTLTexture> v2 = sw_view(v1, sw_cases[1].s);  /* gbra of that */
      if (v2) {
         /* composed: the result channel c is parent's channel from v1's mapping of v2's word */
         unsigned composed[4], last[4];
         for (unsigned c = 0; c < 4; c++) {
            unsigned w = sw_cases[1].s[c];
            composed[c] = w < 2 ? w : sw_cases[0].s[w - 2];
            last[c] = w;
         }
         if (probe(@"m5b_f2d", v2, near, &top, "view-swizzle-of-swizzle")) {
            static float got[16][16][4];
            rd(T16, 0, 0, 0, 16, 16, 16, got);
            want_swizzled(&want, composed);
            judge("view-swizzle-of-swizzle-as-composed", DM_RGBA32F, got, &want, TX_EXACT);
            want_swizzled(&want, last);
            judge("view-swizzle-of-swizzle-as-second-alone", DM_RGBA32F, got, &want, TX_EXACT);
         }
      } else {
         say("view-swizzle-of-swizzle refused");
      }
      /* a view of the swizzled view that names no swizzle */
      id<MTLTexture> v3 = [v1 newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D
                                                     levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
      if (v3 && probe(@"m5b_f2d", v3, near, &top, "view-of-swizzled-no-swizzle")) {
         static float got[16][16][4];
         static const unsigned identity[4] = { 2, 3, 4, 5 };
         rd(T16, 0, 0, 0, 16, 16, 16, got);
         want_swizzled(&want, sw_cases[0].s);
         judge("view-of-swizzled-no-swizzle-keeps-it", DM_RGBA32F, got, &want, TX_EXACT);
         want_swizzled(&want, identity);
         judge("view-of-swizzled-no-swizzle-drops-it", DM_RGBA32F, got, &want, TX_EXACT);
      }
   }
   /* a swizzle over a format view: the format's order first, the swizzle on what that reads */
   {
      id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm textureType:MTLTextureType2D
                                                   levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw_of(sw_cases[1].s)];
      if (v && probe(@"m5b_f2d", v, near, &top, "view-swizzle-gbra-of-bgra8")) {
         dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               float t[4], b[4];
               tm_texelf(0, 0, x, y, t);
               b[0] = t[2];
               b[1] = t[1];
               b[2] = t[0];
               b[3] = t[3];
               for (unsigned c = 0; c < 4; c++)
                  want.px[y][x][c] = b[sw_cases[1].s[c] - 2];
            }
         judge16("view-swizzle-gbra-of-bgra8", &want, TX_EXACT);
      }
   }
}

/* A swizzle over the formats that have fewer channels: the source of a channel the format has not got. */
static void
case_swizzle_small(int arg)
{
   (void)arg;
   static uint8_t mem[16 * 16];
   memset(mem, 100, sizeof(mem));
   const struct pix_u_t top = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
   id<MTLSamplerState> near = nearest();
   static const struct { const char *n; MTLPixelFormat f; float m[4]; } fm[] = {
      { "R8", MTLPixelFormatR8Unorm, { 100.0f / 255.0f, 0, 0, 1 } },
      { "A8", MTLPixelFormatA8Unorm, { 0, 0, 0, 100.0f / 255.0f } },
   };
   static const struct { const char *n; unsigned s[4]; } sw[] = {
      { "abgr", { 5, 4, 3, 2 } }, { "rrrr", { 2, 2, 2, 2 } }, { "aaaa", { 5, 5, 5, 5 } }, { "01ar", { 0, 1, 5, 2 } },
   };
   for (unsigned f = 0; f < COUNT(fm); f++) {
      id<MTLTexture> t = mk2d(fm[f].f, 16, 16, 1, USE_RD | USE_FV);
      put(t, 0, 0, 16, 16, 1, 1, mem);
      for (unsigned i = 0; i < COUNT(sw); i++) {
         char name[64];
         snprintf(name, sizeof(name), "swizzle-%s-%s", fm[f].n, sw[i].n);
         id<MTLTexture> v = [t newTextureViewWithPixelFormat:fm[f].f textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                      slices:NSMakeRange(0, 1) swizzle:sw_of(sw[i].s)];
         if (!v || !probe(@"m5b_f2d", v, near, &top, name))
            continue;
         float px[4], want[4];
         rd(T16, 0, 0, 0, 1, 1, 16, px);
         bool same = true;
         for (unsigned c = 0; c < 4; c++) {
            want[c] = sw[i].s[c] == 0 ? 0.0f : sw[i].s[c] == 1 ? 1.0f : fm[f].m[sw[i].s[c] - 2];
            same = same && fabsf(px[c] - want[c]) < 1e-3f;
         }
         say("%s metal %.3f %.3f %.3f %.3f model %.3f %.3f %.3f %.3f %s", name, (double)px[0], (double)px[1], (double)px[2], (double)px[3],
             (double)want[0], (double)want[1], (double)want[2], (double)want[3], same ? "agree" : "DIFFER");
      }
   }
}

/* The next few one at a time: each may be refused by an assertion. */

static void
case_swizzle_depth(int arg)
{
   (void)arg;
   id<MTLTexture> z = mk(MTLTextureType2D, MTLPixelFormatDepth32Float, 16, 16, 1, 1, 1, 1, USE_RD | USE_RT, PRIVATE);
   MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
   pd.depthAttachment.texture = z;
   pd.depthAttachment.loadAction = MTLLoadActionClear;
   pd.depthAttachment.clearDepth = 0.25;
   pd.depthAttachment.storeAction = MTLStoreActionStore;
   run_pass(pd, nil);
   id<MTLTexture> v = [z newTextureViewWithPixelFormat:MTLPixelFormatDepth32Float textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                slices:NSMakeRange(0, 1) swizzle:sw_of(sw_cases[0].s)];
   say("swizzle-depth a swizzled view of Depth32Float: %s", v ? "made" : "refused");
   if (!v)
      return;
   const struct pix_u_t u = tx_u(1, 1, 0, 0, 0);
   if (probe(@"m5b_fread", v, nil, &u, "swizzle-depth-as-2d")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("swizzle-depth abgr, texture2d read of clear 0.25 -> %.3f %.3f %.3f %.3f", (double)px[0], (double)px[1], (double)px[2], (double)px[3]);
   }
   if (probe(@"fdread", v, nil, NULL, "swizzle-depth-as-depth2d")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("swizzle-depth abgr, depth2d read of clear 0.25 -> %.3f", (double)px[0]);
   }
}

static void
case_swizzle_descriptor(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static uint8_t mem[16 * 16 * 4];
   MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:FMT_RGBA8 width:16 height:16 mipmapped:NO];
   td.usage = USE_RD;
   td.swizzle = sw_of(sw_cases[0].s);
   id<MTLTexture> t = [D newTextureWithDescriptor:td];
   tm_fill(mem, 0, 0, 16, 16);
   put(t, 0, 0, 16, 16, 1, 4, mem);
   const struct pix_u_t top = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
   if (probe(@"m5b_f2d", t, nearest(), &top, "swizzle-descriptor")) {
      want_swizzled(&want, sw_cases[0].s);
      judge16("swizzle-descriptor", &want, TX_EXACT);
   }
}

static void
case_swizzle_render(int arg)
{
   (void)arg;
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV);
   for (unsigned i = 0; i < 2; i++) {
      id<MTLTexture> v = sw_view(p, sw_cases[i].s);
      if (!v) {
         say("swizzle-render view refused");
         return;
      }
      if (!probe_into(@"m5a_fsrc", v, 0, 0, 0, nil, nil, NULL, clear0, "swizzle-render"))
         return;
      uint8_t got[4];
      float s[4];
      dm_src(5, 3, s);
      [p getBytes:got bytesPerRow:64 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
      say("swizzle-render through %s view: parent (5,3) bytes %d %d %d %d; shader wrote %d %d %d %d", sw_cases[i].name,
          got[0], got[1], got[2], got[3], dm_unorm8(s[0]), dm_unorm8(s[1]), dm_unorm8(s[2]), dm_unorm8(s[3]));
   }
}

static void
case_swizzle_write(int arg)
{
   (void)arg;
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV | USE_WR);
   for (unsigned i = 0; i < 2; i++) {
      id<MTLTexture> v = sw_view(p, sw_cases[i].s);
      if (!v) {
         say("swizzle-write view refused");
         return;
      }
      run_compute(@"kwr", 16, 16, ^(id<MTLComputeCommandEncoder> e) {
         [e setTexture:v atIndex:0];
      });
      uint8_t got[4];
      [p getBytes:got bytesPerRow:64 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
      say("swizzle-write through %s view: parent (5,3) bytes %d %d %d %d; shader wrote 64 128 191 255", sw_cases[i].name, got[0], got[1], got[2], got[3]);
   }
}

static void
case_view_write(int arg)
{
   (void)arg;
   static uint8_t mem[4 * 4 * 4], got[4 * 4 * 4];
   id<MTLTexture> p = parent5(USE_RD | USE_RT | USE_FV);
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(2, 1) slices:NSMakeRange(0, 1)];
   tm_fill(mem, 7, 7, 4, 4);
   put(v, 0, 0, 4, 4, 1, 4, mem);
   rd(p, 2, 0, 0, 4, 4, 4, got);
   tbytes("view-write-parent-read", got, mem, 16, 4, 4);
}

static void
case_view_nofv(int arg)
{
   (void)arg;
   id<MTLTexture> p = parent5(USE_RD);   /* no PixelFormatView */
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
   say("view-format-without-usage bgra8 of rgba8 usage=read-only: %s", v ? "made" : "refused");
}

static void
case_view_nofv_srgb(int arg)
{
   (void)arg;
   id<MTLTexture> p = parent5(USE_RD);
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
   say("view-format-without-usage srgb of rgba8 usage=read-only: %s", v ? "made" : "refused");
}

static void
case_view_3d_of_array(int arg)
{
   (void)arg;
   id<MTLTexture> a = mk(MTLTextureType2DArray, FMT_RGBA8, 16, 16, 1, 8, 1, 1, USE_RD, SHARED);
   id<MTLTexture> v = [a newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType3D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
   say("view-3d-of-array: %s", v ? "made" : "refused");
}

static void
case_view_2d_of_3d(int arg)
{
   (void)arg;
   id<MTLTexture> t = mk(MTLTextureType3D, FMT_RGBA8, 16, 16, 4, 1, 1, 1, USE_RD | USE_RT, SHARED);
   id<MTLTexture> v = [t newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(1, 1)];
   say("view-2d-of-3d slice 1 (the test's clear-3d-plane1): %s", v ? "made" : "refused");
   if (v) {
      const float c[4] = { 1, 0, 0, 1 };
      run_pass(rpd(v, 0, 0, 0, c), nil);
      uint8_t got[4];
      rd(t, 0, 0, 1, 1, 1, 4, got);
      say("view-2d-of-3d cleared plane 1 reads %d %d %d %d", got[0], got[1], got[2], got[3]);
   }
}

static void
case_view_sizeclass(int arg)
{
   (void)arg;
   id<MTLTexture> p = parent5(USE_RD | USE_FV);
   id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatR16Unorm];
   say("view-another-size-class r16 of rgba8: %s", v ? "made" : "refused");
}

static void
case_view_x32s8(int arg)
{
   (void)arg;
   static struct dm_picture want;
   id<MTLTexture> z = mk(MTLTextureType2D, MTLPixelFormatDepth32Float_Stencil8, 16, 16, 1, 1, 1, 1, USE_RD | USE_RT | USE_FV, PRIVATE);
   MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
   pd.depthAttachment.texture = z;
   pd.depthAttachment.loadAction = MTLLoadActionClear;
   pd.depthAttachment.clearDepth = 0.5;
   pd.depthAttachment.storeAction = MTLStoreActionStore;
   pd.stencilAttachment.texture = z;
   pd.stencilAttachment.loadAction = MTLLoadActionClear;
   pd.stencilAttachment.clearStencil = 42;
   pd.stencilAttachment.storeAction = MTLStoreActionStore;
   run_pass(pd, nil);
   id<MTLTexture> v = [z newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8 textureType:MTLTextureType2D
                                                levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
   if (!v) {
      say("view-x32s8 refused");
      return;
   }
   const struct pix_u_t u = tx_u(1, 1, 0, 0, 0);
   if (probe(@"m5b_freadu", v, nil, &u, "view-x32s8")) {
      dm_clear(&want, (const float[4]){ 42, 0, 0, 1 });
      judge16("view-x32s8", &want, TX_EXACT);
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("view-x32s8 reads %.0f %.0f %.0f %.0f", (double)px[0], (double)px[1], (double)px[2], (double)px[3]);
   }
}

/* ---- 6: a pass into a level, slice, face, plane ------------------------------------------------------- */

static void
tx_fsrc_bytes(uint8_t *to, unsigned w, unsigned h)
{
   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
         float s[4];
         dm_src(x, y, s);
         for (unsigned c = 0; c < 4; c++)
            to[((size_t)y * w + x) * 4 + c] = (uint8_t)dm_unorm8(s[c]);
      }
}

static void
check_level(const char *name, id<MTLTexture> t, unsigned level, unsigned slice, unsigned z, unsigned w, unsigned h, const void *want)
{
   static uint8_t got[64 * 64 * 4];
   rd(t, level, slice, z, w, h, 4, got);
   tbytes(name, got, want, w * h, 4, w);
}

static void
case_render_into(int arg)
{
   (void)arg;
   static uint8_t mem[32 * 32 * 4];
   id<MTLTexture> t = mk2d(FMT_RGBA8, 32, 32, 3, USE_RD | USE_RT);
   for (unsigned l = 0; l < 3; l++) {
      tm_fill(mem, l, 0, 32 >> l, 32 >> l);
      put(t, l, 0, 32 >> l, 32 >> l, 1, 4, mem);
   }
   if (probe_into(@"m5a_fsrc", t, 1, 0, 0, nil, nil, NULL, NULL, "render-level1")) {
      tx_fsrc_bytes(mem, 16, 16);
      check_level("render-level1", t, 1, 0, 0, 16, 16, mem);
      tm_fill(mem, 0, 0, 32, 32);
      check_level("render-level1-neighbour0", t, 0, 0, 0, 32, 32, mem);
      tm_fill(mem, 2, 0, 8, 8);
      check_level("render-level1-neighbour2", t, 2, 0, 0, 8, 8, mem);
   }
   /* a clear of one level, through a view of it */
   id<MTLTexture> v = [t newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(2, 1) slices:NSMakeRange(0, 1)];
   if (v) {
      const float c[4] = { 0.5f, 0.25f, 0.75f, 1.0f };
      run_pass(rpd(v, 0, 0, 0, c), nil);
      for (unsigned i = 0; i < 64; i++) {
         mem[i * 4 + 0] = 128;
         mem[i * 4 + 1] = 64;
         mem[i * 4 + 2] = 191;
         mem[i * 4 + 3] = 255;
      }
      check_level("clear-level2", t, 2, 0, 0, 8, 8, mem);
   }
   t = mk(MTLTextureType2DArray, FMT_RGBA8, 16, 16, 1, 4, 1, 1, USE_RD | USE_RT, SHARED);
   for (unsigned s = 0; s < 4; s++) {
      tm_fill(mem, 0, s, 16, 16);
      put(t, 0, s, 16, 16, 1, 4, mem);
   }
   if (probe_into(@"m5a_fsrc", t, 0, 2, 0, nil, nil, NULL, NULL, "render-slice2")) {
      tx_fsrc_bytes(mem, 16, 16);
      check_level("render-slice2", t, 0, 2, 0, 16, 16, mem);
      tm_fill(mem, 0, 1, 16, 16);
      check_level("render-slice2-neighbour1", t, 0, 1, 0, 16, 16, mem);
      tm_fill(mem, 0, 3, 16, 16);
      check_level("render-slice2-neighbour3", t, 0, 3, 0, 16, 16, mem);
   }
   t = mk(MTLTextureTypeCube, FMT_RGBA8, 16, 16, 1, 1, 1, 1, USE_RD | USE_RT, SHARED);
   for (unsigned s = 0; s < 6; s++) {
      tm_fill(mem, 0, s, 16, 16);
      put(t, 0, s, 16, 16, 1, 4, mem);
   }
   if (probe_into(@"m5a_fsrc", t, 0, 3, 0, nil, nil, NULL, NULL, "render-cube-face3")) {
      tx_fsrc_bytes(mem, 16, 16);
      check_level("render-cube-face3", t, 0, 3, 0, 16, 16, mem);
      tm_fill(mem, 0, 2, 16, 16);
      check_level("render-cube-face3-neighbour2", t, 0, 2, 0, 16, 16, mem);
      tm_fill(mem, 0, 4, 16, 16);
      check_level("render-cube-face3-neighbour4", t, 0, 4, 0, 16, 16, mem);
   }
   t = mk(MTLTextureType3D, FMT_RGBA8, 16, 16, 4, 1, 1, 1, USE_RD | USE_RT, SHARED);
   static uint8_t all[16 * 16 * 4 * 4];
   for (unsigned z = 0; z < 4; z++)
      tm_fill(all + (size_t)z * 16 * 16 * 4, 0, z, 16, 16);
   put(t, 0, 0, 16, 16, 4, 4, all);
   if (probe_into(@"m5a_fsrc", t, 0, 0, 2, nil, nil, NULL, NULL, "render-3d-plane2")) {
      tx_fsrc_bytes(mem, 16, 16);
      check_level("render-3d-plane2", t, 0, 0, 2, 16, 16, mem);
      check_level("render-3d-plane2-neighbour1", t, 0, 0, 1, 16, 16, all + (size_t)1 * 16 * 16 * 4);
      check_level("render-3d-plane2-neighbour3", t, 0, 0, 3, 16, 16, all + (size_t)3 * 16 * 16 * 4);
   }
}

/* From level 0 of an image, through a view, into its level 1: the test's render-from-own-level. */
static void
case_render_own_level(int arg)
{
   (void)arg;
   static uint8_t mem[32 * 32 * 4];
   id<MTLTexture> t = mk2d(FMT_RGBA8, 32, 32, 3, USE_RD | USE_RT);
   tm_fill(mem, 0, 0, 32, 32);
   put(t, 0, 0, 32, 32, 1, 4, mem);
   id<MTLTexture> v = [t newTextureViewWithPixelFormat:FMT_RGBA8 textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
   const struct pix_u_t u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
   if (probe_into(@"m5b_f2d", t, 1, 0, 0, v, nearest(), &u, NULL, "render-from-own-level")) {
      for (unsigned y = 0; y < 16; y++)
         for (unsigned x = 0; x < 16; x++)
            tm_texel(0, 0, x * 2, y * 2, mem + ((size_t)y * 16 + x) * 4);
      check_level("render-from-own-level", t, 1, 0, 0, 16, 16, mem);
   }
}

/* A depth attachment at level 1: the clear lands there and level 0 keeps its own. */
static void
case_render_depth_level(int arg)
{
   (void)arg;
   id<MTLTexture> z = mk(MTLTextureType2D, MTLPixelFormatDepth32Float, 32, 32, 1, 1, 2, 1, USE_RD | USE_RT, PRIVATE);
   for (unsigned l = 0; l < 2; l++) {
      MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
      pd.depthAttachment.texture = z;
      pd.depthAttachment.level = l;
      pd.depthAttachment.loadAction = MTLLoadActionClear;
      pd.depthAttachment.clearDepth = l ? 0.25 : 0.75;
      pd.depthAttachment.storeAction = MTLStoreActionStore;
      run_pass(pd, nil);
   }
   id<MTLBuffer> b = buf(NULL, 8);
   id<MTLCommandBuffer> c = [Q commandBuffer];
   id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
   for (unsigned l = 0; l < 2; l++)
      [e copyFromTexture:z sourceSlice:0 sourceLevel:l sourceOrigin:MTLOriginMake(3, 3, 0) sourceSize:MTLSizeMake(1, 1, 1)
                toBuffer:b destinationOffset:l * 4 destinationBytesPerRow:4 destinationBytesPerImage:4];
   [e endEncoding];
   [c commit];
   [c waitUntilCompleted];
   float *f = b.contents;
   say("render-depth-level1 %s level0=%.3f level1=%.3f model 0.750 0.250", f[0] == 0.75f && f[1] == 0.25f ? "agree" : "DIFFER",
       (double)f[0], (double)f[1]);
}

/* ---- 7: border colours and clamp_to_zero --------------------------------------------------------------- */

static void
want_address(struct dm_picture *want, unsigned as, unsigned at, float scale, float off, const float border[4])
{
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         float u = ((float)x + 0.5f) * scale + off, v = ((float)y + 0.5f) * scale + off;
         int i = tm_wrap(as, u, 16), j = tm_wrap(at, v, 16);
         want->care[y][x] = 1;
         if (i < 0 || j < 0)
            memcpy(want->px[y][x], border, 4 * sizeof(float));
         else
            tm_texelf(0, 0, (unsigned)i, (unsigned)j, want->px[y][x]);
      }
}

static id<MTLTexture>
sampler_tex(void)
{
   return parent5(USE_RD);
}

static const float bcol[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 } };
static const char *const bname[3] = { "transparent", "black", "white" };
#define ADDR_SCALE (3.0f / 16.0f)
#define ADDR_OFF (-1.0f)

static void
case_border_rgba8(int arg)
{
   (void)arg;
   static struct dm_picture want;
   id<MTLTexture> t = sampler_tex();
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   for (unsigned b = 0; b < 3; b++) {
      char name[64];
      snprintf(name, sizeof(name), "sampler-border-%s", bname[b]);
      id<MTLSamplerState> s = smp(TM_BORDER, TM_BORDER, TM_BORDER, 0, 0, 1, 0, 1, 0, 1000, b);
      if (probe(@"m5b_f2d", t, s, &u, name)) {
         want_address(&want, TM_BORDER, TM_BORDER, ADDR_SCALE, ADDR_OFF, bcol[b]);
         judge16(name, &want, TX_EXACT);
      }
      snprintf(name, sizeof(name), "sampler-own-border-%s", bname[b]);
      struct pix_u_t uo = u;
      uo.select = b;
      if (probe(@"m5b_fownb", t, nil, &uo, name)) {
         want_address(&want, TM_BORDER, TM_BORDER, ADDR_SCALE, ADDR_OFF, bcol[b]);
         judge16(name, &want, TX_EXACT);
      }
   }
}

/* clamp_to_zero with a border colour in the same sampler, no axis at border: which the zero is. */
static void
case_zero_with_border(int arg)
{
   (void)arg;
   static uint8_t r8[16 * 16];
   memset(r8, 100, sizeof(r8));
   id<MTLTexture> t = sampler_tex(), t8 = mk2d(MTLPixelFormatR8Unorm, 16, 16, 1, USE_RD);
   put(t8, 0, 0, 16, 16, 1, 1, r8);
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   for (unsigned b = 0; b < 3; b++) {
      char name[64];
      id<MTLSamplerState> sz = smp(TM_ZERO, TM_ZERO, TM_ZERO, 0, 0, 0, 0, 1, 0, 1000, b);
      snprintf(name, sizeof(name), "sampler-zero-with-%s-border-RGBA8", bname[b]);
      float px[4];
      if (probe(@"m5b_f2d", t, sz, &u, name)) {
         rd(T16, 0, 0, 0, 1, 1, 16, px);
         say("%s outside=%.3f %.3f %.3f %.3f", name, (double)px[0], (double)px[1], (double)px[2], (double)px[3]);
      }
      snprintf(name, sizeof(name), "sampler-zero-with-%s-border-R8", bname[b]);
      if (probe(@"m5b_f2d", t8, sz, &u, name)) {
         rd(T16, 0, 0, 0, 1, 1, 16, px);
         say("%s outside=%.3f %.3f %.3f %.3f", name, (double)px[0], (double)px[1], (double)px[2], (double)px[3]);
      }
   }
}

/* Two different modes on two axes, zero and border: the test's sampler-border-two. */
static void
case_border_mixed(int arg)
{
   (void)arg;
   id<MTLTexture> t = sampler_tex();
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   static const struct { const char *n; unsigned as, at, ar, b; } mx[] = {
      { "zero-s-border-t-white", TM_ZERO, TM_BORDER, TM_EDGE, 2 },
      { "border-s-zero-t-white", TM_BORDER, TM_ZERO, TM_EDGE, 2 },
      { "zero-s-border-t-transparent", TM_ZERO, TM_BORDER, TM_EDGE, 0 },
   };
   id<MTLSamplerState> s = smp(mx[arg].as, mx[arg].at, mx[arg].ar, 0, 0, 0, 0, 1, 0, 1000, mx[arg].b);
   say("sampler-border-two %s: %s", mx[arg].n, s ? "made" : "refused");
   if (s && probe(@"m5b_f2d", t, s, &u, mx[arg].n)) {
      float px[16][16][4];
      rd(T16, 0, 0, 0, 16, 16, 16, px);
      say("sampler-border-two %s s-outside (0,8)=%.3f %.3f %.3f %.3f t-outside (8,0)=%.3f %.3f %.3f %.3f", mx[arg].n, (double)px[8][0][0],
          (double)px[8][0][1], (double)px[8][0][2], (double)px[8][0][3], (double)px[0][8][0], (double)px[0][8][1],
          (double)px[0][8][2], (double)px[0][8][3]);
   }
}

/* A format and a border colour: the value outside. Each format in its own process: integer ones may be refused. */
static const struct { const char *name; MTLPixelFormat f; unsigned bytes; NSString *fs; } bfmt[] = {
   { "R8", MTLPixelFormatR8Unorm, 1, @"m5b_f2d" },
   { "A8", MTLPixelFormatA8Unorm, 1, @"m5b_f2d" },
   { "RG8", MTLPixelFormatRG8Unorm, 2, @"m5b_f2d" },
   { "B5G6R5", MTLPixelFormatB5G6R5Unorm, 2, @"m5b_f2d" },
   { "RGBA8Uint", MTLPixelFormatRGBA8Uint, 4, @"fsu" },
   { "R8Uint", MTLPixelFormatR8Uint, 1, @"fsu" },
   { "RGBA8Sint", MTLPixelFormatRGBA8Sint, 4, @"fsi" },
};

static void
case_border_fmt(int arg)
{
   static uint8_t mem[16 * 16 * 16];
   memset(mem, 100, sizeof(mem));
   id<MTLTexture> t = mk2d(bfmt[arg].f, 16, 16, 1, USE_RD);
   put(t, 0, 0, 16, 16, 1, bfmt[arg].bytes, mem);
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   for (unsigned b = 0; b < 3; b++) {
      char name[64];
      snprintf(name, sizeof(name), "sampler-border-%s-%s", bfmt[arg].name, bname[b]);
      id<MTLSamplerState> s = smp(TM_BORDER, TM_BORDER, TM_BORDER, 0, 0, 0, 0, 1, 0, 1000, b);
      if (probe(bfmt[arg].fs, t, s, &u, name)) {
         float px[16][16][4];
         rd(T16, 0, 0, 0, 16, 16, 16, px);
         say("%s outside=%.3f %.3f %.3f %.3f inside=%.3f %.3f %.3f %.3f border-as-given=%.0f %.0f %.0f %.0f", name, (double)px[0][0][0],
             (double)px[0][0][1], (double)px[0][0][2], (double)px[0][0][3], (double)px[8][8][0], (double)px[8][8][1],
             (double)px[8][8][2], (double)px[8][8][3], (double)bcol[b][0], (double)bcol[b][1], (double)bcol[b][2], (double)bcol[b][3]);
      }
   }
}

static void
case_zero(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static uint8_t r8[16 * 16];
   for (unsigned i = 0; i < 256; i++)
      r8[i] = (uint8_t)i;
   id<MTLSamplerState> s = smp(TM_ZERO, TM_ZERO, TM_ZERO, 0, 0, 0, 0, 1, 0, 1000, 0);
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   id<MTLTexture> t8 = mk2d(MTLPixelFormatR8Unorm, 16, 16, 1, USE_RD);
   put(t8, 0, 0, 16, 16, 1, 1, r8);
   if (probe(@"m5b_f2d", t8, s, &u, "sampler-zero-r8")) {
      dm_clear(&want, (const float[4]){ 0, 0, 0, 1 });
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            int i = tm_wrap(TM_ZERO, ((float)x + 0.5f) * ADDR_SCALE + ADDR_OFF, 16);
            int j = tm_wrap(TM_ZERO, ((float)y + 0.5f) * ADDR_SCALE + ADDR_OFF, 16);
            if (i >= 0 && j >= 0)
               want.px[y][x][0] = (float)(j * 16 + i) / 255.0f;
         }
      judge16("sampler-zero-r8", &want, TX_EXACT);
      float px[16][16][4];
      rd(T16, 0, 0, 0, 16, 16, 16, px);
      say("sampler-zero-r8 outside=%.3f %.3f %.3f %.3f model 0 0 0 1", (double)px[0][0][0], (double)px[0][0][1], (double)px[0][0][2], (double)px[0][0][3]);
   }
   static const struct { const char *n; MTLPixelFormat f; unsigned b; } o[] = {
      { "A8", MTLPixelFormatA8Unorm, 1 }, { "RG8", MTLPixelFormatRG8Unorm, 2 }, { "RGBA8", MTLPixelFormatRGBA8Unorm, 4 },
      { "B5G6R5", MTLPixelFormatB5G6R5Unorm, 2 }, { "RGBA16Float", MTLPixelFormatRGBA16Float, 8 }, { "RG11B10Float", MTLPixelFormatRG11B10Float, 4 },
   };
   for (unsigned i = 0; i < COUNT(o); i++) {
      static uint8_t mem[16 * 16 * 8];
      memset(mem, 100, sizeof(mem));
      id<MTLTexture> t = mk2d(o[i].f, 16, 16, 1, USE_RD);
      put(t, 0, 0, 16, 16, 1, o[i].b, mem);
      if (probe(@"m5b_f2d", t, s, &u, o[i].n)) {
         float px[16][16][4];
         rd(T16, 0, 0, 0, 16, 16, 16, px);
         say("sampler-zero-%s outside=%.3f %.3f %.3f %.3f", o[i].n, (double)px[0][0][0], (double)px[0][0][1], (double)px[0][0][2], (double)px[0][0][3]);
      }
   }
}

/* ---- 8: cube faces ------------------------------------------------------------------------------------ */

static void
case_cube(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static uint8_t mem[16 * 16 * 4];
   id<MTLSamplerState> near = nearest();
   id<MTLTexture> t = mk(MTLTextureTypeCube, FMT_RGBA8, 16, 16, 1, 1, 2, 1, USE_RD, SHARED);
   for (unsigned f = 0; f < 6; f++)
      for (unsigned l = 0; l < 2; l++) {
         tm_fill(mem, l, f, 16 >> l, 16 >> l);
         put(t, l, f, 16 >> l, 16 >> l, 1, 4, mem);
      }
   for (unsigned f = 0; f < 6; f++)
      for (unsigned l = 0; l < 2; l++) {
         char name[64];
         snprintf(name, sizeof(name), "tex-cube-F%u-L%u-sample", f, l);
         struct pix_u_t u = { .lod = (float)l };
         tm_cube_axes(f, u.u0, u.u1, u.u2);
         if (probe(@"m5b_fcube", t, near, &u, name)) {
            want_texels(&want, l, f, 16 >> l, 16 >> l);
            judge16(name, &want, TX_EXACT);
         }
      }
   /* directions, by a compute kernel: along each axis, off the axes, and the ties tm_cube_face decides */
   static const struct { const char *n; float d[3]; } dirs[] = {
      { "+X", { 1, 0, 0 } }, { "-X", { -1, 0, 0 } }, { "+Y", { 0, 1, 0 } }, { "-Y", { 0, -1, 0 } }, { "+Z", { 0, 0, 1 } }, { "-Z", { 0, 0, -1 } },
      { "off+X", { 1, 0.3f, -0.6f } }, { "off-X", { -1, -0.7f, 0.2f } }, { "off+Y", { 0.4f, 1, 0.55f } }, { "off-Y", { -0.8f, -1, 0.1f } },
      { "off+Z", { 0.45f, -0.35f, 1 } }, { "off-Z", { 0.65f, 0.15f, -1 } },
      { "tie+X+Y", { 1, 1, 0 } }, { "tie+X-Y", { 1, -1, 0 } }, { "tie+X+Z", { 1, 0, 1 } }, { "tie+X-Z", { 1, 0, -1 } },
      { "tie+Y+Z", { 0, 1, 1 } }, { "tie-Y+Z", { 0, -1, 1 } }, { "tie+X+Y+Z", { 1, 1, 1 } }, { "tie-X-Y-Z", { -1, -1, -1 } },
   };
   float ds[COUNT(dirs)][4];
   for (unsigned i = 0; i < COUNT(dirs); i++) {
      memcpy(ds[i], dirs[i].d, 12);
      ds[i][3] = 0;
   }
   id<MTLBuffer> db = buf(ds, sizeof(ds)), o = buf(NULL, COUNT(dirs) * 16);
   run_compute(@"kcube", COUNT(dirs), 1, ^(id<MTLComputeCommandEncoder> e) {
      [e setTexture:t atIndex:0];
      [e setSamplerState:near atIndex:0];
      [e setBuffer:db offset:0 atIndex:1];
      [e setBuffer:o offset:0 atIndex:0];
   });
   float (*r)[4] = o.contents;
   unsigned wrong = 0;
   for (unsigned i = 0; i < COUNT(dirs); i++) {
      float mu, mv;
      unsigned mf = tm_cube_face(dirs[i].d, &mu, &mv);
      /* red's high nibble is x, green's y and low nibble the slice (tm_texel) */
      int rb = (int)lrintf(r[i][0] * 255.0f), gb = (int)lrintf(r[i][1] * 255.0f);
      int gotf = gb & 15, gotx = rb >> 4, goty = gb >> 4;
      int mx = (int)floorf(mu * 16.0f), my = (int)floorf(mv * 16.0f);
      mx = mx > 15 ? 15 : mx;
      my = my > 15 ? 15 : my;
      bool same = gotf == (int)mf && gotx == mx && goty == my;
      wrong += !same;
      say("cube-dir %s face metal=%d model=%u texel metal=(%d,%d) model=(%d,%d) %s", dirs[i].n, gotf, mf, gotx, goty, mx, my, same ? "agree" : "DIFFER");
   }
   say("cube-dirs %s wrong=%u/%u", wrong ? "DIFFER" : "agree", wrong, (unsigned)COUNT(dirs));
   t = mk(MTLTextureTypeCubeArray, FMT_RGBA8, 16, 16, 1, 2, 1, 1, USE_RD, SHARED);
   for (unsigned s = 0; s < 12; s++) {
      tm_fill(mem, 0, s, 16, 16);
      put(t, 0, s, 16, 16, 1, 4, mem);
   }
   for (unsigned c = 0; c < 2; c++)
      for (unsigned f = 0; f < 6; f += 5) {
         char name[64];
         snprintf(name, sizeof(name), "tex-cubearray-C%u-F%u-sample", c, f);
         struct pix_u_t u = { .index = c };
         tm_cube_axes(f, u.u0, u.u1, u.u2);
         if (probe(@"m5b_fcubearr", t, near, &u, name)) {
            want_texels(&want, 0, c * 6 + f, 16, 16);
            judge16(name, &want, TX_EXACT);
         }
      }
}

/* ---- 9: address modes ----------------------------------------------------------------------------------- */

/* What the sweep below is tried against besides tm_wrap: the wrap done on the texel index, after the floor. */
static int
wrap_index(unsigned mode, float u, unsigned size)
{
   int n = (int)size, i = (int)floor(u * (float)size);
   switch (mode) {
   case TM_REPEAT:
      i = ((i % n) + n) % n;
      break;
   case TM_MIRROR: {
      int k = ((i % (2 * n)) + 2 * n) % (2 * n);
      i = k < n ? k : 2 * n - 1 - k;
      break;
   }
   case TM_MIRROR_EDGE:
      i = i < 0 ? -1 - i : i;
      break;
   case TM_ZERO:
   case TM_BORDER:
      if (i < 0 || i >= n)
         return -1;
      break;
   default:
      break;
   }
   return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

static void
case_addr(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static const char *const mode_name[6] = { "edge", "mirroredge", "repeat", "mirror", "zero", "border" };
   const float zero[4] = { 0, 0, 0, 0 };
   id<MTLTexture> t = sampler_tex();
   const struct pix_u_t u = tx_u(ADDR_SCALE, ADDR_SCALE, ADDR_OFF, ADDR_OFF, 0.0f);
   for (unsigned m = 0; m < 6; m++) {
      char name[64];
      snprintf(name, sizeof(name), "sampler-addr-%s", mode_name[m]);
      id<MTLSamplerState> s = smp(m, m, m, 0, 0, 1, 0, 1, 0, 1000, 0);
      if (probe(@"m5b_f2d", t, s, &u, name)) {
         want_address(&want, m, m, ADDR_SCALE, ADDR_OFF, zero);
         judge16(name, &want, TX_EXACT);
      }
   }
   id<MTLSamplerState> s = smp(TM_REPEAT, TM_MIRROR, TM_EDGE, 0, 0, 1, 0, 1, 0, 1000, 0);
   if (probe(@"m5b_f2d", t, s, &u, "sampler-addr-repeat-mirror")) {
      want_address(&want, TM_REPEAT, TM_MIRROR, ADDR_SCALE, ADDR_OFF, zero);
      judge16("sampler-addr-repeat-mirror", &want, TX_EXACT);
   }
   /* a small texture of distinct texels, at coordinates that include the exact edges */
   static const float us[] = { -2.125f, -1.0f, -0.875f, -0.25f, -0.125f, -0.0f, 0.0f, 0.125f, 0.25f, 0.5f, 0.875f, 0.9990234375f, 1.0f, 1.125f, 1.25f, 1.5f, 1.875f, 2.0f, 2.125f, 3.5f };
   uint8_t small[4 * 4];
   for (unsigned i = 0; i < 4; i++) {
      small[i * 4] = (uint8_t)((i + 1) * 50);
      small[i * 4 + 1] = small[i * 4 + 2] = 0;
      small[i * 4 + 3] = 255;
   }
   id<MTLTexture> st = mk2d(FMT_RGBA8, 4, 1, 1, USE_RD);
   put(st, 0, 0, 4, 1, 1, 4, small);
   id<MTLBuffer> ub = buf(us, sizeof(us)), o = buf(NULL, COUNT(us) * 16);
   char ul[256] = "";
   for (unsigned i = 0; i < COUNT(us); i++)
      sprintf(ul + strlen(ul), " %g", (double)us[i]);
   say("addr4 coordinates%s", ul);
   for (unsigned m = 0; m < 6; m++) {
      id<MTLSamplerState> ss = smp(m, m, m, 0, 0, 0, 0, 1, 0, 1000, 0);
      run_compute(@"ksmp", COUNT(us), 1, ^(id<MTLComputeCommandEncoder> e) {
         [e setTexture:st atIndex:0];
         [e setSamplerState:ss atIndex:0];
         [e setBuffer:ub offset:0 atIndex:1];
         [e setBuffer:o offset:0 atIndex:0];
      });
      float (*r)[4] = o.contents;
      char a[256] = "", b[256] = "";
      unsigned wrong = 0;
      for (unsigned i = 0; i < COUNT(us); i++) {
         int got = (int)lrintf(r[i][0] * 255.0f / 50.0f) - 1;   /* -1: the border */
         int mo = tm_wrap(m, us[i], 4);
         sprintf(a + strlen(a), " %d", got);
         sprintf(b + strlen(b), " %d", mo);
         wrong += got != mo;
      }
      say("addr4-%s metal%s model%s %s wrong=%u", mode_name[m], a, b, wrong ? "DIFFER" : "agree", wrong);
   }
   /* every multiple of 1/16 from -3 to 3, half of them on a texel's edge of the four, and off the edges */
   for (unsigned off = 0; off < 2; off++) {
      float sweep[97];
      for (unsigned i = 0; i < 97; i++)
         sweep[i] = ((float)i - 48.0f) / 16.0f + (off ? 1.0f / 64.0f : 0.0f);
      id<MTLBuffer> sb = buf(sweep, sizeof(sweep)), so = buf(NULL, 97 * 16);
      for (unsigned m = 0; m < 6; m++) {
         id<MTLSamplerState> ss = smp(m, m, m, 0, 0, 0, 0, 1, 0, 1000, 0);
         run_compute(@"ksmp", 97, 1, ^(id<MTLComputeCommandEncoder> e) {
            [e setTexture:st atIndex:0];
            [e setSamplerState:ss atIndex:0];
            [e setBuffer:sb offset:0 atIndex:1];
            [e setBuffer:so offset:0 atIndex:0];
         });
         float (*r)[4] = so.contents;
         unsigned wm = 0, wp = 0;
         for (unsigned i = 0; i < 97; i++) {
            int got = (int)lrintf(r[i][0] * 255.0f / 50.0f) - 1;
            wm += got != tm_wrap(m, sweep[i], 4);
            wp += got != wrap_index(m, sweep[i], 4);
         }
         say("addr-sweep-%s %s of 97: tm_wrap %s wrong=%u; the wrap on the index (this tool's wrap_index) %s wrong=%u", mode_name[m],
             off ? "off the edges" : "on multiples of 1/16", wm ? "DIFFER" : "agree", wm, wp ? "DIFFER" : "agree", wp);
      }
   }
}

/* ---- 10: generateMipmaps ---------------------------------------------------------------------------------- */

static void
tx_gradient(uint8_t *to, unsigned w, unsigned h, unsigned slice)
{
   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
         uint8_t *t = to + ((size_t)y * w + x) * 4;
         t[0] = (uint8_t)(x * 255 / (w - 1 ? w - 1 : 1));
         t[1] = (uint8_t)(y * 255 / (h - 1 ? h - 1 : 1));
         t[2] = (uint8_t)((x + y) * 127 / (w + h - 2 ? (w + h - 2) / 2 : 1) / 2);
         t[3] = (uint8_t)(255 - x * 7 - slice * 20);
      }
}

static void
gen_mips(id<MTLTexture> t)
{
   id<MTLCommandBuffer> c = [Q commandBuffer];
   id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
   [e generateMipmapsForTexture:t];
   [e endEncoding];
   [c commit];
   [c waitUntilCompleted];
}

static void
mip_check(id<MTLTexture> t, unsigned slice, unsigned levels, unsigned w, bool srgb, const char *word)
{
   static uint8_t above[64 * 64 * 4], level[64 * 64 * 4];
   unsigned aw = w;
   rd(t, 0, slice, 0, aw, aw, 4, above);
   for (unsigned l = 1; l < levels; l++) {
      unsigned lw = aw > 1 ? aw / 2 : 1;
      char name[64];
      snprintf(name, sizeof(name), "%s-L%u", word, l);
      rd(t, l, slice, 0, lw, lw, 4, level);
      int max = 0;
      unsigned bad = tm_box_level(above, aw, aw, level, lw, lw, srgb, srgb ? 2 : 1, &max);
      unsigned exact = 0, total = 0;
      for (unsigned y = 0; y < lw; y++)
         for (unsigned x = 0; x < lw; x++)
            for (unsigned c = 0; c < 4; c++) {
               unsigned sum = 0;
               for (unsigned dy = 0; dy < 2; dy++)
                  for (unsigned dx = 0; dx < 2; dx++)
                     sum += above[((size_t)(y * 2 + dy) * aw + x * 2 + dx) * 4 + c];
               exact += level[((size_t)y * lw + x) * 4 + c] == (sum + 2) / 4;
               total++;
            }
      char row[256] = "";
      for (unsigned x = 0; x < lw && x < 4; x++) {
         const uint8_t *p = level + (size_t)x * 4;
         sprintf(row + strlen(row), " %d,%d,%d,%d", p[0], p[1], p[2], p[3]);
      }
      say("%s %s beyond-tolerance=%u maxstep=%d sum+2/4-exact=%u/%u row0%s", name, bad ? "DIFFER" : "agree", bad, max, exact, total, row);
      memcpy(above, level, (size_t)lw * lw * 4);
      aw = lw;
   }
}

static void
case_mips(int arg)
{
   (void)arg;
   static uint8_t mem[32 * 32 * 4];
   static const struct { const char *word; MTLPixelFormat fmt; MTLTextureType type; unsigned slices; bool srgb; } mip[] = {
      { "blit-mip-rgba8", MTLPixelFormatRGBA8Unorm, MTLTextureType2D, 1, false },
      { "blit-mip-bgra8", MTLPixelFormatBGRA8Unorm, MTLTextureType2D, 1, false },
      { "blit-mip-srgb", MTLPixelFormatRGBA8Unorm_sRGB, MTLTextureType2D, 1, true },
      { "blit-mip-array", MTLPixelFormatRGBA8Unorm, MTLTextureType2DArray, 3, false },
      { "blit-mip-cube", MTLPixelFormatRGBA8Unorm, MTLTextureTypeCube, 6, false },
   };
   for (unsigned i = 0; i < COUNT(mip); i++) {
      id<MTLTexture> a = mk(mip[i].type, mip[i].fmt, 32, 32, 1, mip[i].type == MTLTextureType2DArray ? 3 : 1, 6, 1, USE_RD | USE_RT, SHARED);
      for (unsigned s = 0; s < mip[i].slices; s++) {
         tx_gradient(mem, 32, 32, s);
         put(a, 0, s, 32, 32, 1, 4, mem);
      }
      gen_mips(a);
      for (unsigned s = 0; s < mip[i].slices; s += mip[i].slices > 1 ? mip[i].slices - 1 : 1) {
         char word[64];
         snprintf(word, sizeof(word), "%s-S%u", mip[i].word, s);
         mip_check(a, s, 6, 32, mip[i].srgb, word);
      }
   }
   /* 3D: the eight of each box */
   id<MTLTexture> a = mk(MTLTextureType3D, FMT_RGBA8, 16, 16, 8, 1, 2, 1, USE_RD | USE_RT, SHARED);
   static uint8_t vol[16 * 16 * 8 * 4], lev[8 * 8 * 4 * 4];
   for (unsigned z = 0; z < 8; z++)
      for (unsigned y = 0; y < 16; y++)
         for (unsigned x = 0; x < 16; x++) {
            uint8_t *t = vol + (((size_t)z * 16 + y) * 16 + x) * 4;
            t[0] = (uint8_t)(x * 16);
            t[1] = (uint8_t)(y * 16);
            t[2] = (uint8_t)(z * 32);
            t[3] = (uint8_t)(255 - x * 4 - z * 8);
         }
   put(a, 0, 0, 16, 16, 8, 4, vol);
   gen_mips(a);
   [a getBytes:lev bytesPerRow:8 * 4 bytesPerImage:8 * 8 * 4 fromRegion:MTLRegionMake3D(0, 0, 0, 8, 8, 4) mipmapLevel:1 slice:0];
   int max = 0;
   for (unsigned z = 0; z < 4; z++)
      for (unsigned y = 0; y < 8; y++)
         for (unsigned x = 0; x < 8; x++)
            for (unsigned c = 0; c < 4; c++) {
               unsigned sum = 0;
               for (unsigned k = 0; k < 8; k++)
                  sum += vol[(((size_t)(z * 2 + (k >> 2)) * 16 + y * 2 + ((k >> 1) & 1)) * 16 + x * 2 + (k & 1)) * 4 + c];
               int d = abs((int)lev[(((size_t)z * 8 + y) * 8 + x) * 4 + c] - (int)((sum + 4) / 8));
               max = d > max ? d : max;
            }
   say("blit-mip-3d-L1 %s maxstep=%d (the test allows 1)", max > 1 ? "DIFFER" : "agree", max);
}

static void
case_mip_uint(int arg)
{
   (void)arg;
   id<MTLTexture> a = mk(MTLTextureType2D, MTLPixelFormatRGBA8Uint, 16, 16, 1, 1, 5, 1, USE_RD | USE_RT, SHARED);
   gen_mips(a);
   say("blit-mip-uint generateMipmaps on RGBA8Uint: made, no refusal");
}

/* ---- 11: compare samplers, depth2d ------------------------------------------------------------------------ */

static void
case_compare(int arg)
{
   (void)arg;
   static struct dm_picture want;
   static const char *const fn_name[8] = { "never", "less", "equal", "lessequal", "greater", "notequal", "greaterequal", "always" };
   const float depth = 4.5f / 16.0f;
   id<MTLTexture> z = mk(MTLTextureType2D, MTLPixelFormatDepth32Float, 16, 16, 1, 1, 1, 1, USE_RD | USE_RT, PRIVATE);
   MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
   pd.depthAttachment.texture = z;
   pd.depthAttachment.loadAction = MTLLoadActionClear;
   pd.depthAttachment.clearDepth = depth;
   pd.depthAttachment.storeAction = MTLStoreActionStore;
   run_pass(pd, nil);
   for (unsigned f = 0; f < 8; f++) {
      char name[64];
      snprintf(name, sizeof(name), "sampler-compare-%s", fn_name[f]);
      id<MTLSamplerState> s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 1, f, 1, 0, 1000, 0);
      struct pix_u_t u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
      u.ref = 1.0f / 16;
      if (probe(@"m5b_fcmp", z, s, &u, name)) {
         dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               want.px[y][x][0] = tm_compare(f, ((float)x + 0.5f) / 16, depth) ? 1.0f : 0.0f;
               want.px[y][x][1] = ((float)y + 0.5f) / 16;
               want.px[y][x][2] = 0.25f;
               want.px[y][x][3] = 1.0f;
            }
         judge16(name, &want, TX_EXACT);
         float px[16][4];
         rd(T16, 0, 0, 0, 16, 1, 16, px);
         char a[32] = "", b[32] = "";
         for (unsigned x = 0; x < 16; x++) {
            a[x] = px[x][0] > 0.5f ? '1' : '0';
            b[x] = want.px[0][x][0] > 0.5f ? '1' : '0';
         }
         say("%s ref=(x+.5)/16 depth=4.5/16 x=0..15 metal=%s model=%s", name, a, b);
      }
   }
   /* a depth of known values per pixel: (x + 0.5) / 16, from the shader; its read and its sample */
   id<MTLRenderPipelineState> dp = pso2(@"vquad", @"fdepth", nil, MTLPixelFormatInvalid, NULL, MTLPixelFormatDepth32Float, 1, "depth-write");
   MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
   dd.depthCompareFunction = MTLCompareFunctionAlways;
   dd.depthWriteEnabled = YES;
   id<MTLDepthStencilState> ds = [D newDepthStencilStateWithDescriptor:dd];
   if (!dp)
      return;
   run_pass(pd, ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:dp];
      [e setDepthStencilState:ds];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   });
   dm_clear(&want, (const float[4]){ 0, 0, 0, 1 });
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++)
         want.px[y][x][0] = ((float)x + 0.5f) / 16;
   if (probe(@"fdread", z, nil, NULL, "depth2d-read"))
      judge16("depth2d-read", &want, TX_EXACT);
   id<MTLSamplerState> s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 0, 0, 1, 0, 1000, 0);
   const struct pix_u_t u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
   if (probe(@"fdsample", z, s, &u, "depth2d-sample"))
      judge16("depth2d-sample", &want, TX_EXACT);
   float px[4];
   rd(T16, 0, 0, 0, 1, 1, 16, px);
   say("depth2d-sample (0,0) %.6f model %.6f", (double)px[0], (double)want.px[0][0][0]);
}

/* A depth format through texture2d<float>: what the other three channels are, or a refusal. */
static void
case_depth_as_2d(int arg)
{
   (void)arg;
   id<MTLTexture> z = mk(MTLTextureType2D, MTLPixelFormatDepth32Float, 16, 16, 1, 1, 1, 1, USE_RD | USE_RT, PRIVATE);
   MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor new];
   pd.depthAttachment.texture = z;
   pd.depthAttachment.loadAction = MTLLoadActionClear;
   pd.depthAttachment.clearDepth = 0.25;
   pd.depthAttachment.storeAction = MTLStoreActionStore;
   run_pass(pd, nil);
   const struct pix_u_t u = tx_u(1, 1, 0, 0, 0);
   if (probe(@"m5b_fread", z, nil, &u, "depth-as-texture2d")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("depth-as-texture2d read clear 0.25 -> %.3f %.3f %.3f %.3f", (double)px[0], (double)px[1], (double)px[2], (double)px[3]);
   }
}

/* ---- 12: compressed ----------------------------------------------------------------------------------------- */

struct tx_cformat {
   unsigned mtl;
   const char *name;
   unsigned bw, bh, kind;
};

static const struct tx_cformat cf_table[] = {
   { 170, "EAC_R11Unorm", 4, 4, 4 }, { 174, "EAC_RG11Unorm", 4, 4, 5 },
   { 178, "EAC_RGBA8", 4, 4, 6 }, { 179, "EAC_RGBA8_sRGB", 4, 4, 7 },
   { 180, "ETC2_RGB8", 4, 4, 2 }, { 181, "ETC2_RGB8_sRGB", 4, 4, 3 },
   { 182, "ETC2_RGB8A1", 4, 4, 8 }, { 183, "ETC2_RGB8A1_sRGB", 4, 4, 9 },
   { 204, "ASTC_4x4_LDR", 4, 4, 0 }, { 186, "ASTC_4x4_sRGB", 4, 4, 1 },
   { 212, "ASTC_8x8_LDR", 8, 8, 0 }, { 218, "ASTC_12x12_LDR", 12, 12, 0 },
   { 207, "ASTC_6x5_LDR", 6, 5, 0 },
};

/* air_metal_test.c, tx_block. */
static void
tx_block(const struct tx_cformat *cf, unsigned bx, unsigned by, uint8_t *to, unsigned *size, float out[4])
{
   unsigned r4 = (bx * 3 + 2) & 15, g4 = (by * 3 + 5) & 15, b4 = (bx + by * 2 + 8) & 15;
   unsigned a8 = 40 + bx * 40 + by * 7;
   memset(to, 0, 16);
   if (cf->kind <= 1) {
      static const uint8_t head[8] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
      uint16_t c[4] = { (uint16_t)(r4 * 4369), (uint16_t)(g4 * 4369), (uint16_t)(b4 * 4369), (uint16_t)(a8 * 257) };
      memcpy(to, head, 8);
      memcpy(to + 8, c, 8);
      *size = 16;
      for (unsigned i = 0; i < 4; i++)
         out[i] = (float)c[i] / 65535.0f;
      if (cf->kind == 1)
         for (unsigned i = 0; i < 3; i++)
            out[i] = dm_srgb_decode((float)(c[i] >> 8) / 255.0f);
      return;
   }
   if (cf->kind == 4 || cf->kind == 5) {
      to[0] = (uint8_t)(r4 * 16 + 3);
      out[0] = (float)(to[0] * 8 + 1) / 2047.0f;
      out[1] = 0;
      out[2] = 0;
      out[3] = 1;
      *size = 8;
      if (cf->kind == 5) {
         to[8] = (uint8_t)(g4 * 16 + 3);
         out[1] = (float)(to[8] * 8 + 1) / 2047.0f;
         *size = 16;
      }
      return;
   }
   uint8_t *rgb = to;
   *size = 8;
   out[3] = 1.0f;
   if (cf->kind == 6 || cf->kind == 7) {
      to[0] = (uint8_t)a8;
      out[3] = (float)a8 / 255.0f;
      rgb = to + 8;
      *size = 16;
   }
   unsigned cr, cg, cb;
   if (cf->kind == 8 || cf->kind == 9) {
      unsigned r5 = r4 * 2, g5 = g4 * 2, b5 = b4 * 2;
      rgb[0] = (uint8_t)(r5 << 3);
      rgb[1] = (uint8_t)(g5 << 3);
      rgb[2] = (uint8_t)(b5 << 3);
      rgb[3] = 2;
      cr = ((r5 << 3) | (r5 >> 2)) + 2;
      cg = ((g5 << 3) | (g5 >> 2)) + 2;
      cb = ((b5 << 3) | (b5 >> 2)) + 2;
   } else {
      rgb[0] = (uint8_t)(r4 << 4 | r4);
      rgb[1] = (uint8_t)(g4 << 4 | g4);
      rgb[2] = (uint8_t)(b4 << 4 | b4);
      rgb[3] = 0;
      cr = r4 * 17 + 2;
      cg = g4 * 17 + 2;
      cb = b4 * 17 + 2;
   }
   cr = cr > 255 ? 255 : cr;
   cg = cg > 255 ? 255 : cg;
   cb = cb > 255 ? 255 : cb;
   const bool srgb = cf->kind == 3 || cf->kind == 7 || cf->kind == 9;
   out[0] = srgb ? dm_srgb_decode((float)cr / 255.0f) : (float)cr / 255.0f;
   out[1] = srgb ? dm_srgb_decode((float)cg / 255.0f) : (float)cg / 255.0f;
   out[2] = srgb ? dm_srgb_decode((float)cb / 255.0f) : (float)cb / 255.0f;
}

static void
case_compressed(int arg)
{
   const struct tx_cformat *cf = &cf_table[arg];
   static uint8_t mem[4 * 4 * 16], got[4 * 4 * 16];
   static struct dm_picture want;
   static float colour[4][4][4];
   char name[64];
   const unsigned w = cf->bw * 4, h = cf->bh * 4;
   id<MTLTexture> t = mk2d((MTLPixelFormat)cf->mtl, w, h, 1, USE_RD);
   if (!t)
      return;
   unsigned size = 16;
   dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned by = 0; by < 4; by++)
      for (unsigned bx = 0; bx < 4; bx++) {
         uint8_t block[16];
         tx_block(cf, bx, by, block, &size, colour[by][bx]);
         memcpy(mem + (size_t)(by * 4 + bx) * size, block, size);
      }
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++)
         memcpy(want.px[y][x], colour[y / 4][x / 4], 4 * sizeof(float));
   [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:mem bytesPerRow:4 * size];
   [t getBytes:got bytesPerRow:4 * size fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
   snprintf(name, sizeof(name), "cf-%s-bytes", cf->name);
   tbytes(name, got, mem, 16, size, 4);
   snprintf(name, sizeof(name), "cf-%s-sample", cf->name);
   const struct pix_u_t u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
   if (probe(@"m5b_f2d", t, nearest(), &u, name)) {
      judge16(name, &want, 1.0f / 256.0f);
      float px[16][16][4];
      rd(T16, 0, 0, 0, 16, 16, 16, px);
      say("%s block(1,2) metal %.4f %.4f %.4f %.4f model %.4f %.4f %.4f %.4f", name, (double)px[8][4][0], (double)px[8][4][1],
          (double)px[8][4][2], (double)px[8][4][3], (double)colour[2][1][0], (double)colour[2][1][1], (double)colour[2][1][2],
          (double)colour[2][1][3]);
   }
}

/* ---- 13: every format of tm_formats, and the packed 32-bit words ------------------------------------------- */

static void
case_pf_read(int arg)
{
   const struct tm_format *f = &tm_formats[arg];
   static struct dm_picture want;
   static uint8_t mem[DM_W * DM_H * 16], got[DM_W * DM_H * 16];
   char name[80];
   const bool is_int = f->kind == TM_UINT || f->kind == TM_SINT;
   id<MTLTexture> t = mk2d((MTLPixelFormat)f->mtl, DM_W, DM_H, 1, USE_RD);
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++)
         tm_format_texel(f, x, y, mem + ((size_t)y * DM_W + x) * f->bytes);
   put(t, 0, 0, DM_W, DM_H, 1, f->bytes, mem);
   rd(t, 0, 0, 0, DM_W, DM_H, f->bytes, got);
   snprintf(name, sizeof(name), "pf-%s-bytes", f->name);
   tbytes(name, got, mem, DM_W * DM_H, f->bytes, DM_W);
   dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++)
         tm_format_read(f, mem + ((size_t)y * DM_W + x) * f->bytes, want.px[y][x]);
   snprintf(name, sizeof(name), "pf-%s-read", f->name);
   struct pix_u_t u = tx_u(1, 1, 0, 0, 0.0f);
   if (probe(f->kind == TM_UINT ? @"m5b_freadu" : f->kind == TM_SINT ? @"m5b_freadi" : @"m5b_fread", t, nil, &u, name))
      judge16(name, &want, tm_format_slop(f));
   if (!is_int) {
      snprintf(name, sizeof(name), "pf-%s-sample", f->name);
      u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0.0f);
      if (probe(@"m5b_f2d", t, nearest(), &u, name))
         judge16(name, &want, tm_format_slop(f));
   }
}

static void
case_pf_render(int arg)
{
   const struct tm_format *f = &tm_formats[arg];
   static uint8_t got[DM_W * DM_H * 16], expect[DM_W * DM_H * 16];
   char name[80];
   snprintf(name, sizeof(name), "pf-%s-render", f->name);
   id<MTLTexture> t = mk2d((MTLPixelFormat)f->mtl, DM_W, DM_H, 1, USE_RD | USE_RT);
   NSString *fs = f->kind == TM_UINT ? @"m5b_fuint" : f->kind == TM_SINT ? @"m5b_fsint" : @"m5a_fsrc";
   if (!probe_into(fs, t, 0, 0, 0, nil, nil, NULL, NULL, name))
      return;
   unsigned bad = 0, exact = 0, fx = 0, fy = 0;
   const unsigned steps = f->kind == TM_SRGB ? 5 : f->kind == TM_RG11B10 ? 2 : 1;
   rd(t, 0, 0, 0, DM_W, DM_H, f->bytes, got);
   if (f->kind == TM_RGB9E5) {
      uint32_t w;
      memcpy(&w, got + (3 * DM_W + 5) * 4, 4);
      float d[4];
      tm_format_read(f, (const uint8_t *)&w, d);
      say("%s renders on this GPU; the model has no write for it (renderable=%d in tm_formats). (5,3) word 0x%08x reads %.4f %.4f %.4f", name, f->renderable, w,
          (double)d[0], (double)d[1], (double)d[2]);
      return;
   }
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         float v[4];
         if (f->kind == TM_UINT) {
            v[0] = (float)x;
            v[1] = (float)y;
            v[2] = (float)(x + y);
            v[3] = 3.0f;
         } else if (f->kind == TM_SINT) {
            v[0] = (float)x - 8.0f;
            v[1] = (float)y - 8.0f;
            v[2] = (float)(x + y) - 16.0f;
            v[3] = -1.0f;
         } else {
            dm_src(x, y, v);
         }
         uint8_t *e = expect + ((size_t)y * DM_W + x) * f->bytes;
         const uint8_t *g = got + ((size_t)y * DM_W + x) * f->bytes;
         tm_format_write(f, v, e);
         exact += !memcmp(g, e, f->bytes);
         if (!tm_format_close(f, g, e, steps)) {
            if (!bad) {
               fx = x;
               fy = y;
            }
            bad++;
         }
      }
   say("%s %s wrong=%u/256 exact=%u/256 (the test allows %u steps)", name, bad ? "DIFFER" : "agree", bad, exact, steps);
   if (bad) {
      char a[64] = "", b[64] = "";
      for (unsigned k = 0; k < f->bytes; k++) {
         sprintf(a + strlen(a), " %02x", got[((size_t)fy * DM_W + fx) * f->bytes + k]);
         sprintf(b + strlen(b), " %02x", expect[((size_t)fy * DM_W + fx) * f->bytes + k]);
      }
      say("%s first (%u,%u) metal%s model%s", name, fx, fy, a, b);
   }
}

static void
case_packed_read(int arg)
{
   (void)arg;
   static const struct { unsigned mtl; uint32_t w; } T[] = {
      { 90, 0x000003FF }, { 90, 0x000FFC00 }, { 90, 0x3FF00000 }, { 90, 0xC0000000 }, { 90, 0x40000000 }, { 90, 0x00000200 },
      { 94, 0x000003FF }, { 94, 0x000FFC00 }, { 94, 0x3FF00000 }, { 94, 0xC0000000 },
      { 92, 0x000003C0 }, { 92, 0x3C0u << 11 }, { 92, 0x1E0u << 22 }, { 92, 0x00000001 }, { 92, 0x0000003F }, { 92, 0x00000040 },
      { 92, 0x000007BF }, { 92, 0x000007C0 }, { 92, 0x000007FF }, { 92, 0x1Fu << 22 }, { 92, 1u << 22 },
      { 93, 256u | (16u << 27) }, { 93, (256u << 9) | (16u << 27) }, { 93, (256u << 18) | (16u << 27) }, { 93, 511u | (31u << 27) },
      { 93, 1u | (0u << 27) }, { 93, 0x1FFu | (15u << 27) },
   };
   for (unsigned i = 0; i < COUNT(T); i++) {
      const struct tm_format *f = tm_find(T[i].mtl);
      id<MTLTexture> t = mk2d((MTLPixelFormat)T[i].mtl, 1, 1, 1, USE_RD);
      put(t, 0, 0, 1, 1, 1, 4, &T[i].w);
      id<MTLBuffer> o = buf(NULL, 16);
      run_compute(@"krd", 1, 1, ^(id<MTLComputeCommandEncoder> e) {
         [e setTexture:t atIndex:0];
         [e setBuffer:o offset:0 atIndex:0];
      });
      float *g = o.contents, m[4];
      tm_format_read(f, (const uint8_t *)&T[i].w, m);
      bool same = true;
      for (unsigned c = 0; c < 4; c++)
         if (!(fabsf(g[c] - m[c]) <= fabsf(m[c]) * 1e-5f + 1e-7f) && !(isnan(g[c]) && isnan(m[c])) && !(isinf(g[c]) && isinf(m[c])))
            same = false;
      say("packed-read %s word=0x%08x metal %.9g %.9g %.9g %.9g model %.9g %.9g %.9g %.9g %s", f->name, T[i].w, (double)g[0], (double)g[1],
          (double)g[2], (double)g[3], (double)m[0], (double)m[1], (double)m[2], (double)m[3], same ? "agree" : "DIFFER");
   }
}

/* The shared-exponent encode of the extension that defined the format: no header has it (tm_format_write
 * has no write for it, tex_model says it is not renderable), so this is what the format's definition says. */
static uint32_t
e5_encode(float r, float g, float b)
{
   const double maxv = 511.0 / 512.0 * ldexp(1.0, 16);
#define E5C(x) ((double)((x) > 0 ? ((x) < maxv ? (x) : maxv) : 0))
   double rc = isnan(r) ? 0 : E5C(r), gc = isnan(g) ? 0 : E5C(g), bc = isnan(b) ? 0 : E5C(b);
#undef E5C
   double m = fmax(fmax(rc, gc), bc);
   int e = (m > 0 ? (int)floor(log2(m)) : -100);
   e = (e < -16 ? -16 : e) + 1 + 15;
   double denom = ldexp(1.0, e - 24);
   int ms = (int)floor(m / denom + 0.5);
   if (ms == 512) {
      denom *= 2;
      e++;
   }
   uint32_t ri = (uint32_t)floor(rc / denom + 0.5), gi = (uint32_t)floor(gc / denom + 0.5), bi = (uint32_t)floor(bc / denom + 0.5);
   return ri | gi << 9 | bi << 18 | (uint32_t)e << 27;
}

static const unsigned packed_write_fmt[] = { 90, 94, 92, 93 };

static void
case_packed_write(int arg)
{
   const unsigned mtl = packed_write_fmt[arg];
   const struct tm_format *f = tm_find(mtl);
   static const float vals_float[16][4] = {
      { 1.0f, 0.5f, 0.25f, 1.0f }, { 0.1f, 0.2f, 0.3f, 1.0f }, { 3.14159f, 0.0078125f, 12.0f, 1.0f }, { 1e-5f, 6.103515625e-05f, 3e-5f, 1.0f },
      { 0.99999f, 0.999f, 0.9921875f, 1.0f }, { 65000.0f, 64512.0f, 64512.0f, 1.0f }, { 65024.0f, 65024.0f, 64512.0f, 1.0f }, { 65500.0f, 70000.0f, 70000.0f, 1.0f },
      { 1e9f, 1e9f, 1e9f, 1.0f }, { -1.0f, -0.5f, -1e-5f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0009765625f, 1.0078125f, 1.015625f, 1.0f },
      { (float)INFINITY, 1.0f, 1.0f, 1.0f }, { (float)NAN, 1.0f, 1.0f, 1.0f }, { 0.5f, (float)NAN, 2.0f, 1.0f }, { 1e-7f, 1e-6f, 1e-8f, 1.0f },
   };
   static const float vals_unorm[16][4] = {
      { 1.0f, 0.5f, 0.25f, 1.0f }, { 0.1f, 0.2f, 0.3f, 0.34f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.5f, -0.5f, 2.0f, -1.0f },
      { 0.00048828125f, 0.0009765625f, 0.00146484375f, 0.16f }, { 0.17f, 0.5f, 0.66f, 0.67f }, { 0.83f, 0.84f, 0.99f, 0.5f }, { 0.75f, 0.875f, 0.9375f, 0.75f },
      { 1.0f / 1023.0f, 2.0f / 1023.0f, 3.0f / 1023.0f, 1.0f / 3.0f }, { 511.5f / 1023.0f, 512.5f / 1023.0f, 0.5f, 2.0f / 3.0f }, { 0.25f, 0.25f, 0.25f, 0.25f }, { 0.2f, 0.4f, 0.6f, 0.8f },
      { 0.3f, 0.6f, 0.9f, 0.2f }, { 0.7f, 0.8f, 0.9f, 0.4f }, { 0.11f, 0.22f, 0.33f, 0.6f }, { 0.44f, 0.55f, 0.66f, 0.8f },
   };
   const bool isfloat = mtl == 92 || mtl == 93;
   const float (*vals)[4] = isfloat ? vals_float : vals_unorm;
   id<MTLTexture> t = mk2d((MTLPixelFormat)mtl, 16, 1, 1, USE_RD | USE_RT);
   id<MTLRenderPipelineState> p = pso(@"vquad", @"fvals", nil, (MTLPixelFormat)mtl, f->name);
   if (!p)
      return;
   id<MTLBuffer> vb = buf(vals, 16 * 16);
   run_pass(rpd(t, 0, 0, 0, NULL), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      [e setFragmentBuffer:vb offset:0 atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   });
   uint32_t got[16];
   rd(t, 0, 0, 0, 16, 1, 4, got);
   for (unsigned i = 0; i < 16; i++) {
      uint32_t m = 0;
      const char *which = "model";
      if (mtl == 93) {
         m = e5_encode(vals[i][0], vals[i][1], vals[i][2]);
         which = "definition";
      } else {
         tm_format_write(f, vals[i], (uint8_t *)&m);
      }
      say("packed-write %s in %.9g %.9g %.9g %.9g metal=0x%08x %s=0x%08x %s", f->name, (double)vals[i][0], (double)vals[i][1], (double)vals[i][2],
          (double)vals[i][3], got[i], which, m, got[i] == m ? "agree" : "DIFFER");
   }
}

/* ---- 14: blends ------------------------------------------------------------------------------------------- */

#define DM_ON(sr, dr, opr, sa, da, opa, m) { 1, opr, opa, sr, dr, sa, da, m }
static const struct { const char *name; struct dm_blend b; } blend_cases[] = {
   { "srcover", DM_ON(DM_SA, DM_ISA, DM_ADD, DM_SA, DM_ISA, DM_ADD, DM_ALL) },
   { "premult", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_ALL) },
   { "oneone", DM_ON(DM_ONE, DM_ONE, DM_ADD, DM_ONE, DM_ONE, DM_ADD, DM_ALL) },
   { "min", DM_ON(DM_ONE, DM_ONE, DM_MIN, DM_ONE, DM_ONE, DM_MIN, DM_ALL) },
   { "max", DM_ON(DM_ONE, DM_ONE, DM_MAX, DM_ONE, DM_ONE, DM_MAX, DM_ALL) },
   { "revsub", DM_ON(DM_ONE, DM_ONE, DM_RSUB, DM_ONE, DM_ONE, DM_RSUB, DM_ALL) },
   { "sub", DM_ON(DM_ONE, DM_ONE, DM_SUB, DM_ONE, DM_ONE, DM_SUB, DM_ALL) },
   { "modulate", DM_ON(DM_DC, DM_ZERO, DM_ADD, DM_DC, DM_ZERO, DM_ADD, DM_ALL) },
   { "invdst", DM_ON(DM_IDC, DM_SC, DM_ADD, DM_IDA, DM_SA, DM_ADD, DM_ALL) },
   { "invsrc", DM_ON(DM_ISC, DM_DC, DM_ADD, DM_ISC, DM_DC, DM_ADD, DM_ALL) },
   { "dstalpha", DM_ON(DM_DA, DM_IDA, DM_ADD, DM_DA, DM_IDA, DM_ADD, DM_ALL) },
   { "saturated", DM_ON(DM_SAS, DM_ZERO, DM_ADD, DM_ONE, DM_ZERO, DM_ADD, DM_ALL) },
   { "halves", DM_ON(DM_SA, DM_ISA, DM_ADD, DM_ONE, DM_ONE, DM_MAX, DM_ALL) },
   { "constant", DM_ON(DM_BC, DM_IBC, DM_ADD, DM_BA, DM_IBA, DM_ADD, DM_ALL) },
   { "constalpha", DM_ON(DM_BA, DM_IBA, DM_ADD, DM_IBC, DM_BC, DM_ADD, DM_ALL) },
   { "mask-r", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_R) },
   { "mask-a", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_A) },
   { "mask-rgb", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_R | DM_G | DM_B) },
   { "mask-gba", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_G | DM_B | DM_A) },
   { "mask-gb", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, DM_G | DM_B) },
   { "mask-none", DM_ON(DM_ONE, DM_ISA, DM_ADD, DM_ONE, DM_ISA, DM_ADD, 0) },
   { "off-mask-rb", { 0, 0, 0, 1, 0, 1, 0, DM_R | DM_B } },
   { "off", { 0, 0, 0, 1, 0, 1, 0, DM_ALL } },
};
static const unsigned blend_formats[] = { DM_RGBA8, DM_BGRA8, DM_RGBA8_SRGB, DM_BGRA8_SRGB, DM_RGBA16F, DM_RGBA32F };

static void
case_blend(int arg)
{
   const unsigned fmt = blend_formats[arg];
   static const float constant[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
   static struct dm_picture want;
   static uint8_t mem[DM_W * DM_H * 16], got[DM_W * DM_H * 16];
   unsigned texel = dm_texel_bytes(fmt), agree = 0, total = 0, allexact = 0, allfloat = 0;
   id<MTLTexture> t = mk2d((MTLPixelFormat)fmt, DM_W, DM_H, 1, USE_RD | USE_RT);
   for (unsigned ci = 0; ci < COUNT(blend_cases); ci++) {
      char name[64];
      snprintf(name, sizeof(name), "blend-%s-%s", dm_format_name(fmt), blend_cases[ci].name);
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            float d[4];
            dm_dst(x, y, d);
            dm_encode(fmt, d, mem + ((size_t)y * DM_W + x) * texel);
         }
      put(t, 0, 0, DM_W, DM_H, 1, texel, mem);
      id<MTLRenderPipelineState> p = pso2(@"vquad", @"m5a_fsrc", nil, (MTLPixelFormat)fmt, &blend_cases[ci].b, MTLPixelFormatInvalid, 1, name);
      if (!p)
         continue;
      run_pass(rpd(t, 0, 0, 0, NULL), ^(id<MTLRenderCommandEncoder> e) {
         [e setRenderPipelineState:p];
         [e setBlendColorRed:constant[0] green:constant[1] blue:constant[2] alpha:constant[3]];
         [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      });
      dm_blend_model(&want, fmt, &blend_cases[ci].b, constant, dm_src);
      rd(t, 0, 0, 0, DM_W, DM_H, texel, got);
      total++;
      agree += judge(name, fmt, got, &want, 0.0f) ? 1 : 0;
      /* the model's encoded bytes against Metal's, all of them: Apple's rounding against the model's;
       * and against a blend in plain float with one rounding at the end, which is what the model
       * was before it learned Adreno's integers */
      unsigned same = 0, flt = 0;
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            uint8_t e[16], d0b[16];
            float s[4], d0[4], d[4], out[4];
            dm_encode(fmt, want.px[y][x], e);
            same += !memcmp(got + ((size_t)y * DM_W + x) * texel, e, texel);
            dm_src(x, y, s);
            dm_dst(x, y, d0);
            dm_encode(fmt, d0, d0b);
            dm_decode(fmt, d0b, d);
            if (dm_is_srgb(fmt))
               for (unsigned c = 0; c < 3; c++)
                  d[c] = dm_srgb_decode(d[c]);
            dm_blend_pixel(&blend_cases[ci].b, s, d, constant, dm_is_unorm(fmt), out);
            if (dm_is_srgb(fmt))
               for (unsigned c = 0; c < 3; c++)
                  out[c] = dm_srgb_encode(out[c]);
            dm_encode(fmt, out, e);
            flt += !memcmp(got + ((size_t)y * DM_W + x) * texel, e, texel);
         }
      say("%s pixels bit-exact with the model %u/256, with a plain float blend %u/256", name, same, flt);
      allexact += same == 256;
      allfloat += flt == 256;
   }
   say("blend-%s summary agree=%u/%u whole-picture-exact=%u/%u whole-picture-exact-as-float-blend=%u/%u", dm_format_name(fmt), agree, total,
       allexact, total, allfloat, total);
}

/* ---- 15: A8 and R8 ------------------------------------------------------------------------------------------ */

static void
case_a8_read(int arg)
{
   (void)arg;
   static uint8_t mem[16 * 16];
   for (unsigned i = 0; i < 256; i++)
      mem[i] = (uint8_t)i;
   id<MTLTexture> t = mk2d(MTLPixelFormatA8Unorm, 16, 16, 1, USE_RD);
   put(t, 0, 0, 16, 16, 1, 1, mem);
   float px[4];
   const struct pix_u_t a = tx_u(1, 1, 0, 0, 0), b = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 0);
   if (probe(@"m5b_fread", t, nil, &a, "a8-read")) {
      [T16 getBytes:px bytesPerRow:16 * 16 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
      say("a8-read texel 0x88 -> %.4f %.4f %.4f %.4f model 0 0 0 %.4f", (double)px[0], (double)px[1], (double)px[2], (double)px[3], 136.0 / 255.0);
   }
   if (probe(@"m5b_f2d", t, nearest(), &b, "a8-sample")) {
      [T16 getBytes:px bytesPerRow:16 * 16 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
      say("a8-sample texel 0x88 -> %.4f %.4f %.4f %.4f model 0 0 0 %.4f", (double)px[0], (double)px[1], (double)px[2], (double)px[3], 136.0 / 255.0);
   }
}

static void
case_a8_clear(int arg)
{
   (void)arg;
   id<MTLTexture> t = mk2d(MTLPixelFormatA8Unorm, 16, 16, 1, USE_RD | USE_RT);
   const float c[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
   run_pass(rpd(t, 0, 0, 0, c), nil);
   uint8_t got;
   rd(t, 0, 0, 0, 1, 1, 1, &got);
   say("a8-clear (0.25 0.5 0.75 0.125) byte=%d model (alpha) 32 %s", got, got == 32 ? "agree" : "DIFFER");
}

static void
case_r8_clear(int arg)
{
   (void)arg;
   id<MTLTexture> t = mk2d(MTLPixelFormatR8Unorm, 16, 16, 1, USE_RD | USE_RT);
   const float c[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
   run_pass(rpd(t, 0, 0, 0, c), nil);
   uint8_t got;
   rd(t, 0, 0, 0, 1, 1, 1, &got);
   say("r8-clear (0.25 0.5 0.75 0.125) byte=%d model (red) 64 %s", got, got == 64 ? "agree" : "DIFFER");
}

static void
case_a8_render(int arg)
{
   (void)arg;
   static uint8_t got[16 * 16];
   id<MTLTexture> t = mk2d(MTLPixelFormatA8Unorm, 16, 16, 1, USE_RD | USE_RT);
   if (!probe_into(@"m5a_fsrc", t, 0, 0, 0, nil, nil, NULL, clear0, "a8-render"))
      return;
   rd(t, 0, 0, 0, 16, 16, 1, got);
   float s[4];
   dm_src(5, 3, s);
   say("a8-render (5,3) byte=%d shader alpha would be %d, red %d", got[3 * 16 + 5], dm_unorm8(s[3]), dm_unorm8(s[0]));
}

/* ---- extras: what the tests ask of Metal outside the fifteen, that a Mac can say ------------------------------ */

/* The explicit-LOD and derivative rows of tx_samplers. */
static void
case_lod(int arg)
{
   (void)arg;
   static struct dm_picture want;
   id<MTLTexture> t = sampler_tex();
   id<MTLSamplerState> s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 1, 0, 1, 0, 1000, 0);
   struct pix_u_t u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 1.25f);
   if (probe(@"m5b_f2d", t, s, &u, "sampler-mip-nearest-1.25")) {
      want_texels(&want, 1, 0, 8, 8);
      judge16("sampler-mip-nearest-1.25", &want, TX_EXACT);
   }
   u.lod = 1.75f;
   if (probe(@"m5b_f2d", t, s, &u, "sampler-mip-nearest-1.75")) {
      want_texels(&want, 2, 0, 4, 4);
      judge16("sampler-mip-nearest-1.75", &want, TX_EXACT);
   }
   u.lod = 1.5f;
   if (probe(@"m5b_f2d", t, s, &u, "sampler-mip-nearest-1.5")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("sampler-mip-nearest-1.5 (exactly half) blue's high nibble says level %d", (int)lrintf(px[2] * 255.0f) >> 4);
   }
   s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 2, 0, 1, 0, 1000, 0);
   u.lod = 1.5f;
   if (probe(@"m5b_f2d", t, s, &u, "sampler-mip-linear-1.5")) {
      dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            float a[4], b[4];
            tm_texelf(1, 0, x / 2, y / 2, a);
            tm_texelf(2, 0, x / 4, y / 4, b);
            for (unsigned c = 0; c < 4; c++)
               want.px[y][x][c] = (a[c] + b[c]) / 2.0f;
         }
      judge16("sampler-mip-linear-1.5", &want, TX_FILTERED);
   }
   s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 0, 0, 1, 0, 1000, 0);
   u.lod = 3.0f;
   if (probe(@"m5b_f2d", t, s, &u, "sampler-mip-none")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("sampler-mip-none lod 3 not mipmapped: reads level %d (test row sampler-mip-none: 0)", (int)lrintf(px[2] * 255.0f) >> 4);
   }
   static const struct { const char *name; float scale, lmin, lmax, bias; unsigned level; } cases[] = {
      { "sampler-lod-derivative", 4.0f / 16, 0, 1000, 0, 2 },
      { "sampler-lod-clamp-max", 4.0f / 16, 0, 1.0f, 0, 1 },
      { "sampler-lod-clamp-min", 1.0f / 16, 3.0f, 1000, 0, 3 },
      { "sampler-lod-bias", 1.0f / 16, 0, 1000, 2.0f, 2 },
      { "sampler-lod-bias-clamp", 1.0f / 16, 0, 1.0f, 3.0f, 1 },
   };
   for (unsigned i = 0; i < COUNT(cases); i++) {
      s = smp(TM_REPEAT, TM_REPEAT, TM_EDGE, 0, 0, 1, 0, 1, cases[i].lmin, cases[i].lmax, 0);
      struct pix_u_t v = tx_u(cases[i].scale, cases[i].scale, 0, 0, 0.0f);
      v.bias = cases[i].bias;
      if (probe(@"m5b_f2dimp", t, s, &v, cases[i].name)) {
         unsigned l = cases[i].level, w = 16 >> l;
         dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++)
               tm_texelf(l, 0, (unsigned)tm_wrap(TM_REPEAT, ((float)x + 0.5f) * cases[i].scale, w),
                         (unsigned)tm_wrap(TM_REPEAT, ((float)y + 0.5f) * cases[i].scale, w), want.px[y][x]);
         judge16(cases[i].name, &want, TX_EXACT);
      }
   }
   s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, 1, 0, 1, 0, 1.0f, 0);
   u = tx_u(1.0f / 16, 1.0f / 16, 0, 0, 3.0f);
   if (probe(@"m5b_f2d", t, s, &u, "sampler-lod-clamp-explicit")) {
      float px[4];
      rd(T16, 0, 0, 0, 1, 1, 16, px);
      say("sampler-lod-clamp-explicit clamp [0,1] level(3) in a fragment: reads level %d", (int)lrintf(px[2] * 255.0f) >> 4);
   }
   /* air_metal_test.c's sampler-lod-clamp-explicit rows as they stand (another session's, after the reviewer's run) */
   static const struct { const char *name; float lmin, lmax; unsigned level[4]; } clamps[] = {
      { "2-2", 2.0f, 2.0f, { 2, 2, 2, 2 } },
      { "2-1000", 2.0f, 1000.0f, { 2, 2, 2, 3 } },
      { "0-1", 0.0f, 1.0f, { 0, 1, 1, 1 } },
   };
   for (unsigned c = 0; c < COUNT(clamps); c++)
      for (unsigned mip = 1; mip <= 2; mip++) {
         id<MTLSamplerState> sc = smp(TM_EDGE, TM_EDGE, TM_EDGE, 0, 0, mip, 0, 1, clamps[c].lmin, clamps[c].lmax, 0);
         for (unsigned ask = 0; ask < 4; ask++) {
            char name[80];
            snprintf(name, sizeof(name), "sampler-lod-clamp-explicit-%s-%s-L%u", clamps[c].name, mip == 1 ? "nearest" : "linear", ask);
            struct pix_u_t v = tx_u(1.0f / 16, 1.0f / 16, 0, 0, (float)ask);
            if (probe(@"m5b_f2d", t, sc, &v, name)) {
               unsigned l = clamps[c].level[ask];
               want_texels(&want, l, 0, 16 >> l, 16 >> l);
               judge16(name, &want, TX_EXACT);
            }
         }
      }
   /* the shader's own clamp [1, 2] */
   static const struct { const char *name; float scale; unsigned level; } own[] = {
      { "sampler-own-lodclamp-min", 1.0f / 16, 1 },
      { "sampler-own-lodclamp-max", 16.0f / 16, 2 },
   };
   for (unsigned i = 0; i < COUNT(own); i++) {
      const float sc = own[i].scale;
      struct pix_u_t v = tx_u(sc, sc, 0, 0, 0.0f);
      if (probe(@"m5b_fownlod", t, nil, &v, own[i].name)) {
         unsigned l = own[i].level, w = 16 >> l;
         dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++)
               tm_texelf(l, 0, (unsigned)tm_wrap(TM_EDGE, ((float)x + 0.5f) * sc, w), (unsigned)tm_wrap(TM_EDGE, ((float)y + 0.5f) * sc, w), want.px[y][x]);
         judge16(own[i].name, &want, TX_EXACT);
      }
   }
   /* a linear filter: tm_linear_axis */
   s = smp(TM_EDGE, TM_EDGE, TM_EDGE, 1, 1, 1, 0, 1, 0, 1000, 0);
   const float o = 1.0f / 64.0f;
   u = tx_u(1.0f / 16, 1.0f / 16, o, o, 0.0f);
   if (probe(@"m5b_f2d", t, s, &u, "sampler-linear")) {
      dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            int i0, i1, j0, j1;
            float wx, wy, a[4], b[4], c_[4], d[4];
            tm_linear_axis(TM_EDGE, ((float)x + 0.5f) / 16 + o, 16, &i0, &i1, &wx);
            tm_linear_axis(TM_EDGE, ((float)y + 0.5f) / 16 + o, 16, &j0, &j1, &wy);
            tm_texelf(0, 0, i0, j0, a);
            tm_texelf(0, 0, i1, j0, b);
            tm_texelf(0, 0, i0, j1, c_);
            tm_texelf(0, 0, i1, j1, d);
            for (unsigned c = 0; c < 4; c++)
               want.px[y][x][c] = (a[c] * (1 - wx) + b[c] * wx) * (1 - wy) + (c_[c] * (1 - wx) + d[c] * wx) * wy;
         }
      judge16("sampler-linear", &want, TX_FILTERED);
   }
}

/* The test makes a 1D texture of three levels and samples each: whether a Metal does. */
static void
case_1d_mips(int arg)
{
   (void)arg;
   id<MTLTexture> t = mk(MTLTextureType1D, FMT_RGBA8, 16, 1, 1, 1, 3, 1, USE_RD, SHARED);
   say("tex-1d three levels: %s, levels=%u", t ? "made" : "refused", t ? (unsigned)t.mipmapLevelCount : 0);
}

static void
case_1darray_render(int arg)
{
   (void)arg;
   static uint8_t mem[16 * 4];
   id<MTLTexture> t = mk(MTLTextureType1DArray, FMT_RGBA8, 16, 1, 1, 2, 1, 1, USE_RD | USE_RT, SHARED);
   tm_fill(mem, 0, 0, 16, 1);
   put(t, 0, 0, 16, 1, 1, 4, mem);
   tm_fill(mem, 0, 1, 16, 1);
   put(t, 0, 1, 16, 1, 1, 4, mem);
   if (probe_into(@"m5a_fsrc", t, 0, 1, 0, nil, nil, NULL, NULL, "render-1darray-slice1")) {
      tx_fsrc_bytes(mem, 16, 1);
      check_level("render-1darray-slice1", t, 0, 1, 0, 16, 1, mem);
      tm_fill(mem, 0, 0, 16, 1);
      check_level("render-1darray-slice1-neighbour0", t, 0, 0, 0, 16, 1, mem);
   }
}

static void
case_1d_render(int arg)
{
   (void)arg;
   static uint8_t mem[16 * 4];
   id<MTLTexture> t = mk(MTLTextureType1D, FMT_RGBA8, 16, 1, 1, 1, 1, 1, USE_RD | USE_RT, SHARED);
   if (probe_into(@"m5a_fsrc", t, 0, 0, 0, nil, nil, NULL, clear0, "render-1d")) {
      tx_fsrc_bytes(mem, 16, 1);
      check_level("render-1d", t, 0, 0, 0, 16, 1, mem);
   }
}

static void
case_msarray(int arg)
{
   (void)arg;
   static struct dm_picture want;
   id<MTLTexture> t = mk(MTLTextureType2DMultisampleArray, FMT_RGBA8, 16, 16, 1, 3, 1, 4, USE_RD | USE_RT, PRIVATE);
   if (!t)
      return;
   id<MTLRenderPipelineState> p = pso2(@"vquad", @"m5a_fsrc", nil, FMT_RGBA8, NULL, MTLPixelFormatInvalid, 4, "render-msarray");
   if (!p)
      return;
   run_pass(rpd(t, 0, 1, 0, clear0), ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:p];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   });
   for (unsigned smpn = 0; smpn < 2; smpn++) {
      char name[64];
      snprintf(name, sizeof(name), "render-msarray-slice1-sample%u", smpn * 3);
      struct pix_u_t u = tx_u(1, 1, 0, 0, 0.0f);
      u.index = 1;
      u.select = smpn * 3;
      if (probe(@"m5b_fmsarr", t, nil, &u, name)) {
         dm_clear(&want, (const float[4]){ 0, 0, 0, 0 });
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               float s[4];
               dm_src(x, y, s);
               for (unsigned c = 0; c < 4; c++)
                  want.px[y][x][c] = (float)dm_unorm8(s[c]) / 255.0f;
            }
         judge16(name, &want, TX_EXACT);
      }
   }
}

/* ---- the table -------------------------------------------------------------------------------------------------- */

static const struct { const char *name; void (*f)(int); unsigned subs; } cases[] = {
   { "gpu", case_gpu, 0 },
   { "enums", case_enums, 0 },
   /* 1 */
   { "orient", case_orient, 0 },
   { "viewport", case_viewport, 0 },
   /* 2 */
   { "lines", case_lines, 0 },
   { "points", case_points, 0 },
   /* 3 */
   { "restart", case_restart, 0 },
   /* 4 */
   { "ids", case_ids, 0 },
   { "steps", case_steps, 0 },
   { "step-constant-rate1", case_step_constant1, 0 },
   /* 5 */
   { "views", case_views, 0 },
   { "view-format", case_view_format, COUNT(view_formats) },
   { "swizzle", case_swizzle, 0 },
   { "swizzle-small", case_swizzle_small, 0 },
   { "swizzle-depth", case_swizzle_depth, 0 },
   { "swizzle-descriptor", case_swizzle_descriptor, 0 },
   { "swizzle-render", case_swizzle_render, 0 },
   { "swizzle-write", case_swizzle_write, 0 },
   { "view-write", case_view_write, 0 },
   { "view-format-without-usage", case_view_nofv, 0 },
   { "view-srgb-without-usage", case_view_nofv_srgb, 0 },
   { "view-3d-of-array", case_view_3d_of_array, 0 },
   { "view-2d-of-3d", case_view_2d_of_3d, 0 },
   { "view-another-size-class", case_view_sizeclass, 0 },
   { "view-x32s8", case_view_x32s8, 0 },
   /* 6 */
   { "render-into", case_render_into, 0 },
   { "render-own-level", case_render_own_level, 0 },
   { "render-depth-level", case_render_depth_level, 0 },
   /* 7 */
   { "border-rgba8", case_border_rgba8, 0 },
   { "border-format", case_border_fmt, COUNT(bfmt) },
   { "zero-with-border", case_zero_with_border, 0 },
   { "border-mixed", case_border_mixed, 3 },
   { "zero", case_zero, 0 },
   /* 8 */
   { "cube", case_cube, 0 },
   /* 9 */
   { "addr", case_addr, 0 },
   /* 10 */
   { "mips", case_mips, 0 },
   { "mip-uint", case_mip_uint, 0 },
   /* 11 */
   { "compare", case_compare, 0 },
   { "depth-as-2d", case_depth_as_2d, 0 },
   /* 12 */
   { "compressed", case_compressed, COUNT(cf_table) },
   /* 13 */
   { "pf-read", case_pf_read, COUNT(tm_formats) },
   { "pf-render", case_pf_render, COUNT(tm_formats) },
   { "packed-read", case_packed_read, 0 },
   { "packed-write", case_packed_write, COUNT(packed_write_fmt) },
   /* 14 */
   { "blend", case_blend, COUNT(blend_formats) },
   /* 15 */
   { "a8-read", case_a8_read, 0 },
   { "a8-clear", case_a8_clear, 0 },
   { "r8-clear", case_r8_clear, 0 },
   { "a8-render", case_a8_render, 0 },
   /* extras */
   { "lod", case_lod, 0 },
   { "tex-1d-mips", case_1d_mips, 0 },
   { "render-1d", case_1d_render, 0 },
   { "render-1darray", case_1darray_render, 0 },
   { "render-msarray", case_msarray, 0 },
};

static int
run_one(const char *name, int arg)
{
   for (unsigned i = 0; i < COUNT(cases); i++) {
      if (strcmp(cases[i].name, name))
         continue;
      @autoreleasepool {
         D = MTLCreateSystemDefaultDevice();
         if (!D) {
            say("no Metal device");
            return 1;
         }
         Q = [D newCommandQueue];
         NSError *e = nil;
         L = [D newLibraryWithSource:[NSString stringWithUTF8String:msl_source] options:nil error:&e];
         if (!L) {
            say("compile: %s", e.description.UTF8String);
            return 1;
         }
         T16 = mk2d(MTLPixelFormatRGBA32Float, DM_W, DM_H, 1, USE_RD | USE_RT);
         cases[i].f(arg);
      }
      return 0;
   }
   fprintf(stderr, "no case %s\n", name);
   return 2;
}

/* The one sentence out of what Metal printed on its way down. */
static void
reason_of(const char *text, char *out, size_t cap)
{
   const char *at = strstr(text, "failed assertion");
   if (!at)
      at = strstr(text, "validateWithDevice");
   if (!at)
      at = strstr(text, "Terminating");
   if (!at)
      at = text;
   size_t n = 0;
   bool blank = false;
   for (; *at && n + 1 < cap; at++) {
      bool sp = *at == '\n' || *at == '\t' || *at == ' ';
      if (sp && blank)
         continue;
      out[n++] = sp ? ' ' : *at;
      blank = sp;
   }
   out[n] = '\0';
}

/* Runs one case as a child. `validate` turns the debug layer on and keeps the child's own lines out of ours. */
static void
spawn_case(const char *self, const char *name, int sub, bool validate)
{
   char subs[16], label[80];
   snprintf(subs, sizeof(subs), "%d", sub < 0 ? 0 : sub);
   if (sub >= 0)
      snprintf(label, sizeof(label), "%s[%d]", name, sub);
   else
      snprintf(label, sizeof(label), "%s", name);
   char *const argv[] = { (char *)self, "--case", (char *)name, subs, NULL };
   int errpipe[2];
   if (pipe(errpipe) != 0)
      return;
   posix_spawn_file_actions_t fa;
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_adddup2(&fa, errpipe[1], 2);
   posix_spawn_file_actions_addclose(&fa, errpipe[0]);
   if (validate)
      posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
   char **env = *_NSGetEnviron();
   unsigned n = 0;
   while (env[n])
      n++;
   char **e2 = calloc(n + 3, sizeof(char *));
   memcpy(e2, env, n * sizeof(char *));
   if (validate) {
      e2[n++] = "MTL_DEBUG_LAYER=1";
      e2[n++] = "MTL_DEBUG_LAYER_ERROR_MODE=assert";
   }
   pid_t pid;
   fflush(stdout);
   if (posix_spawn(&pid, self, &fa, NULL, argv, e2) != 0) {
      say("%s could not start", label);
      free(e2);
      return;
   }
   free(e2);
   posix_spawn_file_actions_destroy(&fa);
   close(errpipe[1]);
   static char text[1 << 16];
   size_t got = 0;
   for (;;) {
      ssize_t r = read(errpipe[0], text + got, sizeof(text) - 1 - got);
      if (r <= 0)
         break;
      got += (size_t)r;
      if (got >= sizeof(text) - 1)
         got = sizeof(text) / 2;   /* keep reading, keep the start */
   }
   text[got] = '\0';
   close(errpipe[0]);
   int st = 0;
   waitpid(pid, &st, 0);
   char why[700];
   reason_of(text, why, sizeof(why));
   if (WIFSIGNALED(st)) {
      if (validate)
         say("%s debug-layer REFUSED: %s", label, why);
      else
         say("%s ABORTED signal=%d: %s", label, WTERMSIG(st), why);
   } else if (WEXITSTATUS(st)) {
      say("%s%s exit=%d: %s", label, validate ? " debug-layer" : "", WEXITSTATUS(st), why);
   } else if (validate) {
      say("%s debug-layer accepted", label);
   }
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   /* M5c's rows, tools/air/m5c_rows.h, the same code the phone runs: --m5c LIB, the library
    * airdump.m makes of tools/air/m5c.metal. */
   if (argc >= 3 && !strcmp(argv[1], "--m5c"))
      return m5c_run(MTLCreateSystemDefaultDevice(), argv[2], "metal", false);
   if (argc >= 3 && !strcmp(argv[1], "--case"))
      return run_one(argv[2], argc >= 4 ? atoi(argv[3]) : 0);
   if (argc >= 2 && !strcmp(argv[1], "--list")) {
      for (unsigned i = 0; i < COUNT(cases); i++)
         printf("%s%s\n", cases[i].name, cases[i].subs ? " N" : "");
      return 0;
   }
   bool validate = !(argc >= 2 && !strcmp(argv[1], "--no-validate"));
   char self[4096];
   uint32_t sz = sizeof(self);
   if (_NSGetExecutablePath(self, &sz) != 0)
      return 1;
   for (unsigned pass = 0; pass < (validate ? 2u : 1u); pass++)
      for (unsigned i = 0; i < COUNT(cases); i++) {
         if (!cases[i].subs) {
            spawn_case(self, cases[i].name, -1, pass == 1);
            continue;
         }
         for (unsigned s = 0; s < cases[i].subs; s++)
            spawn_case(self, cases[i].name, (int)s, pass == 1);
      }
   return 0;
}
