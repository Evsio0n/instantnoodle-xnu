/*
 * M5c's rows, through Metal's public API and nothing else, so the same code runs against real Metal
 * (tools/air/mtlref, --m5c LIB) and against this port (metal_drv_test --m5c LIB): argument buffers,
 * arrays of descriptors, a fragment or vertex shader's stores, function constants. The shaders are
 * tools/air/m5c.metal, compiled by this Mac's Metal compiler. The models are written here, and a row
 * that real Metal does not agree with is the model's to answer for before the phone's is judged.
 *
 * Every line is draw_model.h's: `drvtest: draw-result BACKEND NAME ok|DIFFERS|gallium-lacks ...`,
 * `draw-diff`, or `draw-result BACKEND NAME refused: why`. No retain or release: under ARC (mtlref)
 * that is the compiler's, and on the phone a process that lives for this one run may leak.
 *
 * Needs draw_model.h and tex_model.h first.
 */

struct m5c {
   id<MTLDevice> dev;
   id<MTLCommandQueue> queue;
   id<MTLLibrary> lib;
   const char *backend;
   bool gallium;                 /* it has no views, no arrays and no function constants */
   id<MTLTexture> target;
   id<MTLTexture> t[3];          /* 16 x 16 RGBA8, tm_fill levels 0, 1 and 2 */
   id<MTLSamplerState> near, rep;
   id<MTLFunction> vs;
   unsigned right, wrong, lacks, refused;
};

typedef void (^m5c_rbind)(id<MTLRenderCommandEncoder>);
typedef void (^m5c_cbind)(id<MTLComputeCommandEncoder>);

static void
m5c_said(struct m5c *m, const char *name, const char *why)
{
   dm_refused(m->backend, name, why);
   m->refused++;
}

static void
m5c_nserror(struct m5c *m, const char *name, const char *what, NSError *e)
{
   char why[512];
   snprintf(why, sizeof(why), "%s: %s", what, e ? [[e localizedDescription] UTF8String] : "(no reason)");
   m5c_said(m, name, why);
}

static id<MTLFunction>
m5c_fn(struct m5c *m, const char *fn, const char *row)
{
   id<MTLFunction> f = [m->lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
   if (f == nil)
      m5c_said(m, row, "the library has no such function");
   return f;
}

static id<MTLRenderPipelineState>
m5c_rp(struct m5c *m, id<MTLFunction> vf, id<MTLFunction> ff, MTLPixelFormat depth, const char *row)
{
   if (vf == nil || ff == nil)
      return nil;
   MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
   NSError *e = nil;
   [d setVertexFunction:vf];
   [d setFragmentFunction:ff];
   [[d colorAttachments][0] setPixelFormat:MTLPixelFormatRGBA32Float];
   [d setDepthAttachmentPixelFormat:depth];
   id<MTLRenderPipelineState> ps = [m->dev newRenderPipelineStateWithDescriptor:d error:&e];
   if (ps == nil)
      m5c_nserror(m, row, "no pipeline", e);
   return ps;
}

static id<MTLComputePipelineState>
m5c_cp(struct m5c *m, id<MTLFunction> f, const char *row)
{
   if (f == nil)
      return nil;
   NSError *e = nil;
   id<MTLComputePipelineState> ps = [m->dev newComputePipelineStateWithFunction:f error:&e];
   if (ps == nil)
      m5c_nserror(m, row, "no pipeline", e);
   return ps;
}

static id<MTLBuffer>
m5c_buffer(struct m5c *m, const void *bytes, size_t size)
{
   id<MTLBuffer> b = [m->dev newBufferWithLength:size options:MTLResourceStorageModeShared];
   if (bytes)
      memcpy([b contents], bytes, size);
   else
      memset([b contents], 0, size);
   return b;
}

static id<MTLTexture>
m5c_texture(struct m5c *m, MTLPixelFormat fmt, unsigned w, unsigned h, unsigned levels, MTLTextureUsage usage)
{
   MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h
                                                                            mipmapped:NO];
   [d setMipmapLevelCount:levels];
   [d setUsage:usage];
   [d setStorageMode:MTLStorageModeShared];
   return [m->dev newTextureWithDescriptor:d];
}

static bool
m5c_finish(struct m5c *m, id<MTLCommandBuffer> cb, const char *row)
{
   [cb commit];
   [cb waitUntilCompleted];
   if ([cb status] != MTLCommandBufferStatusCompleted) {
      m5c_nserror(m, row, "the command buffer", [cb error]);
      return false;
   }
   return true;
}

/* One pass into `target`, cleared to 0, whole attachment, into a command buffer or a new one. */
static void
m5c_pass(struct m5c *m, id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> ps,
         unsigned vertices, unsigned instances, m5c_rbind bind)
{
   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
   MTLRenderPassColorAttachmentDescriptor *c = [rp colorAttachments][0];
   [c setTexture:target];
   [c setLoadAction:MTLLoadActionClear];
   [c setStoreAction:MTLStoreActionStore];
   [c setClearColor:MTLClearColorMake(0, 0, 0, 0)];
   id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
   [enc setRenderPipelineState:ps];
   if (bind)
      bind(enc);
   [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:vertices
         instanceCount:instances];
   [enc endEncoding];
}

static bool
m5c_draw(struct m5c *m, id<MTLRenderPipelineState> ps, m5c_rbind bind, const char *row)
{
   if (ps == nil)
      return false;
   id<MTLCommandBuffer> cb = [m->queue commandBuffer];
   m5c_pass(m, cb, m->target, ps, 4, 1, bind);
   return m5c_finish(m, cb, row);
}

static bool
m5c_dispatch(struct m5c *m, id<MTLComputePipelineState> ps, unsigned w, unsigned h, m5c_cbind bind,
             const char *row)
{
   if (ps == nil)
      return false;
   id<MTLCommandBuffer> cb = [m->queue commandBuffer];
   id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
   [enc setComputePipelineState:ps];
   if (bind)
      bind(enc);
   [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
   [enc endEncoding];
   return m5c_finish(m, cb, row);
}

/* Judged: bytes of DM_W x DM_H float4s, the attachment's or a buffer's. */
static void
m5c_judge_bytes(struct m5c *m, const void *got, const struct dm_picture *want, const char *row, bool lacks)
{
   bool ok = dm_verdict(m->backend, row, DM_RGBA32F, got, want, 1.0f / 2048.0f, lacks);
   if (ok)
      m->right++;
   else if (lacks)
      m->lacks++;
   else
      m->wrong++;
}

static void
m5c_judge(struct m5c *m, id<MTLTexture> t, const struct dm_picture *want, const char *row, bool lacks)
{
   static float mem[DM_W * DM_H * 4];
   [t getBytes:mem bytesPerRow:DM_W * 16 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H) mipmapLevel:0];
   m5c_judge_bytes(m, mem, want, row, lacks);
}

static struct dm_picture m5c_want;

/* a texture t[k]'s picture, sampled at (x + 0.5) / 16, plus a colour */
static void
m5c_want_texels(unsigned k, const float add[4])
{
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         m5c_want.care[y][x] = 1;
         tm_texelf(k, 0, x, y, m5c_want.px[y][x]);
         for (unsigned c = 0; add && c < 4; c++)
            m5c_want.px[y][x][c] += add[c];
      }
}

static void
m5c_want_solid(float r, float g, float b, float a)
{
   dm_clear(&m5c_want, (const float[4]){ r, g, b, a });
}

/* ---- argument buffers ------------------------------------------------------------------- */

