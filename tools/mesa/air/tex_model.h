/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * tex_model: what a texture must hold and a sampler must answer, computed on the processor (M5b).
 *
 * The same reason as draw_model.h, whose lines and verdict this uses: two back ends over one GPU
 * can agree on a wrong picture, and Gallium has no view, no border colour and no pass into a level
 * to agree with. Nothing here knows either harness, so air_metal_test (through the contract) and
 * metal_drv_test (through Metal's API) judge by the same numbers.
 *
 * WHAT IS EXACT AND WHAT IS NOT. A nearest sample of an eight-bit texel is k/255 in a float
 * attachment, so the tolerance there is the float's. A linear filter or a mip blend of those is
 * dyadic weights over them, and the texture unit rounds to its own fixed point: one step of eight
 * bits, which is what every caller passes for those. A box filter is the same in a blit
 * (tm_box_level says what is asked of it).
 */

#ifndef TEX_MODEL_H
#define TEX_MODEL_H

#include "draw_model.h"

#include <math.h>

/* ---- texels that say where they are ---------------------------------------------------- */

/*
 * One texel of RGBA8Unorm, unique over level, slice, x and y, up to 16 levels and slices and a
 * 16x16 base: x and y in the high nibble of red and green and the level and slice beside them, the
 * two again in blue, and alpha unlike all of them, so a read of the wrong level, the wrong slice,
 * the wrong row or a swapped pair of channels cannot be the same bytes.
 */
static inline void
tm_texel(unsigned level, unsigned slice, unsigned x, unsigned y, uint8_t out[4])
{
   out[0] = (uint8_t)(x << 4 | level);
   out[1] = (uint8_t)(y << 4 | slice);
   out[2] = (uint8_t)(level << 4 | slice);
   out[3] = (uint8_t)(255 - (x ^ (y << 3) ^ (slice << 5)));
}

/* A whole level of them, tight, w x h. */
static inline void
tm_fill(uint8_t *to, unsigned level, unsigned slice, unsigned w, unsigned h)
{
   for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
         tm_texel(level, slice, x, y, to + ((size_t)y * w + x) * 4);
}

static inline void
tm_texelf(unsigned level, unsigned slice, unsigned x, unsigned y, float out[4])
{
   uint8_t b[4];
   tm_texel(level, slice, x, y, b);
   for (unsigned c = 0; c < 4; c++)
      out[c] = (float)b[c] / 255.0f;
}

/* ---- MTLSamplerAddressMode, by the book -------------------------------------------------- */

enum {
   TM_EDGE = 0, TM_MIRROR_EDGE = 1, TM_REPEAT = 2, TM_MIRROR = 3, TM_ZERO = 4, TM_BORDER = 5,
};

/* Which texel a nearest sample at this normalised coordinate reads on one axis, or -1 for the
 * border. The wrap is on the texel's INDEX, after the floor of u times the size. This used to wrap
 * u and floor afterwards, which is the same except exactly on a texel's edge, where a reflection
 * lands on the other side of it: real Metal, on the 97 multiples of 1/16 in [-3, 3] of
 * tools/air/mtlref's addr-sweep on an M1 Max, gives this form 0 wrong in all six modes and the old
 * one 9 wrong on mirror and 3 on mirror-edge. No row of the tests reached those. */
static inline int
tm_wrap(unsigned mode, float u, unsigned size)
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

/* The two coordinate halves of a linear filter on one axis: which two texels and how much of the
 * second, or -1 for a border. For the address modes that clamp and repeat; what the tests ask for. */
static inline void
tm_linear_axis(unsigned mode, float u, unsigned size, int *i0, int *i1, float *w1)
{
   float p = u * (float)size - 0.5f;
   float fl = (float)floor(p);
   *w1 = p - fl;
   int a = (int)fl, b = a + 1, n = (int)size;
   if (mode == TM_REPEAT) {
      a = ((a % n) + n) % n;
      b = ((b % n) + n) % n;
   } else if (mode == TM_ZERO || mode == TM_BORDER) {
      a = a < 0 || a >= n ? -1 : a;
      b = b < 0 || b >= n ? -1 : b;
   } else {
      a = a < 0 ? 0 : (a >= n ? n - 1 : a);
      b = b < 0 ? 0 : (b >= n ? n - 1 : b);
   }
   *i0 = a;
   *i1 = b;
}

/* MTLCompareFunction: whether a comparison passes. Metal's reference is on the left, as Vulkan's. */
static inline bool
tm_compare(unsigned fn, float ref, float depth)
{
   switch (fn) {
   case 0: return false;
   case 1: return ref < depth;
   case 2: return ref == depth;
   case 3: return ref <= depth;
   case 4: return ref > depth;
   case 5: return ref != depth;
   case 6: return ref >= depth;
   default: return true;
   }
}

/* ---- a cube, as both APIs define it -------------------------------------------------------- */

/* Which face and where on it a direction lands (the table in Vulkan's "Cube Map Face Selection"
 * and Apple's face order, +X -X +Y -Y +Z -Z). `*u`, `*v` are in [0, 1]. */
static inline unsigned
tm_cube_face(const float d[3], float *u, float *v)
{
   float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]), ma, sc, tc;
   unsigned face;
   if (ax >= ay && ax >= az) {
      face = d[0] > 0 ? 0 : 1;
      ma = ax;
      sc = d[0] > 0 ? -d[2] : d[2];
      tc = -d[1];
   } else if (ay >= az) {
      face = d[1] > 0 ? 2 : 3;
      ma = ay;
      sc = d[0];
      tc = d[1] > 0 ? d[2] : -d[2];
   } else {
      face = d[2] > 0 ? 4 : 5;
      ma = az;
      sc = d[2] > 0 ? d[0] : -d[0];
      tc = -d[1];
   }
   *u = 0.5f * (sc / ma + 1.0f);
   *v = 0.5f * (tc / ma + 1.0f);
   return face;
}

