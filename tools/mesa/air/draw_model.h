/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * draw_model: what a draw must have left in an attachment, computed on the processor (M5a).
 *
 * The Vulkan back end's first draws that were not a full-screen opaque quad need more than
 * "the same as Gallium": two back ends over one GPU can agree on a wrong picture, and Gallium
 * has no cull, viewport, scissor or blend colour to agree with. So every picture of the M5a
 * tests (air_metal_test's --pixels, through the contract; metal_drv_test's --pixels, through
 * Metal's own API) is compared pixel by pixel with what is computed here, and each prints one
 * line per back end, which two boots' logs can also put side by side:
 *
 *     drvtest: draw-result BACKEND CASE ok|DIFFERS digest HEX expected HEX
 *     drvtest: draw-diff BACKEND CASE N of M pixels differ, first (x,y) got a b c d expected ...
 *
 * Nothing here knows either harness: no Metal, no contract, no back end. Every output varies
 * per pixel, so a constant answer, a mirrored one or one read from the wrong place cannot
 * match. The attachment is DM_W x DM_H.
 *
 * WHAT MAKES THE ARITHMETIC EXACT. Every pattern below is dyadic: x, y are half-odd, and
 * dm_src's values have at most seven bits after the point. So a product of two and a sum of
 * those are exact in binary32 and a float attachment can be compared with ==; only rounding
 * to eight bits needs a tolerance (dm_tolerance). The interpolated colours of a triangle are
 * not exact, and those cases carry a tolerance of their own.
 *
 * VERTICES ON THE SUB-PIXEL GRID. a6xx snaps a window coordinate to 1/256 of a pixel. The
 * triangles below have their corners on 1/16, so a snap moves nothing and which pixel centre
 * is inside is arithmetic. A centre exactly on an edge belongs to whichever triangle the fill
 * rule gives it; dm_triangle says so instead of guessing.
 */

#ifndef DRAW_MODEL_H
#define DRAW_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DM_W 16
#define DM_H 16

/* The picture: every pixel's expected value, in the attachment's own space (an sRGB
 * attachment's colour channels already encoded), and whether the pixel is judged at all. */
struct dm_picture {
   float px[DM_H][DM_W][4];
   uint8_t care[DM_H][DM_W];
};

static inline void
dm_clear(struct dm_picture *p, const float c[4])
{
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         memcpy(p->px[y][x], c, 4 * sizeof(float));
         p->care[y][x] = 1;
      }
}

/* tools/air/m5a_fsrc.ll: the source colour at pixel (x, y). */
static inline void
dm_src(unsigned x, unsigned y, float out[4])
{
   float px = (float)x + 0.5f, py = (float)y + 0.5f;
   out[0] = px / 16.0f;
   out[1] = py / 16.0f;
   out[2] = (px + py) / 32.0f;
   out[3] = (3.0f * px + 5.0f * py) / 128.0f;
}

/* tools/air/m5a_fmrt.ll's second output. */
static inline void
dm_src2(unsigned x, unsigned y, float out[4])
{
   float s[4];
   dm_src(x, y, s);
   out[0] = s[1];
   out[1] = s[2];
   out[2] = s[0];
   out[3] = 1.0f - s[3];
}

/* tools/air/m5a_fcheck.ll: two-pixel squares. */
static inline void
dm_check(unsigned x, unsigned y, float out[4])
{
   unsigned c = ((x >> 1) ^ (y >> 1)) & 1;
   out[0] = (float)c;
   out[1] = 1.0f - (float)c;
   out[2] = 0.25f;
   out[3] = 1.0f;
}

/* What the attachment holds before a blend: unlike the source everywhere, also dyadic. */
static inline void
dm_dst(unsigned x, unsigned y, float out[4])
{
   float px = (float)x + 0.5f, py = (float)y + 0.5f;
   out[0] = (16.0f - px) / 16.0f;
   out[1] = (px + py) / 32.0f;
   out[2] = (16.0f - py) / 16.0f;
   out[3] = (5.0f * px + 3.0f * py) / 128.0f;
}