static void
m5c_argbufs(struct m5c *m)
{
   const float bv[2][4] = { { 9, 9, 9, 9 }, { 0.25f, 0.125f, 0, 0 } };
   id<MTLBuffer> b = m5c_buffer(m, bv, sizeof(bv));
   const float add[4] = { 0.25f, 0.125f, 0, 0 };
   id<MTLFunction> fa1 = m5c_fn(m, "m5c_fa1", "argbuf-fragment");
   id<MTLRenderPipelineState> ps = m5c_rp(m, m->vs, fa1, MTLPixelFormatInvalid, "argbuf-fragment");
   if (ps) {
      id<MTLArgumentEncoder> enc = [fa1 newArgumentEncoderWithBufferIndex:0];
      printf("m5c: %s encodedLength of A1 (3 members) %lu alignment %lu\n", m->backend,
             (unsigned long)[enc encodedLength], (unsigned long)[enc alignment]);
      const NSUInteger step = ([enc encodedLength] + [enc alignment] - 1) / [enc alignment] * [enc alignment];
      id<MTLBuffer> set = m5c_buffer(m, NULL, step * 4);
      id<MTLTexture> t0 = m->t[0], t1 = m->t[1], t2 = m->t[2];
      id<MTLSamplerState> near = m->near;
      [enc setArgumentBuffer:set offset:0];
      [enc setTexture:t0 atIndex:0];
      [enc setSamplerState:near atIndex:1];
      [enc setBuffer:b offset:0 atIndex:2];
      if (m5c_draw(m, ps, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentBuffer:set offset:0 atIndex:0];
             [e useResource:t0 usage:MTLResourceUsageRead];
             [e useResource:b usage:MTLResourceUsageRead];
          }, "argbuf-fragment")) {
         m5c_want_texels(0, add);
         m5c_judge(m, m->target, &m5c_want, "argbuf-fragment", false);
      }
      /* a second encoding in one buffer, reached by the offset of the bind */
      [enc setArgumentBuffer:set offset:step * 2];
      [enc setTexture:t1 atIndex:0];
      [enc setSamplerState:near atIndex:1];
      [enc setBuffer:b offset:0 atIndex:2];
      if (m5c_draw(m, ps, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentBuffer:set offset:step * 2 atIndex:0];
             [e useResource:t1 usage:MTLResourceUsageRead];
             [e useResource:b usage:MTLResourceUsageRead];
          }, "argbuf-offset")) {
         m5c_want_texels(1, add);
         m5c_judge(m, m->target, &m5c_want, "argbuf-offset", false);
      }
      /* Metal's own offset: on a buffer member, at a binding offset and at one no GPU binds at */
      static const struct { const char *name; size_t at; } offs[] = {
         { "argbuf-buffer-offset-256", 256 }, { "argbuf-buffer-offset-20", 20 },
      };
      for (unsigned i = 0; i < 2; i++) {
         id<MTLBuffer> big = m5c_buffer(m, NULL, 1024);
         memcpy((char *)[big contents] + offs[i].at, bv, sizeof(bv));
         [enc setArgumentBuffer:set offset:step];
         [enc setTexture:t0 atIndex:0];
         [enc setSamplerState:near atIndex:1];
         [enc setBuffer:big offset:offs[i].at atIndex:2];
         if (m5c_draw(m, ps, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentBuffer:set offset:step atIndex:0];
                [e useResource:t0 usage:MTLResourceUsageRead];
                [e useResource:big usage:MTLResourceUsageRead];
             }, offs[i].name)) {
            m5c_want_texels(0, add);
            m5c_judge(m, m->target, &m5c_want, offs[i].name, false);
         }
      }
      /* encoded again after a draw that finished: the next draw reads what is there now */
      [enc setArgumentBuffer:set offset:step * 3];
      [enc setTexture:t0 atIndex:0];
      [enc setSamplerState:near atIndex:1];
      [enc setBuffer:b offset:0 atIndex:2];
      m5c_rbind at3 = ^(id<MTLRenderCommandEncoder> e) {
         [e setFragmentBuffer:set offset:step * 3 atIndex:0];
         [e useResource:t0 usage:MTLResourceUsageRead];
         [e useResource:t2 usage:MTLResourceUsageRead];
         [e useResource:b usage:MTLResourceUsageRead];
      };
      if (m5c_draw(m, ps, at3, "argbuf-reencode")) {
         [enc setTexture:t2 atIndex:0];
         if (m5c_draw(m, ps, at3, "argbuf-reencode")) {
            m5c_want_texels(2, add);
            m5c_judge(m, m->target, &m5c_want, "argbuf-reencode", false);
         }
      }
      /* ... and between two passes of ONE command buffer that has not run: real Metal's GPU reads
       * the handles when it runs, so both passes read what was encoded last */
      {
         id<MTLTexture> other = m5c_texture(m, MTLPixelFormatRGBA32Float, DM_W, DM_H, 1,
                                            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
         id<MTLCommandBuffer> cb = [m->queue commandBuffer];
         [enc setTexture:t0 atIndex:0];
         m5c_pass(m, cb, other, ps, 4, 1, at3);
         [enc setTexture:t2 atIndex:0];
         m5c_pass(m, cb, m->target, ps, 4, 1, at3);
         if (m5c_finish(m, cb, "argbuf-reencode-one-cb")) {
            m5c_want_texels(2, add);
            m5c_judge(m, other, &m5c_want, "argbuf-reencode-one-cb", false);
         }
      }
   }
   /* two sets and two direct arguments */
   {
      id<MTLFunction> f = m5c_fn(m, "m5c_fa2", "argbuf-two-sets");
      id<MTLRenderPipelineState> p2 = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "argbuf-two-sets");
      if (p2) {
         id<MTLArgumentEncoder> ex = [f newArgumentEncoderWithBufferIndex:1];
         id<MTLArgumentEncoder> ey = [f newArgumentEncoderWithBufferIndex:2];
         id<MTLBuffer> sx = m5c_buffer(m, NULL, [ex encodedLength]);
         id<MTLBuffer> sy = m5c_buffer(m, NULL, [ey encodedLength]);
         const float uv[4] = { 0, 0, 0, 0.5f }, yv[4] = { 0.25f, 0, 0.125f, 0 };
         id<MTLBuffer> ub = m5c_buffer(m, uv, sizeof(uv)), yb = m5c_buffer(m, yv, sizeof(yv));
         id<MTLTexture> t1 = m->t[1], t2 = m->t[2];
         [ex setArgumentBuffer:sx offset:0];
         [ex setTexture:t1 atIndex:0];
         [ex setSamplerState:m->near atIndex:1];
         [ey setArgumentBuffer:sy offset:0];
         [ey setBuffer:yb offset:0 atIndex:0];
         if (m5c_draw(m, p2, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentBuffer:ub offset:0 atIndex:0];
                [e setFragmentBuffer:sx offset:0 atIndex:1];
                [e setFragmentBuffer:sy offset:0 atIndex:2];
                [e setFragmentTexture:t2 atIndex:0];
                [e useResource:t1 usage:MTLResourceUsageRead];
                [e useResource:yb usage:MTLResourceUsageRead];
             }, "argbuf-two-sets")) {
            for (unsigned y = 0; y < DM_H; y++)
               for (unsigned x = 0; x < DM_W; x++) {
                  float a[4], c[4];
                  tm_texelf(1, 0, x, y, a);
                  tm_texelf(2, 0, x, y, c);
                  m5c_want.care[y][x] = 1;
                  for (unsigned k = 0; k < 4; k++)
                     m5c_want.px[y][x][k] = a[k] + yv[k] + uv[k] + c[k] * 0.5f;
               }
            m5c_judge(m, m->target, &m5c_want, "argbuf-two-sets", false);
         }
      }
   }
   /* a member nothing reads, left out of the encoding (Gallium refuses it: every member it binds) */
   {
      id<MTLFunction> f = m5c_fn(m, "m5c_fa4", "argbuf-unused-member");
      id<MTLRenderPipelineState> p4 = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "argbuf-unused-member");
      if (p4) {
         id<MTLArgumentEncoder> enc = [f newArgumentEncoderWithBufferIndex:0];
         id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
         [enc setArgumentBuffer:set offset:0];
         [enc setBuffer:b offset:0 atIndex:1];
         if (m5c_draw(m, p4, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentBuffer:set offset:0 atIndex:0];
                [e useResource:b usage:MTLResourceUsageRead];
             }, "argbuf-unused-member")) {
            m5c_want_solid(9, 9, 9, 9);
            m5c_judge(m, m->target, &m5c_want, "argbuf-unused-member", false);
         }
      }
   }
   /* the vertex stage's own */
   {
      id<MTLFunction> v = m5c_fn(m, "m5c_va", "argbuf-vertex");
      id<MTLFunction> f = m5c_fn(m, "m5c_fsolid", "argbuf-vertex");
      id<MTLRenderPipelineState> pv = m5c_rp(m, v, f, MTLPixelFormatInvalid, "argbuf-vertex");
      if (pv) {
         id<MTLArgumentEncoder> enc = [v newArgumentEncoderWithBufferIndex:0];
         id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
         const float pos[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } }, scale = 0.5f;
         id<MTLBuffer> pb = m5c_buffer(m, pos, sizeof(pos)), sb = m5c_buffer(m, &scale, sizeof(scale));
         [enc setArgumentBuffer:set offset:0];
         [enc setBuffer:pb offset:0 atIndex:0];
         [enc setBuffer:sb offset:0 atIndex:1];
         if (m5c_draw(m, pv, ^(id<MTLRenderCommandEncoder> e) {
                [e setVertexBuffer:set offset:0 atIndex:0];
                [e useResource:pb usage:MTLResourceUsageRead];
                [e useResource:sb usage:MTLResourceUsageRead];
             }, "argbuf-vertex")) {
            m5c_want_solid(0, 0, 0, 0);
            for (unsigned y = 4; y < 12; y++)
               for (unsigned x = 4; x < 12; x++) {
                  const float c[4] = { 0.5f, 0.25f, 0.75f, 1 };
                  memcpy(m5c_want.px[y][x], c, sizeof(c));
               }
            m5c_judge(m, m->target, &m5c_want, "argbuf-vertex", false);
         }
      }
   }
   /* a kernel's: a buffer it writes, a buffer it reads and a texture */
   {
      id<MTLFunction> f = m5c_fn(m, "m5c_ka", "argbuf-kernel");
      id<MTLComputePipelineState> pk = m5c_cp(m, f, "argbuf-kernel");
      if (pk) {
         id<MTLArgumentEncoder> enc = [f newArgumentEncoderWithBufferIndex:0];
         id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
         static float in[DM_W * DM_H * 4];
         for (unsigned i = 0; i < DM_W * DM_H * 4; i++)
            in[i] = (float)(i % 7) * 0.25f;
         id<MTLBuffer> ib = m5c_buffer(m, in, sizeof(in)), ob = m5c_buffer(m, NULL, sizeof(in));
         id<MTLTexture> t0 = m->t[0];
         [enc setArgumentBuffer:set offset:0];
         [enc setBuffer:ob offset:0 atIndex:0];
         [enc setBuffer:ib offset:0 atIndex:1];
         [enc setTexture:t0 atIndex:2];
         if (m5c_dispatch(m, pk, DM_W, DM_H, ^(id<MTLComputeCommandEncoder> e) {
                [e setBuffer:set offset:0 atIndex:0];
                [e useResource:ob usage:MTLResourceUsageWrite];
                [e useResource:ib usage:MTLResourceUsageRead];
                [e useResource:t0 usage:MTLResourceUsageRead];
             }, "argbuf-kernel")) {
            m5c_want_texels(0, NULL);
            for (unsigned y = 0; y < DM_H; y++)
               for (unsigned x = 0; x < DM_W; x++)
                  for (unsigned c = 0; c < 4; c++)
                     m5c_want.px[y][x][c] += in[(y * DM_W + x) * 4 + c] * 2;
            m5c_judge_bytes(m, [ob contents], &m5c_want, "argbuf-kernel", false);
         }
      }
   }
   /* more than a push set holds */
   {
      id<MTLFunction> f = m5c_fn(m, "m5c_fam", "argbuf-34-members");
      id<MTLRenderPipelineState> pm = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "argbuf-34-members");
      if (pm) {
         id<MTLArgumentEncoder> enc = [f newArgumentEncoderWithBufferIndex:0];
         id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
         id<MTLBuffer> each[34];
         float sum[4] = { 0, 0, 0, 0 };
         [enc setArgumentBuffer:set offset:0];
         for (unsigned i = 0; i < 34; i++) {
            const float v[4] = { (float)(i + 1), (float)i * 0.5f, 1, (float)(i & 1) };
            each[i] = m5c_buffer(m, v, sizeof(v));
            [enc setBuffer:each[i] offset:0 atIndex:i];
            for (unsigned c = 0; c < 4; c++)
               sum[c] += v[c];
         }
         id<MTLBuffer> __strong *all = each;
         if (m5c_draw(m, pm, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentBuffer:set offset:0 atIndex:0];
                for (unsigned i = 0; i < 34; i++)
                   [e useResource:all[i] usage:MTLResourceUsageRead];
             }, "argbuf-34-members")) {
            m5c_want_solid(sum[0], sum[1], sum[2], sum[3]);
            m5c_judge(m, m->target, &m5c_want, "argbuf-34-members", false);
         }
      }
   }
}

/* ---- arrays of descriptors -------------------------------------------------------------- */

struct m5c_u { unsigned i, j; };