/* The three vectors of tools/air/m5b_fcube.ll per face: the direction at pixel (x, y) of a 16x16
 * attachment is major + s * S + t * T, s and t in (-1, 1), chosen so that the face's own u and v
 * are (x + 0.5) / 16 and (y + 0.5) / 16: the attachment is the face, texel for texel. */
static inline void
tm_cube_axes(unsigned face, float major[3], float S[3], float T[3])
{
   static const float tab[6][3][3] = {
      { { 1, 0, 0 }, { 0, 0, -1 }, { 0, -1, 0 } },
      { { -1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } },
      { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
      { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
      { { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } },
      { { 0, 0, -1 }, { -1, 0, 0 }, { 0, -1, 0 } },
   };
   memcpy(major, tab[face][0], 3 * sizeof(float));
   memcpy(S, tab[face][1], 3 * sizeof(float));
   memcpy(T, tab[face][2], 3 * sizeof(float));
}

/* ---- pixel formats ------------------------------------------------------------------------- */

enum tm_kind {
   TM_UNORM, TM_SNORM, TM_UINT, TM_SINT, TM_FLOAT, TM_SRGB,   /* a channel of `bits` */
   TM_RG11B10, TM_RGB9E5,                                     /* a word of three shared */
};

/*
 * One MTLPixelFormat's memory: `bytes` per texel, `kind` of every channel but sRGB's alpha, each
 * channel's width and where it starts counting bits from bit 0 of the little-endian word (or, for
 * a format of whole bytes and whole words, from byte 0, which on this machine is the same number),
 * width 0 for a channel it has not got. So this table IS the claim about each format's layout, and
 * air_vk.c's and air_metal.c's tables are judged against it.
 *
 * THE FOUR PACKED 16-BIT ONES (40-43) ARE WHAT WAS MEASURED, not Apple's words, because Apple's
 * words do not say: MTLPixelFormat.h has the four names under "Packed 16 bit formats" and
 * developer.apple.com's page for b5g6r5Unorm gives the widths and not the order.
 *
 * MEASURED ON REAL METAL ON AN M1 MAX, NOT A11: the public API, a word with one field set, read back
 * with texture.read. The first-named channel is in the low bits, the rule of the packed 32-bit ones:
 *
 *    40 B5G6R5   0x001F b   0x07E0 g   0xF800 r
 *    41 A1BGR5   0x0001 a   0x003E b   0x07C0 g   0xF800 r
 *    42 ABGR4    0x000F a   0x00F0 b   0x0F00 g   0xF000 r
 *    43 BGR5A1   0x001F b   0x03E0 g   0x7C00 r   0x8000 a
 *
 * The same run measured the bit layout of A8Unorm (128 reads (0, 0, 0, 0.502)), RGB10A2, BGR10A2,
 * RG11B10 and RGB9E5, and this table agrees with it: bit layout only, not the conversion of a value,
 * and not a compressed block's. The Gallium library has the mirror of the four, red and blue
 * exchanged, on purpose (air_metal.c says why, and that the source of the exchange is open):
 * tm_gallium_mirror has its numbers, and its rows print "gallium-mirror".
 */
struct tm_format {
   unsigned mtl;
   const char *name;
   enum tm_kind kind;
   unsigned bytes;
   unsigned bits[4], at[4];     /* r g b a */
   bool renderable;             /* the claim: the back end can render into it */
};

static const struct tm_format tm_formats[] = {
   {   1, "A8Unorm",        TM_UNORM, 1, { 0, 0, 0, 8 }, { 0, 0, 0, 0 }, true },
   {  10, "R8Unorm",        TM_UNORM, 1, { 8, 0, 0, 0 }, { 0 }, true },
   {  11, "R8Unorm_sRGB",   TM_SRGB,  1, { 8, 0, 0, 0 }, { 0 }, true },
   {  12, "R8Snorm",        TM_SNORM, 1, { 8, 0, 0, 0 }, { 0 }, true },
   {  13, "R8Uint",         TM_UINT,  1, { 8, 0, 0, 0 }, { 0 }, true },
   {  14, "R8Sint",         TM_SINT,  1, { 8, 0, 0, 0 }, { 0 }, true },
   {  20, "R16Unorm",       TM_UNORM, 2, { 16, 0, 0, 0 }, { 0 }, true },
   {  22, "R16Snorm",       TM_SNORM, 2, { 16, 0, 0, 0 }, { 0 }, true },
   {  23, "R16Uint",        TM_UINT,  2, { 16, 0, 0, 0 }, { 0 }, true },
   {  24, "R16Sint",        TM_SINT,  2, { 16, 0, 0, 0 }, { 0 }, true },
   {  25, "R16Float",       TM_FLOAT, 2, { 16, 0, 0, 0 }, { 0 }, true },
   {  30, "RG8Unorm",       TM_UNORM, 2, { 8, 8, 0, 0 }, { 0, 8 }, true },
   {  31, "RG8Unorm_sRGB",  TM_SRGB,  2, { 8, 8, 0, 0 }, { 0, 8 }, true },
   {  32, "RG8Snorm",       TM_SNORM, 2, { 8, 8, 0, 0 }, { 0, 8 }, true },
   {  33, "RG8Uint",        TM_UINT,  2, { 8, 8, 0, 0 }, { 0, 8 }, true },
   {  34, "RG8Sint",        TM_SINT,  2, { 8, 8, 0, 0 }, { 0, 8 }, true },
   {  40, "B5G6R5Unorm",    TM_UNORM, 2, { 5, 6, 5, 0 }, { 11, 5, 0, 0 }, true },
   {  41, "A1BGR5Unorm",    TM_UNORM, 2, { 5, 5, 5, 1 }, { 11, 6, 1, 0 }, true },
   {  42, "ABGR4Unorm",     TM_UNORM, 2, { 4, 4, 4, 4 }, { 12, 8, 4, 0 }, true },
   {  43, "BGR5A1Unorm",    TM_UNORM, 2, { 5, 5, 5, 1 }, { 10, 5, 0, 15 }, true },
   {  53, "R32Uint",        TM_UINT,  4, { 32, 0, 0, 0 }, { 0 }, true },
   {  54, "R32Sint",        TM_SINT,  4, { 32, 0, 0, 0 }, { 0 }, true },
   {  55, "R32Float",       TM_FLOAT, 4, { 32, 0, 0, 0 }, { 0 }, true },
   {  60, "RG16Unorm",      TM_UNORM, 4, { 16, 16, 0, 0 }, { 0, 16 }, true },
   {  62, "RG16Snorm",      TM_SNORM, 4, { 16, 16, 0, 0 }, { 0, 16 }, true },
   {  63, "RG16Uint",       TM_UINT,  4, { 16, 16, 0, 0 }, { 0, 16 }, true },
   {  64, "RG16Sint",       TM_SINT,  4, { 16, 16, 0, 0 }, { 0, 16 }, true },
   {  65, "RG16Float",      TM_FLOAT, 4, { 16, 16, 0, 0 }, { 0, 16 }, true },
   {  70, "RGBA8Unorm",     TM_UNORM, 4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 }, true },
   {  71, "RGBA8Unorm_sRGB", TM_SRGB, 4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 }, true },
   {  72, "RGBA8Snorm",     TM_SNORM, 4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 }, true },
   {  73, "RGBA8Uint",      TM_UINT,  4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 }, true },
   {  74, "RGBA8Sint",      TM_SINT,  4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 }, true },
   {  80, "BGRA8Unorm",     TM_UNORM, 4, { 8, 8, 8, 8 }, { 16, 8, 0, 24 }, true },
   {  81, "BGRA8Unorm_sRGB", TM_SRGB, 4, { 8, 8, 8, 8 }, { 16, 8, 0, 24 }, true },
   /* red in the low bits and 2 bits of alpha on top: measured with the 16-bit ones, above */
   {  90, "RGB10A2Unorm",   TM_UNORM, 4, { 10, 10, 10, 2 }, { 0, 10, 20, 30 }, true },
   {  91, "RGB10A2Uint",    TM_UINT,  4, { 10, 10, 10, 2 }, { 0, 10, 20, 30 }, true },
   {  92, "RG11B10Float",   TM_RG11B10, 4, { 11, 11, 10, 0 }, { 0, 11, 22, 0 }, true },
   {  93, "RGB9E5Float",    TM_RGB9E5, 4, { 9, 9, 9, 0 }, { 0, 9, 18, 0 }, false },
   {  94, "BGR10A2Unorm",   TM_UNORM, 4, { 10, 10, 10, 2 }, { 20, 10, 0, 30 }, true },
   { 103, "RG32Uint",       TM_UINT,  8, { 32, 32, 0, 0 }, { 0, 32 }, true },
   { 104, "RG32Sint",       TM_SINT,  8, { 32, 32, 0, 0 }, { 0, 32 }, true },
   { 105, "RG32Float",      TM_FLOAT, 8, { 32, 32, 0, 0 }, { 0, 32 }, true },
   { 110, "RGBA16Unorm",    TM_UNORM, 8, { 16, 16, 16, 16 }, { 0, 16, 32, 48 }, true },
   { 112, "RGBA16Snorm",    TM_SNORM, 8, { 16, 16, 16, 16 }, { 0, 16, 32, 48 }, true },
   { 113, "RGBA16Uint",     TM_UINT,  8, { 16, 16, 16, 16 }, { 0, 16, 32, 48 }, true },
   { 114, "RGBA16Sint",     TM_SINT,  8, { 16, 16, 16, 16 }, { 0, 16, 32, 48 }, true },
   { 115, "RGBA16Float",    TM_FLOAT, 8, { 16, 16, 16, 16 }, { 0, 16, 32, 48 }, true },
   { 123, "RGBA32Uint",     TM_UINT, 16, { 32, 32, 32, 32 }, { 0, 32, 64, 96 }, true },
   { 124, "RGBA32Sint",     TM_SINT, 16, { 32, 32, 32, 32 }, { 0, 32, 64, 96 }, true },
   { 125, "RGBA32Float",    TM_FLOAT, 16, { 32, 32, 32, 32 }, { 0, 32, 64, 96 }, true },
};