/* MTLBlendFactor, MTLBlendOperation and MTLColorWriteMask: Apple's own numbers. */
enum {
   DM_ZERO = 0, DM_ONE = 1, DM_SC = 2, DM_ISC = 3, DM_SA = 4, DM_ISA = 5, DM_DC = 6, DM_IDC = 7,
   DM_DA = 8, DM_IDA = 9, DM_SAS = 10, DM_BC = 11, DM_IBC = 12, DM_BA = 13, DM_IBA = 14,
   DM_ADD = 0, DM_SUB = 1, DM_RSUB = 2, DM_MIN = 3, DM_MAX = 4,
   DM_R = 8, DM_G = 4, DM_B = 2, DM_A = 1, DM_ALL = 15,
};

/* One attachment's blend state, in Metal's words. `on` 0 reads only the mask. */
struct dm_blend {
   unsigned on, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, mask;
};

/* Metal's factor for one channel, as Metal's equation has it. */
static inline float
dm_factor(unsigned f, unsigned ch, const float s[4], const float d[4], const float k[4])
{
   switch (f) {
   case DM_ZERO: return 0.0f;
   case DM_ONE:  return 1.0f;
   case DM_SC:   return s[ch];
   case DM_ISC:  return 1.0f - s[ch];
   case DM_SA:   return s[3];
   case DM_ISA:  return 1.0f - s[3];
   case DM_DC:   return d[ch];
   case DM_IDC:  return 1.0f - d[ch];
   case DM_DA:   return d[3];
   case DM_IDA:  return 1.0f - d[3];
   case DM_SAS:  return ch < 3 ? (s[3] < 1.0f - d[3] ? s[3] : 1.0f - d[3]) : 1.0f;
   case DM_BC:   return k[ch];
   case DM_IBC:  return 1.0f - k[ch];
   case DM_BA:   return k[3];
   case DM_IBA:  return 1.0f - k[3];
   }
   return 0.0f;
}