static void
m5c_arrays(struct m5c *m)
{
   char name[64];
   id<MTLTexture> t0 = m->t[0], t1 = m->t[1], t2 = m->t[2];
   id<MTLTexture> __unsafe_unretained ts_[3] = { t0, t1, t2 };
   id<MTLTexture> __unsafe_unretained *ts = ts_;   /* a block cannot name an array */
   id<MTLSamplerState> near = m->near, rep = m->rep;
   id<MTLFunction> f;
   id<MTLRenderPipelineState> p;

   /* direct, an element named in the source */
   f = m5c_fn(m, "m5c_fix_const", "index-direct-const");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-direct-const");
   if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
          [e setFragmentTextures:ts withRange:NSMakeRange(0, 3)];
          [e setFragmentSamplerState:near atIndex:0];
       }, "index-direct-const")) {
      m5c_want_texels(1, NULL);
      m5c_judge(m, m->target, &m5c_want, "index-direct-const", false);
   }
   /* direct, chosen at run time: sampled, read, and each one's size */
   static const struct { const char *fn, *row; } kinds[2] = {
      { "m5c_fix_run", "index-direct-run" }, { "m5c_fix_read", "index-direct-read" },
   };
   for (unsigned k = 0; k < 2; k++) {
      f = m5c_fn(m, kinds[k].fn, kinds[k].row);
      p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, kinds[k].row);
      for (unsigned i = 0; p && i < 3; i++) {
         const struct m5c_u u = { i, 0 };
         id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
         snprintf(name, sizeof(name), "%s-%u", kinds[k].row, i);
         if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentTextures:ts withRange:NSMakeRange(0, 3)];
                if (k == 0)
                   [e setFragmentSamplerState:near atIndex:0];
                [e setFragmentBuffer:ub offset:0 atIndex:0];
             }, name)) {
            m5c_want_texels(i, NULL);
            m5c_judge(m, m->target, &m5c_want, name, false);
         }
      }
   }
   {
      id<MTLTexture> a = m5c_texture(m, MTLPixelFormatRGBA8Unorm, 16, 16, 1, MTLTextureUsageShaderRead);
      id<MTLTexture> b = m5c_texture(m, MTLPixelFormatRGBA8Unorm, 32, 8, 2, MTLTextureUsageShaderRead);
      id<MTLTexture> c = m5c_texture(m, MTLPixelFormatRGBA8Unorm, 8, 4, 1, MTLTextureUsageShaderRead);
      id<MTLTexture> __unsafe_unretained sized_[3] = { a, b, c };
      id<MTLTexture> __unsafe_unretained *sized = sized_;
      static const float want[3][3] = { { 16, 16, 1 }, { 32, 8, 2 }, { 8, 4, 1 } };
      f = m5c_fn(m, "m5c_fix_size", "index-direct-size");
      p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-direct-size");
      for (unsigned i = 0; p && i < 3; i++) {
         const struct m5c_u u = { i, 0 };
         id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
         snprintf(name, sizeof(name), "index-direct-size-%u", i);
         if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentTextures:sized withRange:NSMakeRange(0, 3)];
                [e setFragmentBuffer:ub offset:0 atIndex:0];
             }, name)) {
            m5c_want_solid(want[i][0], want[i][1], want[i][2], 1);
            m5c_judge(m, m->target, &m5c_want, name, false);
         }
      }
   }
   /* an element nothing was bound to, which the shader does not reach */
   f = m5c_fn(m, "m5c_fix_run", "index-direct-hole");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-direct-hole");
   {
      const struct m5c_u u = { 2, 0 };
      id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentTexture:t2 atIndex:2];
             [e setFragmentSamplerState:near atIndex:0];
             [e setFragmentBuffer:ub offset:0 atIndex:0];
          }, "index-direct-hole")) {
         m5c_want_texels(2, NULL);
         m5c_judge(m, m->target, &m5c_want, "index-direct-hole", false);
      }
   }
   /* direct samplers: clamped and repeating, past the edges */
   f = m5c_fn(m, "m5c_fix_sampler", "index-direct-sampler");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-direct-sampler");
   for (unsigned i = 0; p && i < 2; i++) {
      const struct m5c_u u = { i, 0 };
      id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
      id<MTLSamplerState> __unsafe_unretained ss_[2] = { near, rep };
      id<MTLSamplerState> __unsafe_unretained *ss = ss_;
      snprintf(name, sizeof(name), "index-direct-sampler-%u", i);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentTexture:t0 atIndex:0];
             [e setFragmentSamplerStates:ss withRange:NSMakeRange(0, 2)];
             [e setFragmentBuffer:ub offset:0 atIndex:0];
          }, name)) {
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               const unsigned mode = i ? TM_REPEAT : TM_EDGE;
               int a = tm_wrap(mode, ((float)x + 0.5f) / 8 - 0.5f, 16);
               int b = tm_wrap(mode, ((float)y + 0.5f) / 8 - 0.5f, 16);
               m5c_want.care[y][x] = 1;
               tm_texelf(0, 0, (unsigned)a, (unsigned)b, m5c_want.px[y][x]);
            }
         m5c_judge(m, m->target, &m5c_want, name, false);
      }
   }
   /* a different element per pixel */
   f = m5c_fn(m, "m5c_fix_pixel", "index-direct-per-pixel");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-direct-per-pixel");
   if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
          [e setFragmentTextures:ts withRange:NSMakeRange(0, 3)];
          [e setFragmentSamplerState:near atIndex:0];
       }, "index-direct-per-pixel")) {
      for (unsigned y = 0; y < DM_H; y++)
         for (unsigned x = 0; x < DM_W; x++) {
            m5c_want.care[y][x] = 1;
            tm_texelf(x % 3, 0, x, y, m5c_want.px[y][x]);
         }
      m5c_judge(m, m->target, &m5c_want, "index-direct-per-pixel", false);
   }

   /* in an argument buffer: textures, samplers and buffers */
   const float bv[3][4] = { { 1, 0, 0, 1 }, { 0, 2, 0, 1 }, { 0, 0, 3, 1 } };
   id<MTLBuffer> bb[3] = { m5c_buffer(m, bv[0], 16), m5c_buffer(m, bv[1], 16), m5c_buffer(m, bv[2], 16) };
   id<MTLBuffer> b0 = bb[0], b1 = bb[1], b2 = bb[2];
   id<MTLFunction> ftex = m5c_fn(m, "m5c_fib_tex", "index-argbuf");
   if (ftex == nil)
      return;
   id<MTLArgumentEncoder> enc = [ftex newArgumentEncoderWithBufferIndex:1];
   if (enc == nil) {
      m5c_said(m, "index-argbuf", "no encoder for buffer index 1 of m5c_fib_tex");
      return;
   }
   printf("m5c: %s encodedLength of AI (3 textures, 2 samplers, 3 buffers) %lu\n", m->backend,
          (unsigned long)[enc encodedLength]);
   id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
   id<MTLSamplerState> __unsafe_unretained ss[2] = { near, rep };
   id<MTLBuffer> __unsafe_unretained bs[3] = { b0, b1, b2 };
   const NSUInteger none[3] = { 0, 0, 0 };
   [enc setArgumentBuffer:set offset:0];
   [enc setTextures:ts withRange:NSMakeRange(0, 3)];
   [enc setSamplerStates:ss withRange:NSMakeRange(3, 2)];
   [enc setBuffers:bs offsets:none withRange:NSMakeRange(5, 3)];
   m5c_rbind uses = ^(id<MTLRenderCommandEncoder> e) {
      [e setFragmentBuffer:set offset:0 atIndex:1];
      [e useResource:t0 usage:MTLResourceUsageRead];
      [e useResource:t1 usage:MTLResourceUsageRead];
      [e useResource:t2 usage:MTLResourceUsageRead];
      [e useResource:b0 usage:MTLResourceUsageRead];
      [e useResource:b1 usage:MTLResourceUsageRead];
      [e useResource:b2 usage:MTLResourceUsageRead];
   };
   p = m5c_rp(m, m->vs, ftex, MTLPixelFormatInvalid, "index-argbuf-tex");
   for (unsigned i = 0; p && i < 3; i++) {
      const struct m5c_u u = { i, 0 };
      id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
      snprintf(name, sizeof(name), "index-argbuf-tex-%u", i);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             uses(e);
             [e setFragmentBuffer:ub offset:0 atIndex:0];
          }, name)) {
         m5c_want_texels(i, NULL);
         m5c_judge(m, m->target, &m5c_want, name, false);
      }
   }
   f = m5c_fn(m, "m5c_fib_samp", "index-argbuf-samp");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-argbuf-samp");
   for (unsigned i = 0; p && i < 2; i++) {
      const struct m5c_u u = { i, 0 };
      id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
      snprintf(name, sizeof(name), "index-argbuf-samp-%u", i);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             uses(e);
             [e setFragmentBuffer:ub offset:0 atIndex:0];
          }, name)) {
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               const unsigned mode = i ? TM_REPEAT : TM_EDGE;
               int a = tm_wrap(mode, ((float)x + 0.5f) / 8 - 0.5f, 16);
               int b = tm_wrap(mode, ((float)y + 0.5f) / 8 - 0.5f, 16);
               m5c_want.care[y][x] = 1;
               tm_texelf(0, 0, (unsigned)a, (unsigned)b, m5c_want.px[y][x]);
            }
         m5c_judge(m, m->target, &m5c_want, name, false);
      }
   }
   f = m5c_fn(m, "m5c_fib_buf", "index-argbuf-buf");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-argbuf-buf");
   for (unsigned i = 0; p && i < 3; i++) {
      const struct m5c_u u = { i, 0 };
      id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
      snprintf(name, sizeof(name), "index-argbuf-buf-%u", i);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             uses(e);
             [e setFragmentBuffer:ub offset:0 atIndex:0];
          }, name)) {
         m5c_want_solid(bv[i][0], bv[i][1], bv[i][2], bv[i][3]);
         m5c_judge(m, m->target, &m5c_want, name, false);
      }
   }
   f = m5c_fn(m, "m5c_fib_const", "index-argbuf-const");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "index-argbuf-const");
   if (m5c_draw(m, p, uses, "index-argbuf-const")) {
      m5c_want_texels(2, bv[2]);
      m5c_judge(m, m->target, &m5c_want, "index-argbuf-const", false);
   }
   /* stores through an element chosen at run time, from a fragment shader and a kernel */
   {
      id<MTLFunction> fs = m5c_fn(m, "m5c_fib_store", "index-argbuf-store");
      id<MTLFunction> kf = m5c_fn(m, "m5c_kib", "index-kernel-store");
      id<MTLRenderPipelineState> ps = m5c_rp(m, m->vs, fs, MTLPixelFormatInvalid, "index-argbuf-store");
      id<MTLComputePipelineState> pk = m5c_cp(m, kf, "index-kernel-store");
      for (unsigned i = 0; i < 2; i++) {
         const struct m5c_u u = { i, 0 };
         id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
         id<MTLBuffer> w0 = m5c_buffer(m, NULL, DM_W * DM_H * 16), w1 = m5c_buffer(m, NULL, DM_W * DM_H * 16);
         id<MTLBuffer> __unsafe_unretained ws[2] = { w0, w1 };
         const NSUInteger z[2] = { 0, 0 };
         if (ps) {
            id<MTLArgumentEncoder> we = [fs newArgumentEncoderWithBufferIndex:1];
            id<MTLBuffer> wset = m5c_buffer(m, NULL, [we encodedLength]);
            [we setArgumentBuffer:wset offset:0];
            [we setBuffers:ws offsets:z withRange:NSMakeRange(0, 2)];
            snprintf(name, sizeof(name), "index-argbuf-store-%u", i);
            if (m5c_draw(m, ps, ^(id<MTLRenderCommandEncoder> e) {
                   [e setFragmentBuffer:ub offset:0 atIndex:0];
                   [e setFragmentBuffer:wset offset:0 atIndex:1];
                   [e useResource:w0 usage:MTLResourceUsageWrite];
                   [e useResource:w1 usage:MTLResourceUsageWrite];
                }, name)) {
               for (unsigned y = 0; y < DM_H; y++)
                  for (unsigned x = 0; x < DM_W; x++) {
                     const float c[4] = { (float)x + 0.5f, (float)y + 0.5f, 1, 1 };
                     m5c_want.care[y][x] = 1;
                     memcpy(m5c_want.px[y][x], c, sizeof(c));
                  }
               m5c_judge_bytes(m, i ? [w1 contents] : [w0 contents], &m5c_want, name, false);
            }
         }
         if (pk) {
            id<MTLBuffer> k0 = m5c_buffer(m, NULL, DM_W * DM_H * 16), k1 = m5c_buffer(m, NULL, DM_W * DM_H * 16);
            id<MTLBuffer> __unsafe_unretained ks[2] = { k0, k1 };
            id<MTLArgumentEncoder> ke = [kf newArgumentEncoderWithBufferIndex:1];
            id<MTLBuffer> kset = m5c_buffer(m, NULL, [ke encodedLength]);
            [ke setArgumentBuffer:kset offset:0];
            [ke setBuffers:ks offsets:z withRange:NSMakeRange(0, 2)];
            snprintf(name, sizeof(name), "index-kernel-store-%u", i);
            if (m5c_dispatch(m, pk, DM_W, DM_H, ^(id<MTLComputeCommandEncoder> e) {
                   [e setBuffer:ub offset:0 atIndex:0];
                   [e setBuffer:kset offset:0 atIndex:1];
                   [e useResource:k0 usage:MTLResourceUsageWrite];
                   [e useResource:k1 usage:MTLResourceUsageWrite];
                }, name)) {
               for (unsigned y = 0; y < DM_H; y++)
                  for (unsigned x = 0; x < DM_W; x++) {
                     const float c[4] = { (float)x, (float)y, 2, 1 };
                     m5c_want.care[y][x] = 1;
                     memcpy(m5c_want.px[y][x], c, sizeof(c));
                  }
               m5c_judge_bytes(m, i ? [k1 contents] : [k0 contents], &m5c_want, name, false);
            }
         }
      }
   }
   /* an array of textures a kernel writes */
   {
      id<MTLFunction> kf = m5c_fn(m, "m5c_kimg", "index-kernel-image");
      id<MTLComputePipelineState> pk = m5c_cp(m, kf, "index-kernel-image");
      for (unsigned i = 0; pk && i < 2; i++) {
         const struct m5c_u u = { i, 0 };
         id<MTLBuffer> ub = m5c_buffer(m, &u, sizeof(u));
         id<MTLTexture> a = m5c_texture(m, MTLPixelFormatRGBA32Float, DM_W, DM_H, 1,
                                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
         id<MTLTexture> b = m5c_texture(m, MTLPixelFormatRGBA32Float, DM_W, DM_H, 1,
                                        MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
         id<MTLTexture> __unsafe_unretained ws_[2] = { a, b };
         id<MTLTexture> __unsafe_unretained *ws = ws_;
         snprintf(name, sizeof(name), "index-kernel-image-%u", i);
         if (m5c_dispatch(m, pk, DM_W, DM_H, ^(id<MTLComputeCommandEncoder> e) {
                [e setTextures:ws withRange:NSMakeRange(0, 2)];
                [e setBuffer:ub offset:0 atIndex:0];
             }, name)) {
            for (unsigned y = 0; y < DM_H; y++)
               for (unsigned x = 0; x < DM_W; x++) {
                  const float c[4] = { (float)x, (float)y, 3, 1 };
                  m5c_want.care[y][x] = 1;
                  memcpy(m5c_want.px[y][x], c, sizeof(c));
               }
            m5c_judge(m, i ? b : a, &m5c_want, name, false);
         }
      }
   }
}

/* ---- a fragment or vertex shader's stores ----------------------------------------------- */

/* The counts of a counter buffer as a picture: (count, 0, 0, 1). */
static void
m5c_counts(const unsigned *c, unsigned n, float *out)
{
   for (unsigned i = 0; i < DM_W * DM_H; i++) {
      out[i * 4] = i < n ? (float)c[i] : 0;
      out[i * 4 + 1] = out[i * 4 + 2] = 0;
      out[i * 4 + 3] = 1;
   }
}

static void
m5c_want_position(float z)
{
   for (unsigned y = 0; y < DM_H; y++)
      for (unsigned x = 0; x < DM_W; x++) {
         const float c[4] = { (float)x + 0.5f, (float)y + 0.5f, z, 1 };
         m5c_want.care[y][x] = 1;
         memcpy(m5c_want.px[y][x], c, sizeof(c));
      }
}

static void
m5c_stores(struct m5c *m)
{
   static float mem[DM_W * DM_H * 4];
   id<MTLFunction> f;
   id<MTLRenderPipelineState> p;

   f = m5c_fn(m, "m5c_fs_buf", "store-fragment-buffer");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "store-fragment-buffer");
   {
      id<MTLBuffer> out = m5c_buffer(m, NULL, sizeof(mem));
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentBuffer:out offset:0 atIndex:0]; },
                   "store-fragment-buffer")) {
         m5c_want_position(1);
         m5c_judge_bytes(m, [out contents], &m5c_want, "store-fragment-buffer", false);
      }
   }
   f = m5c_fn(m, "m5c_fs_tex", "store-fragment-texture");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "store-fragment-texture");
   {
      id<MTLTexture> w = m5c_texture(m, MTLPixelFormatRGBA32Float, DM_W, DM_H, 1,
                                     MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentTexture:w atIndex:0]; },
                   "store-fragment-texture")) {
         m5c_want_position(2);
         m5c_judge(m, w, &m5c_want, "store-fragment-texture", false);
      }
   }
   /* read_write, on the one format real Metal's tier 1 has it for in every family */
   f = m5c_fn(m, "m5c_fs_rw", "store-fragment-readwrite");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "store-fragment-readwrite");
   {
      id<MTLTexture> w = m5c_texture(m, MTLPixelFormatR32Float, DM_W, DM_H, 1,
                                     MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
      static float v[DM_W * DM_H];
      for (unsigned i = 0; i < DM_W * DM_H; i++)
         v[i] = (float)i;
      [w replaceRegion:MTLRegionMake2D(0, 0, DM_W, DM_H) mipmapLevel:0 withBytes:v bytesPerRow:DM_W * 4];
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentTexture:w atIndex:0]; },
                   "store-fragment-readwrite")) {
         static float got[DM_W * DM_H];
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++) {
               const float c[4] = { (float)(y * DM_W + x), 0, 0, 1 };
               m5c_want.care[y][x] = 1;
               memcpy(m5c_want.px[y][x], c, sizeof(c));
            }
         m5c_judge(m, m->target, &m5c_want, "store-fragment-readwrite-read", false);
         [w getBytes:got bytesPerRow:DM_W * 4 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H) mipmapLevel:0];
         for (unsigned i = 0; i < DM_W * DM_H; i++) {
            mem[i * 4] = got[i];
            mem[i * 4 + 1] = mem[i * 4 + 2] = 0;
            mem[i * 4 + 3] = 1;
         }
         for (unsigned y = 0; y < DM_H; y++)
            for (unsigned x = 0; x < DM_W; x++)
               m5c_want.px[y][x][0] = (float)(y * DM_W + x) + 1;
         m5c_judge_bytes(m, mem, &m5c_want, "store-fragment-readwrite-write", false);
      }
   }
   /* once per covered pixel, whatever the GPU's binning does: which is what these two count */
   f = m5c_fn(m, "m5c_fs_atomic", "store-fragment-atomic");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "store-fragment-atomic");
   {
      id<MTLBuffer> c = m5c_buffer(m, NULL, DM_W * DM_H * 4);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentBuffer:c offset:0 atIndex:0]; },
                   "store-fragment-atomic")) {
         m5c_counts([c contents], DM_W * DM_H, mem);
         m5c_want_solid(1, 0, 0, 1);
         m5c_judge_bytes(m, mem, &m5c_want, "store-fragment-atomic", false);
      }
   }
   f = m5c_fn(m, "m5c_vs_atomic", "store-vertex-atomic");
   p = m5c_rp(m, f, m5c_fn(m, "m5c_fsolid", "store-vertex-atomic"), MTLPixelFormatInvalid,
              "store-vertex-atomic");
   if (p) {
      id<MTLBuffer> c = m5c_buffer(m, NULL, DM_W * DM_H * 4);
      id<MTLCommandBuffer> cb = [m->queue commandBuffer];
      m5c_pass(m, cb, m->target, p, 4, 3, ^(id<MTLRenderCommandEncoder> e) {
         [e setVertexBuffer:c offset:0 atIndex:0];
      });
      /* At least once and not exactly once: real Metal runs vertices 1 and 2 of a strip twice on an
       * M1 (counts 1 2 2 1, per instance), and it promises no count. What matters here is that
       * each ran and that none ran a binning pass's extra time unseen: the counts are printed. */
      if (m5c_finish(m, cb, "store-vertex-atomic")) {
         const unsigned *n = [c contents];
         printf("m5c: %s vertex stage counts", m->backend);
         for (unsigned i = 0; i < 12; i++)
            printf(" %u", n[i]);
         printf("\n");
         for (unsigned i = 0; i < DM_W * DM_H; i++) {
            mem[i * 4] = i < 12 && n[i] >= 1 ? 1 : (i < 12 ? 0 : (float)n[i]);
            mem[i * 4 + 1] = mem[i * 4 + 2] = 0;
            mem[i * 4 + 3] = 1;
         }
         m5c_want_solid(0, 0, 0, 1);
         for (unsigned i = 0; i < 12; i++)
            m5c_want.px[i / DM_W][i % DM_W][0] = 1;
         m5c_judge_bytes(m, mem, &m5c_want, "store-vertex-atomic", false);
      }
   }
   f = m5c_fn(m, "m5c_vs_buf", "store-vertex-buffer");
   p = m5c_rp(m, f, m5c_fn(m, "m5c_fsolid", "store-vertex-buffer"), MTLPixelFormatInvalid,
              "store-vertex-buffer");
   {
      id<MTLBuffer> out = m5c_buffer(m, NULL, sizeof(mem));
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setVertexBuffer:out offset:0 atIndex:0]; },
                   "store-vertex-buffer")) {
         static const float pos[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
         m5c_want_solid(0, 0, 0, 0);
         for (unsigned i = 0; i < 4; i++) {
            const float c[4] = { pos[i][0], pos[i][1], (float)i, 1 };
            memcpy(m5c_want.px[0][i], c, sizeof(c));
         }
         m5c_judge_bytes(m, [out contents], &m5c_want, "store-vertex-buffer", false);
      }
   }
   /* through an argument buffer */
   f = m5c_fn(m, "m5c_fs_argbuf", "store-argbuf");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "store-argbuf");
   if (p) {
      id<MTLArgumentEncoder> enc = [f newArgumentEncoderWithBufferIndex:0];
      id<MTLBuffer> set = m5c_buffer(m, NULL, [enc encodedLength]);
      id<MTLBuffer> out = m5c_buffer(m, NULL, sizeof(mem));
      [enc setArgumentBuffer:set offset:0];
      [enc setBuffer:out offset:0 atIndex:0];
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentBuffer:set offset:0 atIndex:0];
             [e useResource:out usage:MTLResourceUsageWrite];
          }, "store-argbuf")) {
         m5c_want_position(3);
         m5c_judge_bytes(m, [out contents], &m5c_want, "store-argbuf", false);
      }
   }
   /* [[early_fragment_tests]]: a pixel the depth test rejects does not run, and so does not count */
   f = m5c_fn(m, "m5c_fs_early", "store-early-tests");
   p = m5c_rp(m, m->vs, f, MTLPixelFormatDepth32Float, "store-early-tests");
   if (p) {
      MTLTextureDescriptor *zd =
         [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                            width:DM_W height:DM_H mipmapped:NO];
      [zd setUsage:MTLTextureUsageRenderTarget];
      [zd setStorageMode:MTLStorageModePrivate];
      id<MTLTexture> z = [m->dev newTextureWithDescriptor:zd];
      MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
      [dd setDepthCompareFunction:MTLCompareFunctionLess];
      [dd setDepthWriteEnabled:NO];
      id<MTLDepthStencilState> ds = [m->dev newDepthStencilStateWithDescriptor:dd];
      static const struct { const char *name; double clear; float want; } cases[2] = {
         { "store-early-tests-fail", 0.25, 0 }, { "store-early-tests-pass", 0.75, 1 },
      };
      for (unsigned i = 0; i < 2; i++) {
         id<MTLBuffer> c = m5c_buffer(m, NULL, DM_W * DM_H * 4);
         MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
         [[rp colorAttachments][0] setTexture:m->target];
         [[rp colorAttachments][0] setLoadAction:MTLLoadActionClear];
         [[rp colorAttachments][0] setStoreAction:MTLStoreActionStore];
         [[rp depthAttachment] setTexture:z];
         [[rp depthAttachment] setLoadAction:MTLLoadActionClear];
         [[rp depthAttachment] setStoreAction:MTLStoreActionDontCare];
         [[rp depthAttachment] setClearDepth:cases[i].clear];
         id<MTLCommandBuffer> cb = [m->queue commandBuffer];
         id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
         [e setRenderPipelineState:p];
         [e setDepthStencilState:ds];
         [e setFragmentBuffer:c offset:0 atIndex:0];
         [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
         [e endEncoding];
         if (m5c_finish(m, cb, cases[i].name)) {
            m5c_counts([c contents], DM_W * DM_H, mem);
            m5c_want_solid(cases[i].want, 0, 0, 1);
            m5c_judge_bytes(m, mem, &m5c_want, cases[i].name, false);
         }
      }
   }
}