/* The Gallium library's own four (air_metal.c), red and blue exchanged against the table above: what
 * its rows of these formats are judged by, so that they say "gallium-mirror" when they are that and
 * DIFFERS when they are something else. NULL for any other format. */
static inline const struct tm_format *
tm_gallium_mirror(unsigned mtl)
{
   static const struct tm_format mirror[4] = {
      { 40, "B5G6R5Unorm", TM_UNORM, 2, { 5, 6, 5, 0 }, { 0, 5, 11, 0 }, true },
      { 41, "A1BGR5Unorm", TM_UNORM, 2, { 5, 5, 5, 1 }, { 1, 6, 11, 0 }, true },
      { 42, "ABGR4Unorm", TM_UNORM, 2, { 4, 4, 4, 4 }, { 4, 8, 12, 0 }, true },
      { 43, "BGR5A1Unorm", TM_UNORM, 2, { 5, 5, 5, 1 }, { 0, 5, 10, 15 }, true },
   };
   return mtl >= 40 && mtl <= 43 ? &mirror[mtl - 40] : NULL;
}

/* One channel's raw value, a function of the texel and the channel only: distinct per channel and
 * per texel, and using the channel's whole width (the top bit moves with x, the low ones with y), so
 * a channel read from the wrong place or at the wrong width is a different number. */