static inline float
dm_clamp01(float v)
{
   return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * One pixel of a blend. `s` and `d` are in the space the blender works in: linear, for an
 * sRGB attachment, which is what makes it a test of that. A normalised attachment clamps the
 * source and the destination's source before and the result after (Vulkan's rule and
 * Metal's), a float one nothing. A channel the mask leaves out keeps `d`.
 */
static inline void
dm_blend_pixel(const struct dm_blend *b, const float s[4], const float d[4], const float k[4],
               bool clamp, float out[4])
{
   for (unsigned ch = 0; ch < 4; ch++) {
      float v;
      if (!b->on) {
         v = s[ch];
      } else {
         unsigned op = ch < 3 ? b->rgb_op : b->alpha_op;
         float sv = s[ch] * dm_factor(ch < 3 ? b->src_rgb : b->src_alpha, ch, s, d, k);
         float dv = d[ch] * dm_factor(ch < 3 ? b->dst_rgb : b->dst_alpha, ch, s, d, k);
         switch (op) {
         case DM_ADD:  v = sv + dv; break;
         case DM_SUB:  v = sv - dv; break;
         case DM_RSUB: v = dv - sv; break;
         case DM_MIN:  v = s[ch] < d[ch] ? s[ch] : d[ch]; break;
         default:      v = s[ch] > d[ch] ? s[ch] : d[ch]; break;
         }
      }
      if (clamp)
         v = dm_clamp01(v);
      out[ch] = (b->mask & (1u << (3 - ch))) ? v : d[ch];
   }
}

/* M5c: the decode a model reads an sRGB texel by, code by code, when it is not the book's: the
 * phone's tests point it at adreno_srgb.h for the rows they judge against that, and only for them. */
static const float *dm_srgb_curve;

/* sRGB's two curves, by the book (Vulkan's and Metal's are this one). */
static inline float
dm_srgb_decode(float e)
{
   /* a small fixed-point-free pow(x, 2.4), so this header needs no libm */
   if (e <= 0.04045f)
      return e / 12.92f;
   double x = (e + 0.055) / 1.055, y = 1.0;
   /* x^2.4 = x^2 * x^0.4, and x^0.4 by Newton on z^5 = x^2 */
   double z = 1.0, xx = x * x;
   for (unsigned i = 0; i < 40; i++)
      z = z - (z * z * z * z * z - xx) / (5.0 * z * z * z * z);
   y = x * x * z;
   return (float)y;
}

static inline float
dm_srgb_encode(float l)
{
   if (l <= 0.0031308f)
      return l * 12.92f;
   /* l^(1/2.4) = l^(5/12): z^12 = l^5, by Newton */
   double x = l, x5 = x * x * x * x * x, z = 1.0;
   for (unsigned i = 0; i < 60; i++) {
      double z11 = z * z * z * z * z * z * z * z * z * z * z;
      z = z - (z11 * z - x5) / (12.0 * z11);
   }
   return (float)(1.055 * z - 0.055);
}

/* A window coordinate from clip coordinates and Metal's viewport (w = 1 throughout). */
struct dm_viewport {
   float x, y, w, h;
};

static inline void
dm_window(const float clip[2], const struct dm_viewport *vp, float win[2])
{
   win[0] = vp->x + vp->w / 2.0f * (clip[0] + 1.0f);
   win[1] = vp->y + vp->h / 2.0f * (1.0f - clip[1]);
}

/* MTLCullMode and MTLWinding, Apple's numbers. */
enum { DM_CULL_NONE = 0, DM_CULL_FRONT = 1, DM_CULL_BACK = 2, DM_CW = 0, DM_CCW = 1 };

/*
 * Whether a triangle counts as culled, from its three window coordinates (y down, row 0 on
 * top). As seen on the attachment it is counter-clockwise when the shoelace sum is negative,
 * and that is Metal's reading whatever the viewport's sign is inside a driver.
 */
static inline bool
dm_culled(const float w[3][2], unsigned cull, unsigned winding)
{
   float sum = 0.0f;
   for (unsigned i = 0; i < 3; i++) {
      unsigned j = (i + 1) % 3;
      sum += w[i][0] * w[j][1] - w[j][0] * w[i][1];
   }
   bool ccw_on_screen = sum < 0.0f;
   bool front = winding == DM_CCW ? ccw_on_screen : !ccw_on_screen;
   return (cull == DM_CULL_FRONT && front) || (cull == DM_CULL_BACK && !front);
}

/* The rectangle a draw may touch: the scissor cut to the attachment. */
struct dm_rect {
   int x0, y0, x1, y1;
};

static inline struct dm_rect
dm_whole(void)
{
   return (struct dm_rect){ 0, 0, DM_W, DM_H };
}

/*
 * One triangle into a picture, colours interpolated by barycentrics at the pixel centre; w is
 * 1, so perspective changes nothing. A centre strictly inside is that triangle's. A centre
 * exactly ON an edge is the fill rule's, and `shared` says what that means here: true, the
 * edge is shared with a neighbour whose colours agree along it, so either triangle's value is
 * right; false, the pixel is not judged (and the picture says so by care 0).
 */
static inline void
dm_triangle(struct dm_picture *p, const float w[3][2], const float col[3][4], struct dm_rect r,
            bool shared)
{
   float a = (w[1][0] - w[0][0]) * (w[2][1] - w[0][1]) - (w[2][0] - w[0][0]) * (w[1][1] - w[0][1]);
   if (a == 0.0f)
      return;
   for (int y = r.y0; y < r.y1; y++) {
      for (int x = r.x0; x < r.x1; x++) {
         float px = (float)x + 0.5f, py = (float)y + 0.5f;
         float b0 = ((w[1][0] - px) * (w[2][1] - py) - (w[2][0] - px) * (w[1][1] - py)) / a;
         float b1 = ((w[2][0] - px) * (w[0][1] - py) - (w[0][0] - px) * (w[2][1] - py)) / a;
         float b2 = 1.0f - b0 - b1;
         if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
            continue;
         bool edge = b0 == 0.0f || b1 == 0.0f || b2 == 0.0f;
         if (edge && !shared) {
            p->care[y][x] = 0;
            continue;
         }
         for (unsigned c = 0; c < 4; c++)
            p->px[y][x][c] = b0 * col[0][c] + b1 * col[1][c] + b2 * col[2][c];
         p->care[y][x] = 1;
      }
   }
}

/* The attachment formats the M5a pictures are read in, by MTLPixelFormat. */
enum {
   DM_RGBA8 = 70, DM_RGBA8_SRGB = 71, DM_BGRA8 = 80, DM_BGRA8_SRGB = 81, DM_RGBA16F = 115,
   DM_RGBA32F = 125,
};

static inline unsigned
dm_texel_bytes(unsigned fmt)
{
   return fmt == DM_RGBA32F ? 16 : fmt == DM_RGBA16F ? 8 : 4;
}

static inline bool
dm_is_srgb(unsigned fmt)
{
   return fmt == DM_RGBA8_SRGB || fmt == DM_BGRA8_SRGB;
}

static inline bool
dm_is_unorm(unsigned fmt)
{
   return fmt == DM_RGBA8 || fmt == DM_BGRA8 || dm_is_srgb(fmt);
}

static inline const char *
dm_format_name(unsigned fmt)
{
   switch (fmt) {
   case DM_RGBA8: return "rgba8";
   case DM_RGBA8_SRGB: return "rgba8srgb";
   case DM_BGRA8: return "bgra8";
   case DM_BGRA8_SRGB: return "bgra8srgb";
   case DM_RGBA16F: return "rgba16f";
   case DM_RGBA32F: return "rgba32f";
   }
   return "?";
}

/* float <-> half for an exact RGBA16Float round trip; every dyadic here fits. */
static inline uint16_t
dm_half(float v)
{
   union { float f; uint32_t u; } t = { v };
   uint32_t sign = (t.u >> 16) & 0x8000, e = (t.u >> 23) & 0xff, m = t.u & 0x7fffff;
   if (e == 0)
      return (uint16_t)sign;
   int he = (int)e - 127 + 15;
   if (he <= 0) {
      if (he < -10)
         return (uint16_t)sign;
      m |= 0x800000;
      return (uint16_t)(sign | (m >> (14 - he)));
   }
   if (he >= 31)
      return (uint16_t)(sign | 0x7c00);
   return (uint16_t)(sign | ((unsigned)he << 10) | (m >> 13));
}

static inline float
dm_unhalf(uint16_t h)
{
   union { float f; uint32_t u; } t;
   uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
   if (e == 0) {
      if (m == 0) {
         t.u = sign;
         return t.f;
      }
      /* a subnormal half is a normal float */
      float v = (float)m / 16777216.0f;
      return (h & 0x8000) ? -v : v;
   }
   t.u = sign | ((e == 31 ? 255 : e - 15 + 127) << 23) | (m << 13);
   return t.f;
}

/* One texel of the attachment's own bytes from a value in the attachment's space. */
static inline void
dm_encode(unsigned fmt, const float v[4], void *to)
{
   if (fmt == DM_RGBA32F) {
      memcpy(to, v, 16);
   } else if (fmt == DM_RGBA16F) {
      uint16_t h[4];
      for (unsigned c = 0; c < 4; c++)
         h[c] = dm_half(v[c]);
      memcpy(to, h, 8);
   } else {
      uint8_t *b = to;
      bool swap = fmt == DM_BGRA8 || fmt == DM_BGRA8_SRGB;
      for (unsigned c = 0; c < 4; c++)
         b[swap && c < 3 ? 2 - c : c] = (uint8_t)(dm_clamp01(v[c]) * 255.0f + 0.5f);
   }
}

static inline void
dm_decode(unsigned fmt, const void *from, float v[4])
{
   if (fmt == DM_RGBA32F) {
      memcpy(v, from, 16);
   } else if (fmt == DM_RGBA16F) {
      uint16_t h[4];
      memcpy(h, from, 8);
      for (unsigned c = 0; c < 4; c++)
         v[c] = dm_unhalf(h[c]);
   } else {
      const uint8_t *b = from;
      bool swap = fmt == DM_BGRA8 || fmt == DM_BGRA8_SRGB;
      for (unsigned c = 0; c < 4; c++)
         v[c] = (float)b[swap && c < 3 ? 2 - c : c] / 255.0f;
   }
}

static inline uint64_t
dm_fnv(const void *bytes, size_t size, uint64_t h)
{
   const uint8_t *b = bytes;
   for (size_t i = 0; i < size; i++)
      h = (h ^ b[i]) * 0x100000001b3ull;
   return h;
}

/*
 * How far one channel may be from the model, in the channel's own value. An eight-bit
 * attachment allows one step of rounding (two on an sRGB one, which goes through both curves);
 * the caller's own tolerance adds to it for what is interpolated.
 *
 * [M5b: the blend model now quantizes first, as the hardware does (dm_blend_model), and that is
 * what the tolerance of a blend rests on: run 094931's fourteen blends that differed by just over
 * one step (blend-rgba8-premult (9,3), 217/255 against 0.855025) are the whole-picture digests of
 * the new model on all 46 unorm8 rows and all 23 rgba16f rows, 69 of 69, so on this GPU those
 * rows are judged at zero steps in fact (digest == expected) and the one step here is what another
 * correct blender may round to (softpipe blends in float). Nothing was widened for them. The sRGB
 * colour channels are the one exception, and they got narrower over most of the range:
 * dm_srgb_band says what they are judged by now, and why.]
 */
static inline float
dm_tolerance(unsigned fmt, float extra)
{
   float t = extra;
   if (dm_is_srgb(fmt))
      t += 2.0f / 255.0f;
   else if (dm_is_unorm(fmt))
      t += 1.0f / 255.0f;
   return t + (dm_is_unorm(fmt) ? 1e-5f : 0.0f);
}

/* One step of a half at this value, for an RGBA16Float attachment: what a blender rounding to the
 * nearest half and one rounding the other way can differ by. The model is exact on this GPU (see
 * dm_blend_model), so this is room for another correct blender and not for this one. */
static inline float
dm_half_step(float v)
{
   uint16_t h = dm_half(v < 0.0f ? -v : v);
   return dm_unhalf((uint16_t)(h + 1)) - dm_unhalf(h);
}

/*
 * What an sRGB attachment's COLOUR channel is judged by: the model in LINEAR space, plus or minus
 * 2^-10, encoded, plus one step of the attachment's rounding. So the band is a fraction of a step
 * wider than rounding where the curve is flat (1.15 steps at 0.8, where the old rule allowed 2)
 * and 4.2 steps wide at black, where one linear 2^-10 IS 3.3 encoded steps (the curve's
 * slope there is 12.92).
 *
 * The number is measured and it is the one thing in this header that is. This is the case the
 * reviewer's rule (quantize as the hardware does; widen only if that cannot reach it) could not
 * close, and the reason it cannot is that the hardware's sRGB curves are not the book's: with
 * blending OFF, where the only arithmetic left is encoding the source, no combination of source
 * precision (float, half, 8 to 16 bits), result precision and rounding reproduces run 094931's
 * digest (blend-rgba8srgb-off, searched on the host, none of 2662 combinations). What the two
 * logged misses say: blend-*srgb-sub (3,6) blue got 7/255, a linear value in [0.00197, 0.00228],
 * where the exact curve gives 0.00303; blend-*srgb-revsub (11,2) green got 15/255, in [0.00464,
 * 0.00491], where it gives 0.00578. Both are 0.0009 to 0.0011 low, which is 2^-10 (0.000977).
 * Both are subtractions near zero, the one place 2^-10 shows: the old rule of two encoded steps
 * failed them by 0.98 and 0.40 of a step, and it let through two whole steps at the top of the
 * range, where a real error is as visible as anywhere.
 */
#define DM_SRGB_LINEAR_SLOP (1.0f / 1024.0f)

static inline void
dm_srgb_band(float want, float extra, float *lo, float *hi)
{
   float l = dm_srgb_decode(dm_clamp01(want));
   float a = l - DM_SRGB_LINEAR_SLOP, b = l + DM_SRGB_LINEAR_SLOP;
   *lo = dm_srgb_encode(a < 0.0f ? 0.0f : a) - 1.0f / 255.0f - extra - 1e-5f;
   *hi = dm_srgb_encode(b > 1.0f ? 1.0f : b) + 1.0f / 255.0f + extra + 1e-5f;
}

/* The coverage of both, row 0 first: '#' a pixel with anything in it, '.' none, '?' not judged,
 * which is what says what rule a line or a point was drawn by. */
static inline void
dm_map(const char *backend, const char *name, unsigned fmt, const void *got,
       const struct dm_picture *want)
{
   unsigned texel = dm_texel_bytes(fmt);
   const uint8_t *g = got;
   printf("drvtest: draw-map %s %s got", backend, name);
   for (unsigned y = 0; y < DM_H; y++) {
      char row[DM_W + 1];
      for (unsigned x = 0; x < DM_W; x++) {
         float v[4];
         dm_decode(fmt, g + ((size_t)y * DM_W + x) * texel, v);
         row[x] = !want->care[y][x] ? '?'
                : (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f) ? '#' : '.';
      }
      row[DM_W] = '\0';
      printf(" %s", row);
   }
   printf(" want");
   for (unsigned y = 0; y < DM_H; y++) {
      char row[DM_W + 1];
      for (unsigned x = 0; x < DM_W; x++) {
         const float *v = want->px[y][x];
         row[x] = !want->care[y][x] ? '?'
                : (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f) ? '#' : '.';
      }
      row[DM_W] = '\0';
      printf(" %s", row);
   }
   printf("\n");
}

/* M5b: what a right picture is called. "ok" but for a row judged by something other than real Metal,
 * which says so: the Gallium library's mirrored 16-bit formats (tex_model.h, tm_gallium_mirror). */
static const char *dm_right_word = "ok";

/*
 * The verdict: one result line, and for a picture that differs one line more with the first
 * pixel that does and how many do. `lacks` is a case the back end is known not to serve (the
 * Gallium library reads none of M5a's encoder fields): there a difference is printed as what it
 * is and not as a failure. Returns whether the picture is right. `got` is the attachment's own
 * bytes, DM_W x DM_H tight.
 */
static inline bool
dm_verdict(const char *backend, const char *name, unsigned fmt, const void *got,
           const struct dm_picture *want, float extra, bool lacks)
{
   unsigned texel = dm_texel_bytes(fmt), wrong = 0, judged = 0, fx = 0, fy = 0;
   float fg[4] = { 0 }, fw[4] = { 0 }, tol = dm_tolerance(fmt, extra);
   uint64_t dg = 0xcbf29ce484222325ull, dw = dg;
   const uint8_t *g = got;

   for (unsigned y = 0; y < DM_H; y++) {
      for (unsigned x = 0; x < DM_W; x++) {
         uint8_t expected[16];
         float v[4];
         const uint8_t *at = g + ((size_t)y * DM_W + x) * texel;
         dm_encode(fmt, want->px[y][x], expected);
         /* a pixel not judged is not in either digest, so the two can be equal */
         if (want->care[y][x]) {
            dg = dm_fnv(at, texel, dg);
            dw = dm_fnv(expected, texel, dw);
         }
         dm_decode(fmt, at, v);
         if (!want->care[y][x])
            continue;
         judged++;
         bool same = true;
         for (unsigned c = 0; c < 4; c++) {
            float d = v[c] - want->px[y][x][c];
            if (d < 0.0f)
               d = -d;
            /* [M5b: the three colour channels of an sRGB attachment by their band, and its
             * alpha, which no curve touches, by the unorm rule; a half by its own step] */
            if (dm_is_srgb(fmt) && c < 3) {
               float lo, hi;
               dm_srgb_band(want->px[y][x][c], extra, &lo, &hi);
               if (!(v[c] >= lo && v[c] <= hi))
                  same = false;
               continue;
            }
            float t = dm_is_srgb(fmt) ? extra + 1.0f / 255.0f + 1e-5f
                    : fmt == DM_RGBA16F ? extra + dm_half_step(want->px[y][x][c]) : tol;
            /* NaN and anything past the tolerance alike */
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
   }
   const char *word = !wrong ? dm_right_word : lacks ? "gallium-lacks" : "DIFFERS";
   printf("drvtest: draw-result %s %s %s digest %016llx expected %016llx\n", backend, name, word,
          (unsigned long long)dg, (unsigned long long)dw);
   if (wrong && !lacks)
      printf("drvtest: draw-diff %s %s %u of %u pixels differ, first (%u,%u) got %.9g %.9g %.9g "
             "%.9g expected %.9g %.9g %.9g %.9g\n", backend, name, wrong, judged, fx, fy,
             (double)fg[0], (double)fg[1], (double)fg[2], (double)fg[3], (double)fw[0],
             (double)fw[1], (double)fw[2], (double)fw[3]);
   if (wrong)
      dm_map(backend, name, fmt, got, want);
   fflush(stdout);
   return !wrong;
}

/*
 * What a blended attachment must hold: the destination as those bytes really decode (an
 * eight-bit one is rounded, an sRGB one's colour is decoded to linear), blended, and encoded
 * again. So the sRGB rows are the linear-space model and a back end that blends the encoded
 * values misses it by far more than rounding, over most of the attachment.
 *
 * [M5b: AND THE BLEND IN THE PRECISION THE HARDWARE BLENDS IN, which is what run 094931 asked for:
 * blend-rgba8-premult (9,3) got 217/255 where this used to say 0.855025 = 218.03/255, and thirteen
 * more rows like it, with vk's digest EQUAL to Gallium's, so the two agreed on something this
 * model did not. Working the pixel: s = 151, Sa = 92 (0.359375 * 255 = 91.64 rounded), d = 104;
 * 151 + 104 * (255 - 92) / 255 = 151 + 66.48, 217. The model had kept the source in float, where
 * Sa is 91.64.
 *
 * The rule, by format, each one checked against run 094931's WHOLE-PICTURE digests on the host and
 * not only against the pixels the log printed. The digest covers every byte, so one of these
 * being equal is 256 pixels exact:
 *
 *   rgba8, bgra8   INTEGERS. The source rounded to eight bits before anything, the destination
 *                  its own byte, every factor computed from those eight-bit values, and ONE
 *                  rounding, after the sum of the two products: (s*Fs + d*Fd + 127) / 255, clamped.
 *                  46 of 46 rows exact (23 blends, two formats). A rounding per product, which
 *                  is the first thing tried, gets 34 of 46; truncating gets 16.
 *   rgba16f        HALVES. The source and the destination halves, each PRODUCT rounded to the
 *                  nearest half (ties to even) and the sum rounded again. 23 of 23 exact. One
 *                  rounding after the sum, which is what the integers do, gets 20.
 *   rgba32f        floats: nothing rounds on these dyadic values, and this was exact already.
 *   sRGB           the alpha channel is rgba8's integers (no curve touches it) and the colour
 *                  channels are the linear-space float blend this always was: the hardware's
 *                  curves are not the book's, so these are not exact, and dm_srgb_band says what
 *                  they are judged by instead.
 *
 * MIN and MAX ignore the factors in all four, as dm_blend_pixel says.]
 */
static inline int
dm_unorm8(float v)
{
   return (int)(dm_clamp01(v) * 255.0f + 0.5f);
}

static inline int
dm_factor8(unsigned f, unsigned ch, const int s[4], const int d[4], const int k[4])
{
   switch (f) {
   case DM_ZERO: return 0;
   case DM_ONE:  return 255;
   case DM_SC:   return s[ch];
   case DM_ISC:  return 255 - s[ch];
   case DM_SA:   return s[3];
   case DM_ISA:  return 255 - s[3];
   case DM_DC:   return d[ch];
   case DM_IDC:  return 255 - d[ch];
   case DM_DA:   return d[3];
   case DM_IDA:  return 255 - d[3];
   case DM_SAS:  return ch < 3 ? (s[3] < 255 - d[3] ? s[3] : 255 - d[3]) : 255;
   case DM_BC:   return k[ch];
   case DM_IBC:  return 255 - k[ch];
   case DM_BA:   return k[3];
   case DM_IBA:  return 255 - k[3];
   }
   return 0;
}

/* One channel, in eight bits: see above. */
static inline int
dm_blend_channel8(const struct dm_blend *b, unsigned ch, const int s[4], const int d[4],
                  const int k[4])
{
   int v;
   if (!b->on) {
      v = s[ch];
   } else {
      unsigned op = ch < 3 ? b->rgb_op : b->alpha_op;
      int sp = s[ch] * dm_factor8(ch < 3 ? b->src_rgb : b->src_alpha, ch, s, d, k);
      int dp = d[ch] * dm_factor8(ch < 3 ? b->dst_rgb : b->dst_alpha, ch, s, d, k);
      if (op == DM_MIN) {
         v = s[ch] < d[ch] ? s[ch] : d[ch];
      } else if (op == DM_MAX) {
         v = s[ch] > d[ch] ? s[ch] : d[ch];
      } else {
         int sum = op == DM_ADD ? sp + dp : op == DM_SUB ? sp - dp : dp - sp;
         v = (sum < 0 ? 0 : sum + 127) / 255;
      }
      v = v > 255 ? 255 : v;
   }
   return (b->mask & (1u << (3 - ch))) ? v : d[ch];
}

/* A float to the nearest half, ties to even, and back: dm_half truncates, which is right for its
 * one job (a value that fits), and a blender rounds. */
static inline float
dm_round_half(float v)
{
   union { float f; uint32_t u; } t = { v };
   uint32_t sign = (t.u >> 16) & 0x8000, e = (t.u >> 23) & 0xff, m = t.u & 0x7fffff;
   int he = (int)e - 127 + 15;
   uint32_t out;
   if (e == 0 || he < -10) {
      out = 0;
   } else if (he <= 0) {
      unsigned sh = (unsigned)(14 - he);
      uint32_t all = m | 0x800000, r = all >> sh, rem = all & ((1u << sh) - 1), half = 1u << (sh - 1);
      out = r + (rem > half || (rem == half && (r & 1)));
   } else if (he >= 31) {
      out = 0x7c00;
   } else {
      uint32_t r = m >> 13, rem = m & 0x1fff;
      out = (((uint32_t)he << 10) | r) + (rem > 0x1000 || (rem == 0x1000 && (r & 1)));
   }
   return dm_unhalf((uint16_t)(sign | out));
}

static inline void
dm_blend_model(struct dm_picture *p, unsigned fmt, const struct dm_blend *b, const float k[4],
                void (*source)(unsigned, unsigned, float[4]))
{
   dm_clear(p, (const float[4]){ 0, 0, 0, 0 });
   int k8[4];
   for (unsigned c = 0; c < 4; c++)
      k8[c] = dm_unorm8(k[c]);
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         float s[4], d0[4], d[4], out[4];
         uint8_t bytes[16];
         source(x, y, s);
         dm_dst(x, y, d0);
         dm_encode(fmt, d0, bytes);
         dm_decode(fmt, bytes, d);
         /* the destination in eight bits, from the same decode, and the source rounded to it */
         int s8[4], d8[4];
         for (unsigned c = 0; c < 4; c++) {
            s8[c] = dm_unorm8(s[c]);
            d8[c] = dm_unorm8(d[c]);
         }
         if (dm_is_srgb(fmt))
            for (unsigned c = 0; c < 3; c++)
               d[c] = dm_srgb_curve ? dm_srgb_curve[dm_unorm8(d[c])] : dm_srgb_decode(d[c]);
         if (fmt == DM_RGBA16F) {
            /* each product to a half and the sum again (see above) */
            struct dm_blend one = *b;
            float whole[4];
            one.mask = DM_ALL;
            dm_blend_pixel(&one, s, d, k, false, whole);
            for (unsigned c = 0; c < 4; c++) {
               unsigned op = c < 3 ? b->rgb_op : b->alpha_op;
               float v = whole[c];
               if (b->on && op < DM_MIN) {
                  float sv = dm_round_half(s[c] * dm_factor(c < 3 ? b->src_rgb : b->src_alpha, c, s, d, k));
                  float dv = dm_round_half(d[c] * dm_factor(c < 3 ? b->dst_rgb : b->dst_alpha, c, s, d, k));
                  v = op == DM_ADD ? sv + dv : op == DM_SUB ? sv - dv : dv - sv;
               }
               out[c] = (b->mask & (1u << (3 - c))) ? dm_round_half(v) : d[c];
            }
         } else if (dm_is_srgb(fmt)) {
            dm_blend_pixel(b, s, d, k, true, out);
            for (unsigned c = 0; c < 3; c++)
               out[c] = dm_srgb_encode(out[c]);
            /* alpha: eight bits, the factors that read colour reading the same eight bits */
            out[3] = (float)dm_blend_channel8(b, 3, s8, d8, k8) / 255.0f;
         } else if (dm_is_unorm(fmt)) {
            for (unsigned c = 0; c < 4; c++)
               out[c] = (float)dm_blend_channel8(b, c, s8, d8, k8) / 255.0f;
         } else {
            dm_blend_pixel(b, s, d, k, false, out);
         }
         memcpy(p->px[y][x], out, sizeof(out));
      }
}

/* A refusal, in the same family of lines, so one grep finds every case of a run. */
static inline void
dm_refused(const char *backend, const char *name, const char *why)
{
   printf("drvtest: draw-result %s %s refused: %s\n", backend, name, why);
   fflush(stdout);
}

#endif /* DRAW_MODEL_H */