/* ---- what a system shader asked for that no row had (run 125612) ------------------------- */

static void
m5c_three(struct m5c *m)
{
   /* 0.25, 0.75, 0.5 in every order that changes which one wins; float, half, int, uint */
   const float abc[4] = { 0.25f, 0.75f, 0.5f, 0 };
   id<MTLBuffer> u = m5c_buffer(m, abc, sizeof(abc));
   static const struct { const char *fn, *name; float want[4]; } rows[] = {
      { "m5c_three_f32", "three-f32", { 0.75f, 0.25f, 0.5f, 1 } },
      { "m5c_three_f16", "three-f16", { 0.75f, 0.25f, 0.5f, 1 } },
      { "m5c_three_int", "three-int", { 3, 1, 2, 1 } },
      { "m5c_three_uint", "three-uint", { 3, 1, 2, 1 } },
      /* red 8 in the LOW five bits, green 47, blue 16 at the top: what an M1 answers */
      { "m5c_pack565", "three-pack565", { 34280, 0, 0, 1 } },
   };
   for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      id<MTLFunction> f = m5c_fn(m, rows[i].fn, rows[i].name);
      id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, rows[i].name);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentBuffer:u offset:0 atIndex:0]; },
                   rows[i].name)) {
         m5c_want_solid(rows[i].want[0], rows[i].want[1], rows[i].want[2], rows[i].want[3]);
         m5c_judge(m, m->target, &m5c_want, rows[i].name, false);
      }
   }
}