static inline uint32_t
tm_raw(unsigned x, unsigned y, unsigned c, unsigned bits)
{
   static const uint32_t mix[4] = { 0x9d, 0x3a, 0xc5, 0x62 };
   uint32_t v = (((x & 15) << 4) | (y & 15)) ^ mix[c];   /* eight bits */
   if (bits <= 8)
      return v >> (8 - bits);
   /* the eight bits repeated down the word; never all ones, so a clamped value is seen as one */
   uint32_t w = (v << 24) | (v << 16) | (v << 8) | v;
   return bits == 32 ? w >> 1 : w >> (32 - bits);
}

/* The raw value of a float channel: a half or a float that is a small dyadic, per texel, nothing a
 * filter or an exponent could round. */
static inline float
tm_float_value(unsigned x, unsigned y, unsigned c)
{
   return (float)(x * 16 + y + 1) / 64.0f * (c == 3 ? 0.5f : (float)(c + 1));
}

static inline void
tm_put(uint8_t *to, unsigned at, unsigned bits, uint64_t v)
{
   for (unsigned i = 0; i < bits; i++) {
      unsigned b = at + i;
      to[b / 8] = (uint8_t)((to[b / 8] & ~(1u << (b % 8))) | (((v >> i) & 1) << (b % 8)));
   }
}