/* ---- pack and unpack ------------------------------------------------------------------------ */

static void
m5c_pack(struct m5c *m)
{
   id<MTLFunction> f = m5c_fn(m, "m5c_pack_all", "pack");
   id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "pack");
   /* -0.5, 0.25, 0.75 and 1.5 clamp, round and take a sign: one set for all */
   static const float in[4] = { -0.5f, 0.25f, 0.75f, 1.5f };
   const uint32_t word = 0xC3A5F01Eu;
   /* the words by hand, low field first */
   const uint32_t un8 = 0u | (64u << 8) | (191u << 16) | (255u << 24);
   /* -0.5 of 127 and of 32767 go away from zero, as an M1's do */
   const uint32_t sn8 = (uint32_t)(uint8_t)(-64) | (32u << 8) | (95u << 16) | (127u << 24);
   const uint32_t un16 = 0u | (16384u << 16);
   const uint32_t sn16 = (uint32_t)(uint16_t)(-16384) | (8192u << 16);
   const uint32_t un10 = 0u | (256u << 10) | (767u << 20) | (3u << 30);
   const uint32_t un565 = 0u | (16u << 5) | (23u << 11);
   static const struct { const char *name; unsigned which; } words[] = {
      { "pack-unorm4x8", 0 }, { "pack-snorm4x8", 1 }, { "pack-unorm2x16", 2 }, { "pack-snorm2x16", 3 },
      { "pack-unorm10a2", 4 }, { "pack-unorm565", 6 }, { "pack-half-unorm4x8", 7 },
      { "pack-half-unorm2x16", 8 }, { "pack-half-unorm10a2", 9 },
   };
   /* snorm10a2 is not in this Mac's compiler (__HAVE_PACK_UNPACK_RGB10A2_SNORM_FUNCTIONS__): not measured */
   const uint32_t want_word[9] = { un8, sn8, un16, sn16, un10, un565, un8, un16, un10 };
   const unsigned rows = sizeof(words) / sizeof(words[0]);
   static const unsigned unpacks[] = { 10, 11, 12, 13, 14, 16, 17, 18 };
   for (unsigned i = 0; i < rows + 8; i++) {
      const unsigned which = i < rows ? words[i].which : unpacks[i - rows];
      char name[40];
      snprintf(name, sizeof(name), "%s", i < rows ? words[i].name : "unpack");
      if (i >= rows)
         snprintf(name, sizeof(name), "unpack-%u", which);
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentBytes:in length:sizeof(in) atIndex:0];
             [e setFragmentBytes:&which length:sizeof(which) atIndex:1];
             [e setFragmentBytes:&word length:sizeof(word) atIndex:2];
          }, name)) {
         if (i < rows) {
            m5c_want_solid((float)(want_word[i] & 0xffff), (float)(want_word[i] >> 16), 0, 1);
         } else {
            /* 0xC3A5F01E, a field at a time, low first */
            const float u8s[4] = { 0x1E / 255.0f, 0xF0 / 255.0f, 0xA5 / 255.0f, 0xC3 / 255.0f };
            const float s8s[4] = { 30 / 127.0f, -16 / 127.0f, -91 / 127.0f, -61 / 127.0f };
            const float u16s[2] = { 0xF01E / 65535.0f, 0xC3A5 / 65535.0f };
            const float s16s[2] = { -4066 / 32767.0f, -15451 / 32767.0f };
            const uint32_t r10 = word & 1023, g10 = (word >> 10) & 1023, b10 = (word >> 20) & 1023, a2 = word >> 30;
            const uint32_t w16 = word & 0xffff;
            switch (which) {
            case 10: m5c_want_solid(u8s[0], u8s[1], u8s[2], u8s[3]); break;
            case 11: m5c_want_solid(s8s[0], s8s[1], s8s[2], s8s[3]); break;
            case 12: m5c_want_solid(u16s[0], u16s[1], 0, 1); break;
            case 13: m5c_want_solid(s16s[0], s16s[1], 0, 1); break;
            case 14: case 18: m5c_want_solid(r10 / 1023.0f, g10 / 1023.0f, b10 / 1023.0f, a2 / 3.0f); break;
            case 16: m5c_want_solid((w16 & 31) / 31.0f, ((w16 >> 5) & 63) / 63.0f, (w16 >> 11) / 31.0f, 1); break;
            case 17: m5c_want_solid(u16s[0], u16s[1], u16s[0], u16s[1]); break;
            }
         }
         m5c_judge(m, m->target, &m5c_want, name, false);
      }
   }
}

/* ---- half rows in a constant buffer, at the offsets CoreAnimation binds -------------------- */

static void
m5c_half_rows(struct m5c *m)
{
   /* rows of half: an identity with an offset. Binary16 of 1, 0.5, 0.25 and 0.125 by hand. */
   const uint16_t one = 0x3c00, half_ = 0x3800, quarter = 0x3400, eighth = 0x3000, none = 0;
   const uint16_t rows[5][4] = {
      { one, none, none, none }, { none, one, none, none }, { none, none, one, none },
      { none, none, none, one }, { eighth, quarter, none, none },
   };
   (void)half_;
   {
      id<MTLFunction> vf = m5c_fn(m, "m5c_vs_half", "half-varying");
      id<MTLFunction> ff = m5c_fn(m, "m5c_fs_half", "half-varying");
      id<MTLRenderPipelineState> vp = m5c_rp(m, vf, ff, MTLPixelFormatInvalid, "half-varying");
      if (m5c_draw(m, vp, nil, "half-varying")) {
         m5c_want_solid(0.25f, 0.5f, 0.75f, 1);
         m5c_judge(m, m->target, &m5c_want, "half-varying", false);
      }
   }
   static const unsigned offsets[] = { 0, 256, 40096, 19904, 32, 704 };
   for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      char row[48];
      snprintf(row, sizeof(row), "half-rows-offset-%u", offsets[i]);
      id<MTLBuffer> b = m5c_buffer(m, NULL, 65536);
      memcpy((char *)[b contents] + offsets[i] + 128, rows, sizeof(rows));
      id<MTLFunction> f = m5c_fn(m, "m5c_half_rows", row);
      id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, row);
      const unsigned off = offsets[i];
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentBuffer:b offset:off atIndex:1]; }, row)) {
         m5c_want_solid(0.625f, 0.5f, 0.75f, 1);
         m5c_judge(m, m->target, &m5c_want, row, false);
      }
   }
}

/* ---- framebuffer fetch ------------------------------------------------------------------- */