static inline uint64_t
tm_get(const uint8_t *from, unsigned at, unsigned bits)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < bits; i++) {
      unsigned b = at + i;
      v |= (uint64_t)((from[b / 8] >> (b % 8)) & 1) << i;
   }
   return v;
}

/* The two shared-exponent and small-float formats' decode and encode, by the book (Vulkan's "Packed
 * Formats", which is GL's and Metal's). A small float of 5 exponent bits has no sign and a mantissa
 * of 6 or 5. */
static inline float
tm_ufloat_decode(uint32_t v, unsigned mantissa)
{
   unsigned e = v >> mantissa, m = v & ((1u << mantissa) - 1);
   if (e == 0)
      return ldexpf((float)m, -14 - (int)mantissa);
   if (e == 31)
      return m ? NAN : INFINITY;
   return ldexpf(1.0f + ldexpf((float)m, -(int)mantissa), (int)e - 15);
}

static inline uint32_t
tm_ufloat_encode(float f, unsigned mantissa)
{
   if (!(f > 0.0f))
      return 0;
   int ex;
   float fr = frexpf(f, &ex);   /* f = fr * 2^ex, fr in [0.5, 1) */
   int e = ex - 1 + 15;
   if (e >= 31)
      return (uint32_t)(30u << mantissa) | ((1u << mantissa) - 1);
   if (e <= 0)
      return (uint32_t)floor(ldexp(f, 14 + (int)mantissa) + 0.5);
   uint32_t m = (uint32_t)floor(ldexp(fr * 2.0f - 1.0f, (int)mantissa) + 0.5);
   if (m >> mantissa) {
      m = 0;
      e++;
   }
   return ((uint32_t)e << mantissa) | m;
}

/* One format's texel at (x, y), as bytes, from tm_raw (or tm_float_value). */
static inline void
tm_format_texel(const struct tm_format *f, unsigned x, unsigned y, uint8_t *to)
{
   memset(to, 0, f->bytes);
   for (unsigned c = 0; c < 4; c++) {
      if (!f->bits[c])
         continue;
      uint64_t v;
      if (f->kind == TM_FLOAT) {
         float fv = tm_float_value(x, y, c);
         if (f->bits[c] == 16) {
            v = dm_half(fv);
         } else {
            uint32_t u;
            memcpy(&u, &fv, 4);
            v = u;
         }
      } else if (f->kind == TM_RG11B10) {
         v = tm_ufloat_encode(tm_float_value(x, y, c), c == 2 ? 5 : 6);
      } else {
         v = tm_raw(x, y, c, f->bits[c]);
         if (f->kind == TM_RGB9E5)
            v &= 0x1ff;
      }
      tm_put(to, f->at[c], f->bits[c], v);
   }
   if (f->kind == TM_RGB9E5)
      tm_put(to, 27, 5, (x & 7) + 12);   /* a shared exponent that varies, around 2^-3 */
}

/* What a shader reads from those bytes: Metal's and Vulkan's conversions, a channel the format has
 * not got being 0 and its alpha 1 (A8Unorm's colour 0). An integer format's value as a float: the
 * probes convert it, and these are small enough or chosen so that it survives. */