/* cleared to (0.25, 0.5, 0.75, 1), and `draws` draws of `instances` whole-target quads each */
static void
m5c_fetch_row(struct m5c *m, const char *fn, unsigned draws, unsigned instances, MTLPixelFormat format,
              const float want[4], const char *row)
{
   id<MTLFunction> f = m5c_fn(m, fn, row);
   if (f == nil)
      return;
   MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
   NSError *e = nil;
   [d setVertexFunction:m->vs];
   [d setFragmentFunction:f];
   [[d colorAttachments][0] setPixelFormat:format];
   id<MTLRenderPipelineState> p = [m->dev newRenderPipelineStateWithDescriptor:d error:&e];
   if (p == nil) {
      m5c_nserror(m, row, "no pipeline", e);
      return;
   }
   id<MTLTexture> t = format == MTLPixelFormatRGBA32Float ? m->target
      : m5c_texture(m, format, DM_W, DM_H, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
   [[rp colorAttachments][0] setTexture:t];
   [[rp colorAttachments][0] setLoadAction:MTLLoadActionClear];
   [[rp colorAttachments][0] setStoreAction:MTLStoreActionStore];
   [[rp colorAttachments][0] setClearColor:MTLClearColorMake(0.25, 0.5, 0.75, 1)];
   id<MTLCommandBuffer> cb = [m->queue commandBuffer];
   id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
   [enc setRenderPipelineState:p];
   for (unsigned i = 0; i < draws; i++)
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:instances];
   [enc endEncoding];
   if (!m5c_finish(m, cb, row))
      return;
   m5c_want_solid(want[0], want[1], want[2], want[3]);
   if (format == MTLPixelFormatRGBA32Float) {
      m5c_judge(m, t, &m5c_want, row, false);
   } else {
      static uint8_t px[DM_W * DM_H * 4];
      static float mem[DM_W * DM_H * 4];
      [t getBytes:px bytesPerRow:DM_W * 4 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H) mipmapLevel:0];
      for (unsigned i = 0; i < DM_W * DM_H * 4; i++)
         mem[i] = (float)px[i] / 255.0f;
      if (format == MTLPixelFormatBGRA8Unorm)
         for (unsigned i = 0; i < DM_W * DM_H; i++) {
            float b = mem[i * 4];
            mem[i * 4] = mem[i * 4 + 2];
            mem[i * 4 + 2] = b;
         }
      m5c_judge_bytes(m, mem, &m5c_want, row, false);
   }
}

/* draw into attachment 1 and read it back into attachment 0, in one encoder */
static void
m5c_fetch_second(struct m5c *m, MTLPixelFormat format, bool memoryless_second, const char *row)
{
   id<MTLFunction> w = m5c_fn(m, "m5c_fetch_w1", row), r = m5c_fn(m, "m5c_fetch_r1", row);
   if (w == nil || r == nil)
      return;
   NSError *e = nil;
   MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
   [d setVertexFunction:m->vs];
   [d setFragmentFunction:w];
   [[d colorAttachments][0] setPixelFormat:format];
   [[d colorAttachments][0] setWriteMask:MTLColorWriteMaskNone];
   [[d colorAttachments][1] setPixelFormat:format];
   id<MTLRenderPipelineState> pw = [m->dev newRenderPipelineStateWithDescriptor:d error:&e];
   [d setFragmentFunction:r];
   [[d colorAttachments][0] setWriteMask:MTLColorWriteMaskAll];
   id<MTLRenderPipelineState> pr = [m->dev newRenderPipelineStateWithDescriptor:d error:&e];
   if (pw == nil || pr == nil) {
      m5c_nserror(m, row, "no pipeline", e);
      return;
   }
   id<MTLTexture> t0 = m5c_texture(m, format, DM_W, DM_H, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
   id<MTLTexture> t1 = m5c_texture(m, format, DM_W, DM_H, 1, MTLTextureUsageRenderTarget);
   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
   [[rp colorAttachments][0] setTexture:t0];
   [[rp colorAttachments][0] setLoadAction:MTLLoadActionClear];
   [[rp colorAttachments][0] setStoreAction:MTLStoreActionStore];
   [[rp colorAttachments][1] setTexture:t1];
   [[rp colorAttachments][1] setLoadAction:MTLLoadActionClear];
   [[rp colorAttachments][1] setStoreAction:memoryless_second ? MTLStoreActionDontCare : MTLStoreActionStore];
   id<MTLCommandBuffer> cb = [m->queue commandBuffer];
   id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
   [enc setRenderPipelineState:pw];
   [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   [enc setRenderPipelineState:pr];
   [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   [enc endEncoding];
   if (!m5c_finish(m, cb, row))
      return;
   static uint8_t px[DM_W * DM_H * 4];
   static float mem[DM_W * DM_H * 4];
   [t0 getBytes:px bytesPerRow:DM_W * 4 fromRegion:MTLRegionMake2D(0, 0, DM_W, DM_H) mipmapLevel:0];
   for (unsigned i = 0; i < DM_W * DM_H * 4; i++)
      mem[i] = (float)px[i] / 255.0f;
   if (format == MTLPixelFormatBGRA8Unorm)
      for (unsigned i = 0; i < DM_W * DM_H; i++) {
         float b = mem[i * 4];
         mem[i * 4] = mem[i * 4 + 2];
         mem[i * 4 + 2] = b;
      }
   m5c_want_solid(64.0f / 255.0f, 128.0f / 255.0f, 191.0f / 255.0f, 1);
   m5c_judge_bytes(m, mem, &m5c_want, row, false);
}

static void
m5c_fetch(struct m5c *m)
{
   m5c_fetch_second(m, MTLPixelFormatBGRA8Unorm, false, "fetch-second-bgra8");
   m5c_fetch_second(m, MTLPixelFormatBGRA8Unorm, true, "fetch-second-bgra8-dontcare");
   const float read[4] = { 0.625f, 0.5f, 0.375f, 0.75f };
   const float once[4] = { 0.375f, 0.625f, 0.875f, 1 };
   const float three[4] = { 0.625f, 0.875f, 1.125f, 1 };
   /* 160/255, 224/255: the eight bits are read back between the three, and round each time */
   const float three8[4] = { 160.0f / 255.0f, 224.0f / 255.0f, 1, 1 };
   m5c_fetch_row(m, "m5c_fetch", 1, 1, MTLPixelFormatRGBA32Float, read, "fetch-read");
   m5c_fetch_row(m, "m5c_fetch_add", 1, 1, MTLPixelFormatRGBA32Float, once, "fetch-one-add");
   m5c_fetch_row(m, "m5c_fetch_add", 3, 1, MTLPixelFormatRGBA32Float, three, "fetch-three-draws");
   m5c_fetch_row(m, "m5c_fetch_add", 1, 3, MTLPixelFormatRGBA32Float, three, "fetch-three-overlap");
   m5c_fetch_row(m, "m5c_fetch_add", 1, 3, MTLPixelFormatBGRA8Unorm, three8, "fetch-three-overlap-bgra8");
}

/* ---- function constants ----------------------------------------------------------------- */

static id<MTLFunction>
m5c_spec(struct m5c *m, const char *fn, MTLFunctionConstantValues *v, const char *row)
{
   NSError *e = nil;
   id<MTLFunction> f = [m->lib newFunctionWithName:[NSString stringWithUTF8String:fn] constantValues:v
                                             error:&e];
   if (f == nil)
      m5c_nserror(m, row, "no specialized function", e);
   return f;
}

/* (fc_f, fc_or, fc_b) of m5c_fc1 under these values */
static void
m5c_fc1_row(struct m5c *m, MTLFunctionConstantValues *v, float r, float g, float b, const char *row)
{
   id<MTLFunction> f = m5c_spec(m, "m5c_fc1", v, row);
   id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, row);
   if (m5c_draw(m, p, nil, row)) {
      m5c_want_solid(r, g, b, 1);
      m5c_judge(m, m->target, &m5c_want, row, false);
   }
}

static void
m5c_constants(struct m5c *m)
{
   bool yes = true, no = false;
   float half_ = 0.5f, quarter = 0.25f;
   int three = 3, two = 2;
   MTLFunctionConstantValues *v;

   v = [[MTLFunctionConstantValues alloc] init];
   [v setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
   [v setConstantValue:&half_ type:MTLDataTypeFloat atIndex:1];
   [v setConstantValue:&three type:MTLDataTypeInt atIndex:2];
   m5c_fc1_row(m, v, 0.5f, 3, 1, "fc-all-set");

   /* the two is_function_constant_defined take their other side */
   v = [[MTLFunctionConstantValues alloc] init];
   [v setConstantValue:&no type:MTLDataTypeBool atIndex:0];
   m5c_fc1_row(m, v, -1, 7, 0, "fc-not-defined");

   /* the other two setters */
   v = [[MTLFunctionConstantValues alloc] init];
   [v setConstantValue:&no type:MTLDataTypeBool withName:@"fc_b"];
   [v setConstantValue:&quarter type:MTLDataTypeFloat withName:@"fc_f"];
   [v setConstantValue:&two type:MTLDataTypeInt withName:@"fc_i"];
   m5c_fc1_row(m, v, 0.25f, 2, 0, "fc-by-name");
   v = [[MTLFunctionConstantValues alloc] init];
   [v setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
   [v setConstantValues:&quarter type:MTLDataTypeFloat withRange:NSMakeRange(1, 1)];
   [v setConstantValues:&three type:MTLDataTypeInt withRange:NSMakeRange(2, 1)];
   m5c_fc1_row(m, v, 0.25f, 3, 1, "fc-range");
   /* -reset forgets them */
   v = [[MTLFunctionConstantValues alloc] init];
   [v setConstantValue:&quarter type:MTLDataTypeFloat atIndex:1];
   [v reset];
   [v setConstantValue:&no type:MTLDataTypeBool atIndex:0];
   m5c_fc1_row(m, v, -1, 7, 0, "fc-reset");

   /* two specializations of one function, alive together: -newFunctionWithName: is one object */
   {
      MTLFunctionConstantValues *a = [[MTLFunctionConstantValues alloc] init];
      MTLFunctionConstantValues *b = [[MTLFunctionConstantValues alloc] init];
      float fa = 1, fb = 2;
      [a setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
      [a setConstantValue:&fa type:MTLDataTypeFloat atIndex:1];
      [b setConstantValue:&no type:MTLDataTypeBool atIndex:0];
      [b setConstantValue:&fb type:MTLDataTypeFloat atIndex:1];
      id<MTLFunction> ffa = m5c_spec(m, "m5c_fc1", a, "fc-two-at-once");
      id<MTLFunction> ffb = m5c_spec(m, "m5c_fc1", b, "fc-two-at-once");
      id<MTLRenderPipelineState> pa = m5c_rp(m, m->vs, ffa, MTLPixelFormatInvalid, "fc-two-at-once");
      id<MTLRenderPipelineState> pb = m5c_rp(m, m->vs, ffb, MTLPixelFormatInvalid, "fc-two-at-once");
      if (m5c_draw(m, pa, nil, "fc-two-at-once-a")) {
         m5c_want_solid(1, 7, 1, 1);
         m5c_judge(m, m->target, &m5c_want, "fc-two-at-once-a", false);
      }
      if (m5c_draw(m, pb, nil, "fc-two-at-once-b")) {
         m5c_want_solid(2, 7, 0, 1);
         m5c_judge(m, m->target, &m5c_want, "fc-two-at-once-b", false);
      }
   }
   /* the two other ways of asking */
   {
      __block id<MTLFunction> got = nil;
      __block NSError *err = nil;
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
      [v setConstantValue:&quarter type:MTLDataTypeFloat atIndex:1];
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [m->lib newFunctionWithName:@"m5c_fc1" constantValues:v
                completionHandler:^(id<MTLFunction> f, NSError *e) {
                   /* the phone's test is not under ARC, and the function is only the block's */
                   if (f != nil)
                      CFRetain((__bridge CFTypeRef)f);
                   if (e != nil)
                      CFRetain((__bridge CFTypeRef)e);
                   got = f;
                   err = e;
                   dispatch_semaphore_signal(done);
                }];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      if (got == nil) {
         m5c_nserror(m, "fc-completion", "no function", err);
      } else {
         id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, got, MTLPixelFormatInvalid, "fc-completion");
         if (m5c_draw(m, p, nil, "fc-completion")) {
            m5c_want_solid(0.25f, 7, 1, 1);
            m5c_judge(m, m->target, &m5c_want, "fc-completion", false);
         }
      }
      MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
      NSError *e = nil;
      [d setName:@"m5c_fc1"];
      [d setConstantValues:v];
      id<MTLFunction> f = [m->lib newFunctionWithDescriptor:d error:&e];
      if (f == nil) {
         m5c_nserror(m, "fc-descriptor", "no function", e);
      } else {
         id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "fc-descriptor");
         if (m5c_draw(m, p, nil, "fc-descriptor")) {
            m5c_want_solid(0.25f, 7, 1, 1);
            m5c_judge(m, m->target, &m5c_want, "fc-descriptor", false);
         }
      }
   }
   /* a vector, and the two small integer types */
   {
      const float four[4] = { 1, 2, 3, 4 }, duo[2] = { 0.5f, 0.25f };
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:four type:MTLDataTypeFloat4 atIndex:3];
      [v setConstantValue:duo type:MTLDataTypeFloat2 atIndex:4];
      id<MTLFunction> f = m5c_spec(m, "m5c_fc2", v, "fc-vector");
      id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "fc-vector");
      if (m5c_draw(m, p, nil, "fc-vector")) {
         m5c_want_solid(1.5f, 2.25f, 3, 4);
         m5c_judge(m, m->target, &m5c_want, "fc-vector", false);
      }
      const unsigned char small = 200;
      const unsigned short mid = 60000;
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&small type:MTLDataTypeUChar atIndex:5];
      [v setConstantValue:&mid type:MTLDataTypeUShort atIndex:6];
      f = m5c_spec(m, "m5c_fc4", v, "fc-small-integers");
      p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "fc-small-integers");
      if (m5c_draw(m, p, nil, "fc-small-integers")) {
         m5c_want_solid(200, 60000, 0, 1);
         m5c_judge(m, m->target, &m5c_want, "fc-small-integers", false);
      }
   }
   /* what a constant gates need not be bound */
   {
      id<MTLTexture> t0 = m->t[0];
      id<MTLSamplerState> near = m->near;
      const float uv[4] = { 0.25f, 0.5f, 0.75f, 1 };
      id<MTLBuffer> ub = m5c_buffer(m, uv, sizeof(uv));
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
      id<MTLFunction> f = m5c_spec(m, "m5c_fc3", v, "fc-gated-on");
      id<MTLRenderPipelineState> p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "fc-gated-on");
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) {
             [e setFragmentTexture:t0 atIndex:0];
             [e setFragmentSamplerState:near atIndex:0];
          }, "fc-gated-on")) {
         m5c_want_texels(0, NULL);
         m5c_judge(m, m->target, &m5c_want, "fc-gated-on", false);
      }
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&no type:MTLDataTypeBool atIndex:0];
      f = m5c_spec(m, "m5c_fc3", v, "fc-gated-off");
      p = m5c_rp(m, m->vs, f, MTLPixelFormatInvalid, "fc-gated-off");
      if (m5c_draw(m, p, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentBuffer:ub offset:0 atIndex:0]; },
                   "fc-gated-off")) {
         m5c_want_solid(0.25f, 0.5f, 0.75f, 1);
         m5c_judge(m, m->target, &m5c_want, "fc-gated-off", false);
      }
   }
   /* the vertex stage's, and a kernel's */
   {
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&half_ type:MTLDataTypeFloat atIndex:1];
      id<MTLFunction> vf = m5c_spec(m, "m5c_vc", v, "fc-vertex");
      id<MTLRenderPipelineState> p = m5c_rp(m, vf, m5c_fn(m, "m5c_fsolid", "fc-vertex"),
                                            MTLPixelFormatInvalid, "fc-vertex");
      if (m5c_draw(m, p, nil, "fc-vertex")) {
         m5c_want_solid(0, 0, 0, 0);
         for (unsigned y = 4; y < 12; y++)
            for (unsigned x = 4; x < 12; x++) {
               const float c[4] = { 0.5f, 0.25f, 0.75f, 1 };
               memcpy(m5c_want.px[y][x], c, sizeof(c));
            }
         m5c_judge(m, m->target, &m5c_want, "fc-vertex", false);
      }
      v = [[MTLFunctionConstantValues alloc] init];
      [v setConstantValue:&yes type:MTLDataTypeBool atIndex:0];
      [v setConstantValue:&quarter type:MTLDataTypeFloat atIndex:1];
      [v setConstantValue:&two type:MTLDataTypeInt atIndex:2];
      id<MTLFunction> kf = m5c_spec(m, "m5c_kc", v, "fc-kernel");
      id<MTLComputePipelineState> pk = m5c_cp(m, kf, "fc-kernel");
      id<MTLBuffer> out = m5c_buffer(m, NULL, DM_W * DM_H * 16);
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) { [e setBuffer:out offset:0 atIndex:0]; },
                       "fc-kernel")) {
         m5c_want_solid(0, 0, 0, 0);
         const float c[4] = { 0.25f, 2, 1, 1 };
         memcpy(m5c_want.px[0][0], c, sizeof(c));
         m5c_judge_bytes(m, [out contents], &m5c_want, "fc-kernel", false);
      }
   }
}

/* ---- kernels as VideoToolbox's scale writes them (M5e) ------------------------------------ */

static void
m5c_kernels(struct m5c *m)
{
   id<MTLBuffer> out = m5c_buffer(m, NULL, DM_W * DM_H * 16);
   uint8_t px[8 * 8 * 4 + 4 * 4 + 4];
   MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
   [sd setMinFilter:MTLSamplerMinMagFilterNearest];
   [sd setMagFilter:MTLSamplerMinMagFilterNearest];
   [sd setMipFilter:MTLSamplerMipFilterNearest];
   /* level 0 red, level 1 green: which level a kernel's sample reaches */
   id<MTLTexture> mips = m5c_texture(m, MTLPixelFormatRGBA8Unorm, 8, 8, 2, MTLTextureUsageShaderRead);
   for (unsigned i = 0; i < 64; i++)
      memcpy(px + i * 4, (const uint8_t[4]){ 255, 0, 0, 255 }, 4);
   [mips replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:px bytesPerRow:32];
   for (unsigned i = 0; i < 16; i++)
      memcpy(px + i * 4, (const uint8_t[4]){ 0, 255, 0, 255 }, 4);
   [mips replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:1 withBytes:px bytesPerRow:16];
   {
      id<MTLSamplerState> st = [m->dev newSamplerStateWithDescriptor:sd];
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_ks", "kernel-sample-level"), "kernel-sample-level");
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             [e setTexture:mips atIndex:0];
             [e setSamplerState:st atIndex:0];
             [e setBuffer:out offset:0 atIndex:0];
          }, "kernel-sample-level")) {
         m5c_want_solid(0, 0, 0, 0);
         const float c[4] = { 1, 0, 0, 1 };
         memcpy(m5c_want.px[0][0], c, sizeof(c));
         m5c_judge_bytes(m, [out contents], &m5c_want, "kernel-sample-level", false);
      }
   }
   /* a picture of known texels, sampled in pixels through an API sampler, as half */
   id<MTLTexture> pic = m5c_texture(m, MTLPixelFormatBGRA8Unorm, 8, 8, 1, MTLTextureUsageShaderRead);
   for (unsigned y = 0; y < 8; y++)
      for (unsigned x = 0; x < 8; x++)
         memcpy(px + (y * 8 + x) * 4, (const uint8_t[4]){ (uint8_t)(x * 32), (uint8_t)(y * 32), 128, 255 }, 4);
   [pic replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:px bytesPerRow:32];
   [sd setNormalizedCoordinates:NO];
   [sd setMipFilter:MTLSamplerMipFilterNotMipmapped];
   id<MTLSamplerState> pixels = [m->dev newSamplerStateWithDescriptor:sd];
   {
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_kh", "kernel-half-pixels"), "kernel-half-pixels");
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             [e setTexture:pic atIndex:0];
             [e setSamplerState:pixels atIndex:0];
             [e setBuffer:out offset:0 atIndex:0];
          }, "kernel-half-pixels")) {
         m5c_want_solid(0, 0, 0, 0);
         const float c[4] = { 128.0f / 255, 64.0f / 255, 64.0f / 255, 1 };
         memcpy(m5c_want.px[0][0], c, sizeof(c));
         m5c_judge_bytes(m, [out contents], &m5c_want, "kernel-half-pixels", false);
      }
   }
   {
      const uint32_t n = 4;
      id<MTLBuffer> counts = m5c_buffer(m, &n, sizeof(n));
      memset([out contents], 0, DM_W * DM_H * 16);
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_kha", "kernel-half-array"), "kernel-half-array");
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             [e setTexture:pic atIndex:0];
             [e setSamplerState:pixels atIndex:0];
             [e setBuffer:out offset:0 atIndex:0];
             [e setBuffer:counts offset:0 atIndex:1];
          }, "kernel-half-array")) {
         m5c_want_solid(0, 0, 0, 0);
         for (unsigned i = 0; i < 4; i++) {
            const float c[4] = { (2 + i) * 32.0f / 255, 64.0f / 255, 128.0f / 255, 1 };
            memcpy(m5c_want.px[0][i], c, sizeof(c));
         }
         m5c_judge_bytes(m, [out contents], &m5c_want, "kernel-half-array", false);
      }
   }
   /* written as float and as half, read back through -getBytes: as the format's own bytes */
   {
      const struct { const char *fn, *row; MTLPixelFormat format; unsigned bytes; const uint8_t want[8]; } writes[] = {
         { "m5c_kwf", "kernel-write-float-rgba16f", MTLPixelFormatRGBA16Float, 8, { 0, 0x34, 0, 0x38, 0, 0x3a, 0, 0x3c } },
         { "m5c_kwf", "kernel-write-float-bgra8", MTLPixelFormatBGRA8Unorm, 4, { 191, 128, 64, 255 } },
         { "m5c_kwf", "kernel-write-float-rgba8", MTLPixelFormatRGBA8Unorm, 4, { 64, 128, 191, 255 } },
         { "m5c_kwh", "kernel-write-half-rgba16f", MTLPixelFormatRGBA16Float, 8, { 0, 0x34, 0, 0x38, 0, 0x3a, 0, 0x3c } },
         { "m5c_kwh", "kernel-write-half-bgra8", MTLPixelFormatBGRA8Unorm, 4, { 191, 128, 64, 255 } },
      };
      for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
         id<MTLTexture> o = m5c_texture(m, writes[i].format, 2, 2, 1, MTLTextureUsageShaderWrite);
         id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, writes[i].fn, writes[i].row), writes[i].row);
         if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) { [e setTexture:o atIndex:0]; }, writes[i].row)) {
            uint8_t got[8] = { 0 };
            float *as_float = (float *)[out contents];
            [o getBytes:got bytesPerRow:writes[i].bytes * 2 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
            /* the bytes as numbers, one row a picture: what differs prints, and the verdict is the bytes' */
            memset([out contents], 0, DM_W * DM_H * 16);
            m5c_want_solid(0, 0, 0, 0);
            for (unsigned k = 0; k < writes[i].bytes; k++) {
               as_float[k] = got[k];
               m5c_want.px[0][k / 4][k % 4] = writes[i].want[k];
            }
            m5c_judge_bytes(m, as_float, &m5c_want, writes[i].row, false);
         }
      }
   }
   /* an array of textures copied into a local and read at a computed index, as Photos' grid does */
   {
      static const uint8_t colours[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
      NSMutableArray *three = [NSMutableArray array];
      for (unsigned k = 0; k < 3; k++) {
         id<MTLTexture> made = m5c_texture(m, MTLPixelFormatRGBA8Unorm, 8, 8, 1, MTLTextureUsageShaderRead);
         for (unsigned i = 0; i < 64; i++)
            memcpy(px + i * 4, colours[k], 4);
         [made replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:px bytesPerRow:32];
         [three addObject:made];
      }
      [sd setNormalizedCoordinates:YES];
      id<MTLSamplerState> st = [m->dev newSamplerStateWithDescriptor:sd];
      const uint32_t n = 1;
      const float cursor[8] = { 1, 1, 1, 1, 0.5f, 0.5f, 0.5f, 0.5f };
      id<MTLBuffer> counts = m5c_buffer(m, &n, sizeof(n));
      id<MTLBuffer> cursors = m5c_buffer(m, cursor, sizeof(cursor));
      memset([out contents], 0, DM_W * DM_H * 16);
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_kdc", "kernel-descriptors-in-a-local"), "kernel-descriptors-in-a-local");
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             for (unsigned k = 0; k < 3; k++)
                [e setTexture:[three objectAtIndex:k] atIndex:k];
             [e setSamplerState:st atIndex:0];
             [e setBuffer:out offset:0 atIndex:0];
             [e setBuffer:counts offset:0 atIndex:1];
             [e setBuffer:cursors offset:0 atIndex:2];
          }, "kernel-descriptors-in-a-local")) {
         m5c_want_solid(0, 0, 0, 0);
         const float c0[4] = { 0, 1, 0, 1 }, c1[4] = { 0, 0, 2, 2 };   /* (used is 2 by then, and 0.5 * 2) */
         memcpy(m5c_want.px[0][0], c0, sizeof(c0));
         memcpy(m5c_want.px[0][1], c1, sizeof(c1));
         m5c_judge_bytes(m, [out contents], &m5c_want, "kernel-descriptors-in-a-local", false);
      }
   }
   /* a table of 4 weights a texel, 3x64 as VideoToolbox's */
   {
      static float table[64][3][4];
      for (unsigned y = 0; y < 64; y++)
         for (unsigned x = 0; x < 3; x++)
            for (unsigned k = 0; k < 4; k++)
               table[y][x][k] = (float)(y + x * 4 + k) / 64;
      id<MTLTexture> kern = m5c_texture(m, MTLPixelFormatRGBA32Float, 3, 64, 1, MTLTextureUsageShaderRead);
      [kern replaceRegion:MTLRegionMake2D(0, 0, 3, 64) mipmapLevel:0 withBytes:table bytesPerRow:48];
      const uint32_t n = 9;
      id<MTLBuffer> counts = m5c_buffer(m, &n, sizeof(n));
      memset([out contents], 0, DM_W * DM_H * 16);
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_ktb", "kernel-table"), "kernel-table");
      if (m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             [e setTexture:kern atIndex:2];
             [e setBuffer:out offset:0 atIndex:0];
             [e setBuffer:counts offset:0 atIndex:1];
          }, "kernel-table")) {
         float sum = 0;
         for (unsigned i = 0; i < 9; i++)
            sum += table[5][i >> 2][i & 3];
         m5c_want_solid(0, 0, 0, 0);
         const float c[4] = { sum, table[63][1][1], 0, 1 };
         memcpy(m5c_want.px[0][0], c, sizeof(c));
         m5c_judge_bytes(m, [out contents], &m5c_want, "kernel-table", false);
      }
   }
}