static inline void
tm_format_read(const struct tm_format *f, const uint8_t *from, float out[4])
{
   out[0] = out[1] = out[2] = 0.0f;
   out[3] = 1.0f;
   for (unsigned c = 0; c < 4; c++) {
      unsigned n = f->bits[c];
      if (!n)
         continue;
      uint64_t v = tm_get(from, f->at[c], n);
      float r = 0.0f;
      switch (f->kind) {
      case TM_UNORM:
         r = (float)((double)v / (double)((1ull << n) - 1));
         break;
      case TM_SRGB:
         r = c == 3 ? (float)v / 255.0f
             : dm_srgb_curve ? dm_srgb_curve[v] : dm_srgb_decode((float)v / 255.0f);
         break;
      case TM_SNORM: {
         int64_t s = (int64_t)(v << (64 - n)) >> (64 - n);
         double d = (double)s / (double)((1ull << (n - 1)) - 1);
         r = (float)(d < -1.0 ? -1.0 : d);
         break;
      }
      case TM_UINT:
         r = (float)v;
         break;
      case TM_SINT: {
         int64_t s = (int64_t)(v << (64 - n)) >> (64 - n);
         r = (float)s;
         break;
      }
      case TM_FLOAT:
         if (n == 16) {
            r = dm_unhalf((uint16_t)v);
         } else {
            uint32_t u = (uint32_t)v;
            memcpy(&r, &u, 4);
         }
         break;
      case TM_RG11B10:
         r = tm_ufloat_decode((uint32_t)v, c == 2 ? 5 : 6);
         break;
      case TM_RGB9E5:
         r = ldexpf((float)v, (int)tm_get(from, 27, 5) - 15 - 9);
         break;
      }
      out[c] = r;
   }
}

/* How far a sample through the texture unit may be from tm_format_read, as a value, in a float
 * attachment: nothing to speak of for a whole-number conversion, the unorm's own rounding else. */
static inline float
tm_format_slop(const struct tm_format *f)
{
   if (f->kind == TM_UINT || f->kind == TM_SINT)
      return f->bits[0] > 16 ? 1024.0f : 1e-3f;   /* 32 bits through a float */
   if (f->kind == TM_FLOAT || f->kind == TM_RG11B10 || f->kind == TM_RGB9E5)
      return 2e-3f;
   return 1.0f / 512.0f;
}

/* What a render of `want` (a float attachment's value, in the channel's own range) leaves in a
 * format: bytes. For a channel it has not got: nothing. */
static inline void
tm_format_write(const struct tm_format *f, const float want[4], uint8_t *to)
{
   memset(to, 0, f->bytes);
   for (unsigned c = 0; c < 4; c++) {
      unsigned n = f->bits[c];
      if (!n)
         continue;
      uint64_t v = 0;
      switch (f->kind) {
      case TM_UNORM:
         v = (uint64_t)floor((double)dm_clamp01(want[c]) * (double)((1ull << n) - 1) + 0.5);
         break;
      case TM_SRGB:
         v = (uint64_t)floor((double)(c == 3 ? dm_clamp01(want[c])
                                             : dm_srgb_encode(dm_clamp01(want[c]))) * 255.0 + 0.5);
         break;
      case TM_SNORM: {
         double d = want[c] < -1.0f ? -1.0 : want[c] > 1.0f ? 1.0 : (double)want[c];
         v = (uint64_t)(int64_t)floor(d * (double)((1ull << (n - 1)) - 1) + 0.5);
         break;
      }
      case TM_UINT:
      case TM_SINT:
         v = (uint64_t)(int64_t)want[c];
         break;
      case TM_FLOAT:
         if (n == 16) {
            v = dm_half(dm_round_half(want[c]));
         } else {
            uint32_t u;
            memcpy(&u, &want[c], 4);
            v = u;
         }
         break;
      case TM_RG11B10:
         v = tm_ufloat_encode(want[c], c == 2 ? 5 : 6);
         break;
      case TM_RGB9E5:
         break;
      }
      tm_put(to, f->at[c], n, v);
   }
}

/* Whether two texels of a format are within `steps` raw steps per channel (a float channel by the
 * value, its own step): what a render into it is judged by. */
static inline bool
tm_format_close(const struct tm_format *f, const uint8_t *a, const uint8_t *b, unsigned steps)
{
   for (unsigned c = 0; c < 4; c++) {
      unsigned n = f->bits[c];
      if (!n)
         continue;
      uint64_t x = tm_get(a, f->at[c], n), y = tm_get(b, f->at[c], n);
      uint64_t d = x > y ? x - y : y - x;
      if (d > steps)
         return false;
   }
   return true;
}