/* ---- format 540 over a 420f surface (M5e) -------------------------------------------------- */

static void
m5c_surface_540(struct m5c *m)
{
   const int w = 64, h = 64;
   NSDictionary *p0 = @{ (id)kIOSurfacePlaneWidth : @(w), (id)kIOSurfacePlaneHeight : @(h),
                         (id)kIOSurfacePlaneBytesPerElement : @1, (id)kIOSurfacePlaneBytesPerRow : @(64),
                         (id)kIOSurfacePlaneOffset : @(0), (id)kIOSurfacePlaneSize : @(w * h) };
   NSDictionary *p1 = @{ (id)kIOSurfacePlaneWidth : @(w / 2), (id)kIOSurfacePlaneHeight : @(h / 2),
                         (id)kIOSurfacePlaneBytesPerElement : @2, (id)kIOSurfacePlaneBytesPerRow : @(64),
                         (id)kIOSurfacePlaneOffset : @(w * h), (id)kIOSurfacePlaneSize : @(w * h / 2) };
   NSDictionary *desc = @{ (id)kIOSurfaceWidth : @(w), (id)kIOSurfaceHeight : @(h), (id)kIOSurfacePixelFormat : @(0x34323066),
                           (id)kIOSurfaceBytesPerElement : @1, (id)kIOSurfacePlaneInfo : @[ p0, p1 ],
                           (id)kIOSurfaceAllocSize : @(w * h * 3 / 2) };
   for (unsigned ramp = 0; ramp < 2; ramp++) {
      const char *row = ramp ? "surface-540-ramp" : "surface-540-flat";
      IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)desc);
      if (s == NULL) {
         m->refused++;
         printf("drvtest: %s: no surface\n", row);
         continue;
      }
      IOSurfaceLock(s, 0, NULL);
      uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s, 0), *uv = IOSurfaceGetBaseAddressOfPlane(s, 1);
      const size_t ys = IOSurfaceGetBytesPerRowOfPlane(s, 0), uvs = IOSurfaceGetBytesPerRowOfPlane(s, 1);
      for (int j = 0; j < h; j++)
         for (int i = 0; i < w; i++)
            y[j * ys + i] = ramp ? (uint8_t)(i * 4) : 105;
      for (int j = 0; j < h / 2; j++)
         for (int i = 0; i < w / 2; i++) {
            uv[j * uvs + i * 2] = ramp ? (uint8_t)(i * 8) : 141;
            uv[j * uvs + i * 2 + 1] = ramp ? (uint8_t)(j * 8) : 83;
         }
      IOSurfaceUnlock(s, 0, NULL);
      MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
      [d setTextureType:MTLTextureType2D];
      [d setPixelFormat:(MTLPixelFormat)540];
      [d setWidth:w];
      [d setHeight:h];
      [d setUsage:MTLTextureUsageShaderRead];
      id<MTLTexture> t = [m->dev newTextureWithDescriptor:d iosurface:s plane:0];
      id<MTLBuffer> out = m5c_buffer(m, NULL, DM_W * DM_H * 16);
      id<MTLComputePipelineState> pk = m5c_cp(m, m5c_fn(m, "m5c_k540", row), row);
      if (t != nil && m5c_dispatch(m, pk, 1, 1, ^(id<MTLComputeCommandEncoder> e) {
             [e setTexture:t atIndex:0];
             [e setBuffer:out offset:0 atIndex:0];
          }, row)) {
         /* (measured on real Metal, rows: Cr, Y, Cb, 1; centred chroma) */
         const float flat[4] = { 83, 105, 141, 255 };
         const float want[3][4] = { { 34, 40, 38, 255 }, { 34, 40, 38, 255 }, { 36, 42, 40, 255 } };
         m5c_want_solid(0, 0, 0, 0);
         for (unsigned k = 0; k < 3; k++)
            memcpy(m5c_want.px[0][k], ramp ? want[k] : flat, sizeof(flat));
         m5c_judge_bytes(m, [out contents], &m5c_want, row, false);
      } else if (t == nil) {
         m->refused++;
         printf("drvtest: %s: no texture\n", row);
      }
      CFRelease(s);
   }
}

/* ---- the run ---------------------------------------------------------------------------- */

static int
m5c_run(id<MTLDevice> dev, const char *metallib, const char *backend, bool gallium)
{
   struct m5c m = { 0 };
   NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metallib]];
   NSError *e = nil;
   if (data == nil) {
      printf("drvtest: --m5c cannot read %s\n", metallib);
      return 1;
   }
   dispatch_data_t dd = dispatch_data_create([data bytes], [data length], NULL,
                                             DISPATCH_DATA_DESTRUCTOR_DEFAULT);
   m.dev = dev;
   m.backend = backend;
   m.gallium = gallium;
   m.queue = [dev newCommandQueue];
   m.lib = [dev newLibraryWithData:dd error:&e];
   if (m.lib == nil) {
      printf("drvtest: --m5c newLibraryWithData: %s\n", e ? [[e localizedDescription] UTF8String] : "nil");
      return 1;
   }
   m.vs = [m.lib newFunctionWithName:@"m5c_vs"];
   m.target = m5c_texture(&m, MTLPixelFormatRGBA32Float, DM_W, DM_H, 1,
                          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
   static uint8_t bytes[16 * 16 * 4];
   for (unsigned k = 0; k < 3; k++) {
      m.t[k] = m5c_texture(&m, MTLPixelFormatRGBA8Unorm, 16, 16, 1, MTLTextureUsageShaderRead);
      tm_fill(bytes, k, 0, 16, 16);
      [m.t[k] replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:bytes bytesPerRow:64];
   }
   MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
   [sd setSupportArgumentBuffers:YES];
   [sd setSAddressMode:MTLSamplerAddressModeClampToEdge];
   [sd setTAddressMode:MTLSamplerAddressModeClampToEdge];
   m.near = [dev newSamplerStateWithDescriptor:sd];
   [sd setSAddressMode:MTLSamplerAddressModeRepeat];
   [sd setTAddressMode:MTLSamplerAddressModeRepeat];
   m.rep = [dev newSamplerStateWithDescriptor:sd];
   m5c_argbufs(&m);
   m5c_arrays(&m);
   m5c_stores(&m);
   m5c_three(&m);
   m5c_pack(&m);
   m5c_half_rows(&m);
   if (!gallium)
      m5c_kernels(&m);
   if (!gallium)
      m5c_surface_540(&m);
   if (!gallium)
      m5c_fetch(&m);
   /* Gallium has no specialized function, so real Metal's road is the one it would take: inside Metal
    * through a compiler this device has not got, which took 202 s to say so in run 124507. */
   if (gallium)
      dm_refused(backend, "fc-all", "the Gallium library has no specialized function (hkmetal.m)");
   else
      m5c_constants(&m);
   printf("drvtest: draw-summary %s m5c %u ok, %u differ, %u gallium-lacks, %u refused\n", backend, m.right,
          m.wrong, m.lacks, m.refused);
   return 0;
}