/* ---- bytes against bytes -------------------------------------------------------------------- */

/*
 * One result line over raw memory, in draw_model's family: what a read-back of a level or a slice
 * is judged by, which is not 16x16 of a format dm_verdict knows. `count` texels of `texel` bytes,
 * w to a row for the sentence. Exact: these are bytes that went in and must come out.
 */
static inline bool
tm_bytes(const char *backend, const char *name, const void *got, const void *want, unsigned count,
         unsigned texel, unsigned w, bool lacks)
{
   const uint8_t *g = got, *e = want;
   unsigned wrong = 0, first = 0;
   for (unsigned i = 0; i < count; i++)
      if (memcmp(g + (size_t)i * texel, e + (size_t)i * texel, texel)) {
         if (!wrong)
            first = i;
         wrong++;
      }
   uint64_t seed = 0xcbf29ce484222325ull;
   uint64_t dg = dm_fnv(g, (size_t)count * texel, seed), dw = dm_fnv(e, (size_t)count * texel, seed);
   printf("drvtest: draw-result %s %s %s digest %016llx expected %016llx\n", backend, name,
          !wrong ? "ok" : lacks ? "gallium-lacks" : "DIFFERS", (unsigned long long)dg,
          (unsigned long long)dw);
   if (wrong && !lacks) {
      printf("drvtest: draw-diff %s %s %u of %u texels differ, first (%u,%u) got", backend, name,
             wrong, count, first % w, first / w);
      for (unsigned b = 0; b < texel && b < 16; b++)
         printf(" %02x", g[(size_t)first * texel + b]);
      printf(" expected");
      for (unsigned b = 0; b < texel && b < 16; b++)
         printf(" %02x", e[(size_t)first * texel + b]);
      printf("\n");
   }
   fflush(stdout);
   return !wrong;
}

/*
 * A level of RGBA8 against the box filter of the one above it, as that one really reads back. So
 * no error compounds from level to level, and each is a statement about one blit.
 *
 * WHY A BOX, AND HOW CLOSE: a halving of a power-of-two level through a linear filter samples every
 * destination texel at the corner of four source texels, each weighted 1/4, on both turnip's blit
 * (the 2D engine's bilinear in tu6_blit_image, or r3d's txl with VK_FILTER_LINEAR) and Gallium's
 * (util_blitter's linear sample); the average of four bytes is a multiple of 1/4 of a step, and
 * what is left is how the filter rounds that to a byte. One step each way is every rounding there
 * is, and no more: a filter that fetched a neighbouring texel would be many steps out on the
 * gradient these are fed. An sRGB format filters in linear and encodes after, which is the
 * reason it is its own case, and a decode and an encode each add a step at most: two, there.
 *
 * Returns how many channel values were out, and `max` the largest difference, in steps.
 */
static inline unsigned
tm_box_level(const uint8_t *above, unsigned aw, unsigned ah, const uint8_t *level, unsigned w,
             unsigned h, bool srgb, unsigned steps, int *max)
{
   unsigned wrong = 0;
   *max = 0;
   for (unsigned y = 0; y < h; y++) {
      for (unsigned x = 0; x < w; x++) {
         for (unsigned c = 0; c < 4; c++) {
            double sum = 0.0;
            unsigned taps = 0;
            for (unsigned dy = 0; dy < 2; dy++) {
               for (unsigned dx = 0; dx < 2; dx++) {
                  unsigned sx = x * aw / w + (aw > w ? dx : 0);
                  unsigned sy = y * ah / h + (ah > h ? dy : 0);
                  uint8_t v = above[((size_t)sy * aw + sx) * 4 + c];
                  sum += srgb && c < 3 ? dm_srgb_decode((float)v / 255.0f) : (float)v / 255.0f;
                  taps++;
               }
            }
            double m = sum / taps;
            if (srgb && c < 3)
               m = dm_srgb_encode((float)m);
            int d = (int)level[((size_t)y * w + x) * 4 + c] - (int)floor(m * 255.0 + 0.5);
            d = d < 0 ? -d : d;
            *max = d > *max ? d : *max;
            if (d > (int)steps)
               wrong++;
         }
      }
   }
   return wrong;
}

#endif /* TEX_MODEL_H */
