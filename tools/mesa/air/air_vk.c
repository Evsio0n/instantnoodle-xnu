/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_vk: the Metal driver's contract (air_metal.h) a second time, over Vulkan.
 *
 * WHAT THIS IS. userland/src/hkmetal.m never calls Gallium: it dlopens a library and
 * looks up the rwa_* calls of air_metal.h by name. Until now that library was one,
 * libhackiosair.dylib -- air_metal.c over freedreno's Gallium driver. This file is the
 * same calls over turnip, Mesa's Vulkan driver for the same Adreno, and it is linked
 * INTO turnip's own library (libvulkan_freedreno.dylib, src/freedreno/vulkan/meson.build
 * on Darwin) so that the AIR frontend, NIR and turnip are one copy in one image: the
 * frontend makes NIR with turnip's own compiler options (tu_hackios.cc) and turnip
 * takes that NIR through the Vulkan runtime's door for it,
 * VkPipelineShaderStageNirCreateInfoMESA (src/vulkan/runtime/vk_pipeline.h), the one
 * Mesa's own vk_meta.c draws its clears and blits through. No SPIR-V is made anywhere.
 * hkmetal.m picks this library with the boot-arg metal_backend=vk or the environment
 * variable HK_METAL_BACKEND=vk; the rwa_* names are the same, so nothing else in the
 * driver changes.
 *
 * WHAT IT DOES. Milestone M3a (verified on the phone, run 012447) was the smallest Metal
 * draw: a device, buffers, 2D textures, a read-back, a clear, and a render pipeline whose
 * stages bind nothing, over the generated full-screen vertex stage. M3b (verified on the
 * phone, runs 020036 and 020900) added RESOURCES: `constant` and `device` buffers, sampled
 * textures and samplers (the
 * application's and the shader's own `constexpr sampler`s) in either stage, filled with
 * push descriptors at each draw; texture writes; the blit encoder's copies and fills;
 * and rwa_module_textures, which metal_drv_test --draw-all asks before it binds. M3c-1
 * (verified on the phone, run 023037) added 3D TEXTURES (MTLTextureType3D, see the section
 * of that name below): made, written, read, copied, cleared and sampled as a 2D texture
 * is, with one level and one sample. M3c-2 (verified on the phone, runs 030433 and
 * 030853) added DEPTH AND STENCIL (the
 * section of that name below): the four depth/stencil formats air_metal.c maps, a depth
 * attachment in a render pass with its clears, MTLDepthStencilState, fragment shaders
 * that store [[depth]], depth textures sampled as depth2d, copied, cleared and read back
 * by aspect (rwa_depth_read). M3c-3 (verified on the phone, run 034547) added MULTISAMPLE
 * (the section of that
 * name below): MTLTextureType2DMultisample textures of 2 and 4 samples, colour and depth,
 * multisample colour and depth attachments in a pipeline of that sample count, the
 * resolve at the end of a pass (rwa_resolve_texture, through dynamic rendering's resolve
 * attachments), and shader reads of texture2d_ms and depth2d_ms. M3d (built and run on the
 * phone, runs 043116 and 043536; see below) added COMPUTE (the section of that name
 * below): rwa_compute_create, rwa_dispatch and rwa_dispatch_buffers over a Vulkan
 * compute pipeline made from the same NIR, one per threadgroup size; a kernel's buffers,
 * sampled textures and samplers through the same push-descriptor lowering a draw uses, in
 * a binding range of their own; textures a kernel writes (storage images); threadgroup
 * memory; and the barriers that order a dispatch against everything around it. What is
 * still refused, each by name and with "(M3b)" in the sentence so a log can be searched
 * for all of them at once: storage images (a texture a shader writes), argument buffers,
 * vertex attributes and index buffers, multisampling, blending,
 * texture types other than 2D and 3D, mip chains, compute. [M3c-3: multisampling is no
 * longer in that list, and a MTLTextureType2DMultisample is a type this back end makes.]
 * [M3d: nor is compute, and a kernel's storage images are bound; a texture written by a
 * FRAGMENT or VERTEX shader is still refused with "(M3b)". What Vulkan or turnip itself
 * cannot do for a kernel says "(M3d)": COMPUTE below lists each.]
 * [M5c: nor are a texture a fragment or vertex shader writes, and argument buffers: ARGUMENT
 * BUFFERS, at resolve_binding.]
 * [M3d was built and run on the phone, runs 043116 and 043536, metal_drv_test
 * --compute-all over the corpus's kernels on both back ends: 45 of the 52 kernels came back
 * the same as on Gallium (air_metal.c). Of the other seven, six were refused by this back
 * end alone -- MetalFaceMask_DilateCubeLuma, _DilateCubeChroma1, _DilateCubeChroma2,
 * _ErodeCubeLuma, _ErodeCubeChroma1 and _ErodeCubeChroma2 in PortraitFilters.cifilter's
 * default.metallib, each sampling a texture3d with its own sampler(coord::pixel), which
 * Vulkan has no unnormalised sampler for -- and Gallium ran them (Dilate* t1
 * 5ecb9ef8b9c36895, Erode* t1 891419c82f0d6315). The seventh, fbs_pcg_iter2, differs from
 * run to run on BOTH back ends: it adds into global memory with atomics from many threads,
 * so its result is not one number on either, and the difference is not this file's.]
 * [M3d-2, this version: those six. A shader's own sampler in pixel coordinates that samples
 * a texture3d is made a NORMALISED Vulkan sampler with the same filter and address modes,
 * and every coordinate sampled through it is multiplied by 1 / textureSize(level 0) in the
 * NIR, with no half-texel offset (3D TEXTURES IN PIXEL COORDINATES, at
 * pixel_to_normalized). The 2D pixel-coordinate path is untouched, and an API sampler in
 * pixel coordinates with a texture3d is still refused by name, now with "(M3d)": its kind
 * is first known at the draw, after the translation. And rwa_device_open now says once a
 * process which source the library was built from ("air_vk: built from air_vk.c of
 * <__TIMESTAMP__>, compiled <__DATE__> <__TIME__>"), so a log tells a stale build apart.]
 * [M3d-2 was verified on the phone, run 050912: sysvals and sample3d read back the host's
 * values on both back ends.]
 * [M3e, this version: rwa_texture_from_surface is real -- an IOSurface's memory as a 2D
 * texture, zero copy, which is what a CAMetalLayer drawable is (TEXTURES OVER AN IOSURFACE,
 * below). No call answers "(M3b)" for it any more; what an import cannot be says "(M3e)":
 * a YCbCr (two-plane) format, a depth format, and a device without the three import
 * extensions. Verified on the phone, run 054947: imports, clears and copies the same as on
 * Gallium, BGRA8Unorm and RGBA16Float, strides that are not width x bytes; RGBA16Float at 832
 * a row refused on both.]
 * [M3e, runs 060507 and 061800: a draw into an imported surface read back by the processor
 * showed whole 64-byte lines of the old picture, on this back end alone. The cause was the
 * processor's own clean lines, which the kernel drops at a submission only for a buffer the
 * program has mapped, or after a seed change or 4 ms -- and Gallium maps it, turnip did not.
 * An imported surface is now mapped once at import (THE SURFACE MARKED "SEEN", in
 * texture_of_shape). Tried and without effect there, and removed: a full barrier with a
 * wait-for-idle before the host barrier at the end of every command buffer
 * (AIR_VK_HOSTFLUSH=wfi, run 061800).]
 * [M4-1, this version, built and NOT yet run on the phone (out/rc.jobs.m4fix): THE GPU FAULTS
 * OF RUN 071937, and a second thread recording, which is what the code and that log show and
 * which the phone has still to confirm as the whole of it.
 * metal_drv_test --time on this back end faulted the GPU's SMMU in four processes of five,
 * each time right after 256 EMPTY command buffers were committed and dropped, and never on
 * Gallium. An empty Metal command buffer records nothing, so rwa_flush handed out no fence,
 * and hkmetal.m's -hkWaitFence then waits with rwa_finish instead -- on its completion queue
 * ("hk.metal.completion"), with only its own rw_pipe_lock held. rwa_finish was
 * submit_and_wait: it BEGAN a command buffer through device->current, recorded a barrier and
 * submitted it, all on that second thread, while the thread the contract belongs to went on
 * recording through the same `current`, the same VkCommandPool and the same VkQueue -- every
 * one of them externally synchronised in Vulkan -- in calls rw_pipe_lock does not cover
 * (-getBytes:, -replaceRegion:, and the wait every destroy began with). During the loop the
 * two were kept apart by that lock (-commit takes it too); the first case after it,
 * tm_present's -getBytes:, is where the backlog of up to 256 waits met the main thread's
 * recording, and it is where every fault came: the log has that case's reads and those waits
 * failing side by side. The same second thread reached the recording side a second way:
 * a command buffer released last on the completion queue releases what it held, and every
 * destroy began with idle(), which was submit_and_wait as well. Gallium never takes either
 * road: freedreno's flush always hands out a fence (its last one, when nothing was drawn), so
 * -hkWaitFence never falls back, and a Gallium destroy records nothing. The fix is here and
 * not in hkmetal.m, and it is the rule ONE COMMAND BUFFER AT A TIME now states: only the
 * thread that began a recording records into it or submits it. rwa_flush with nothing recorded
 * hands out the newest submission's fence (device->last, counted, freedreno's last_fence), so
 * an empty command buffer costs no submission at all; rwa_finish and every destroy wait for
 * that same fence when the calling thread is not recording (wait_for_all), and record and
 * submit only when it is; and a pair goes back to the pool only when its last holder lets go,
 * to be reused only once its fence has signalled, as before. rwa_device_close waits for the
 * device to be idle before it destroys anything, and the library prints one line at process
 * exit ("air_vk: exit: pending-pairs N in-flight M") with what was still on the GPU. Not
 * settled by this: the syndromes (0x30002, 0x30012) read as level-2 translation faults by the
 * SMMU's own layout, a 2 MB region the tables walked never had, and hk_adreno.c clears only
 * level-3 entries, so a BO freed too early in its own program would not fault that way. A
 * command stream two threads wrote can name any address; but if --burst still faults with this
 * fix, look next at the kernel's table switch (ring_table), not here.]
 * [That M4-1 fix was verified on the phone, run 080405 (libvulkan_freedreno 9f625b63388cc528):
 * --time on this back end faulted in 0 processes of 5, against 4 of 5 before; --burst 256 ok on all
 * 6, no recovery and no device lost; nothing in flight at any process's end; the imported-surface
 * draws still right.]
 * [M4-1, THE COST PART, this version, built and NOT yet run on the phone (out/rc.jobs.m4r1): vk's
 * GPU time was 20-43% under Gallium's in steady drawing (run 071517) and its processor's system
 * time far over it, for three reasons read from the code, and these are the three answers:
 *   every wait was a busy poll, on a clock the kernel does not go by: not this file's. The Mesa
 *     tree's tu_knl_drm.h and tools/mesa/hackios-drm/hk_drm.c's fence_wait, deadlines from
 *     mach_continuous_time() itself, with a back-off as the last guard;
 *   every texture write and destroy was a submission and a wait of its own, 7-40x Gallium's
 *     submissions: UPLOADS (an upload command buffer that goes ahead of the recording at the
 *     next flush, in one submission with it) and when_done (a destroy goes when the fence of the
 *     last work that named it has signalled: reap, one mechanism for pairs, objects and the ring);
 *   a staging buffer was made, mapped and closed per call: THE STAGING RING.
 * The contract does not change, and no export does. What is still waited for: a read-back; a write
 * with no rwa_device_defer_waits or over an IOSurface (the old promise, there is a reader who
 * cannot be told); rwa_finish; rwa_device_close; and a full ring. metal_drv_test --order checks the
 * orders Metal promises on both back ends.]
 * [M5a, this version, built and NOT yet run on the phone (out/rc.jobs.m5a*): THE DRAW PATH, its
 * section below. Everything drawn on this back end until now was a full-screen quad, opaque,
 * with no vertex input and no index buffer. Now: blending, every factor and operation Gallium
 * serves plus the blend colour, per attachment, and sRGB; [[stage_in]] with every vertex format
 * and step function Gallium serves; indexed draws, 16 and 32 bits, base vertex, base instance,
 * instances, and primitive restart as Metal has it; cull mode and winding; a viewport with an
 * origin and a scissor; per-attachment clears. "(M3b)" no longer answers any of them, and what
 * Vulkan or turnip cannot do says "(M5a)". The contract grew seven fields at the end of
 * rwa_draw (air_metal.h) and no export: 48, as before.]
 * [M5a was verified on the phone, runs 094931-095811: 126 of 140 blend pictures ok and the other
 * 14 EQUAL to Gallium's digest for digest, which the model in draw_model.h now reproduces whole
 * (it quantizes first, as the hardware does); every vertex format, index, restart, viewport,
 * scissor, orientation and pass case right against the CPU model.]
 * [M5b, this version, built and NOT yet run on the phone (out/rc.jobs.m5b*): TEXTURES, LEVELS AND
 * SLICES, its section below. 1D, 1DArray, 2DArray, Cube, CubeArray and 2DMultisampleArray beside
 * the three there were; mip chains on all of them, a level and a slice in every call that takes
 * one and in a pass's attachments; rwa_generate_mipmaps; texture views and their swizzle, through
 * one new symbol (49 exports); the border colours; the formats air_metal.c maps that this one did
 * not, and those it refuses that Vulkan has (the packed 32-bit ones, ETC2, EAC, ASTC). "(M3b)" and
 * "(M3c)" no longer answer any of them, and what Vulkan or turnip cannot do says "(M5b)".]
 * What M3c has reached and not
 * finished says "(M3c)" instead: rendering into a 3D texture, a sampler in pixel
 * coordinates on one [M3d-2: the shader's own is served, an API one says "(M3d)"], a
 * buffer copy whose bytesPerImage is not whole rows, a copy between
 * a depth format and any other format, a fragment shader's [[stencil]] store, and a depth
 * attachment on a device without Vulkan 1.3's dynamic depth/stencil state; and, since
 * M3c-3, a sample count turnip does not offer for a format, a multisample array, a resolve
 * into a 3D texture or between depth textures that were not made render targets, and a
 * depth resolve on a device without SAMPLE_ZERO depth resolves. A refusal
 * is the honest answer for
 * the reason the whole contract gives: a draw that binds a texture this library ignored
 * would come back as a picture, and a picture that is wrong reads like a result.
 *
 * THE NIR PATH, in full:
 *
 *   air_module_function_constants + air_to_nir      the frontend, as air_metal.c calls it,
 *     with tu_hackios_nir_options(VkDevice)          but with TURNIP's options
 *   NOT finalize_for_gallium                          turnip lowers system values and I/O
 *                                                     itself (tu_spirv_to_nir runs
 *                                                     nir_lower_system_values; ir3 runs
 *                                                     nir_lower_io in ir3_nir_post_finalize,
 *                                                     with interpolated inputs, so the
 *                                                     flat-varying trap finalize_for_gallium
 *                                                     documents does not arise here)
 *   shape_like_spirv                                  the few passes vk_spirv_to_nir runs
 *                                                     on every SPIR-V shader before turnip
 *                                                     sees it, because a NIR stage skips
 *                                                     them (vk_pipeline_shader_stage_to_nir
 *                                                     returns the clone untouched)
 *   lower_stage                                       AIR's Gallium-style resource indices
 *                                                     in the form SPIR-V's are in by then
 *                                                     (see RESOURCES below)
 *   VkPipelineShaderStageNirCreateInfoMESA            in each VkPipelineShaderStageCreateInfo,
 *     module = VK_NULL_HANDLE, pName = "main"         the frontend's entry point is "main"
 *   vkCreateGraphicsPipelines + VkPipelineRenderingCreateInfo (dynamic rendering, no
 *     VkRenderPass), with one push descriptor set in the pipeline layout
 *
 * The frontend hands the fragment [[position]] over as a SYSTEM_VALUE_FRAG_COORD
 * variable, which is exactly the form spirv_to_nir gives FragCoord (vtn_variables.c), so
 * turnip's own nir_lower_system_values makes the intrinsic ir3 wants; and its I/O is
 * variables with VARYING_SLOT_VAR0 + n locations, which is the form turnip links stages
 * by.
 *
 * RESOURCES, which is the whole of M3b and the part to read before changing any of it.
 *
 * The frontend speaks Gallium: a buffer access is load_ubo/load_ssbo/store_ssbo/
 * ssbo_atomic with an IMMEDIATE block index (Metal's [[buffer(n)]]), and a texture access
 * is a nir_tex_instr whose texture_index and sampler_index are Metal's [[texture(n)]] and
 * [[sampler(n)]]. Turnip lowers only the Vulkan form (tu_shader.cc, tu_lower_io:
 * lower_vulkan_resource_index, lower_load_vulkan_descriptor, lower_ssbo_ubo_intrinsic --
 * which leaves a load whose index has ONE component alone, as a driver-param UBO -- and
 * lower_tex, which reads texture and sampler DEREFS). lower_stage below rewrites the one
 * into the other, into exactly what nir_lower_explicit_io(vec2_index_32bit_offset) makes
 * of a SPIR-V buffer access, because tu_lower_io runs after that pass and sees only its
 * output:
 *
 *   buffer   index = vec2 of load_vulkan_descriptor(vulkan_resource_index(0, set 0,
 *            binding B)), and the offset plus its third component (0, once turnip has
 *            lowered it) -- the form tu_lower_io turns into a bindless handle
 *   texture  a texture_deref of a nir_var_uniform of a texture type, and a sampler_deref
 *            of one of the bare sampler type, each with descriptor_set 0 and binding B;
 *            texture_index and sampler_index then 0
 *
 * SEPARATE images and samplers (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE and _SAMPLER) and not
 * combined ones, because that is Metal's model: any [[texture(n)]] may be sampled with
 * any [[sampler(m)]], chosen per instruction, and a combined descriptor would need one
 * binding per pair the shader happens to use. Turnip serves both kinds the same way
 * (build_bindless: a separate sampler is its own descriptor, a combined one is the second
 * half of one), so nothing is lost.
 *
 * ONE PUSH DESCRIPTOR SET, set 0, for both stages, and not one set per stage: Vulkan
 * allows at most ONE push set in a pipeline layout (VUID-VkPipelineLayoutCreateInfo-
 * pSetLayouts-00293). The two stages are told apart by binding ranges, and Metal's three
 * index spaces (nir_air.h: a buffer, a texture and a sampler are numbered independently,
 * so buffer 0 and texture 0 coexist) by an offset per kind:
 *
 *   binding = stage (fragment 0, vertex 256) + kind (buffer 0, texture 32, sampler 160)
 *             + Metal's own index (buffers 0..30, textures 0..127, samplers 0..31 --
 *               Metal's 16 and the shader's own constexpr samplers, which the frontend
 *               numbers after them)
 *
 * [M3d: and compute 512, in a pipeline layout of its own (COMPUTE, THE BINDINGS); a
 * texture a kernel writes is a STORAGE_IMAGE at the same texture range.]
 *
 * Binding numbers may be sparse; what Vulkan bounds is the NUMBER of descriptors in a push
 * set, maxPushDescriptors (32 on turnip, printed at device open). [M5c: a pipeline whose two
 * stages bind more than that between them takes an ordinary set (ARGUMENT BUFFERS, MORE THAN A
 * PUSH SET HOLDS). This said it was refused by name, as it was until then.] Nothing is ever
 * dropped to make it fit.
 *
 * A `constant` buffer is a UNIFORM_BUFFER when its metadata declares a size
 * (air.buffer_size) no larger than maxUniformBufferRange AND every load from it is at a
 * constant offset inside that size; otherwise it is a STORAGE_BUFFER read as non-writeable.
 * The second is never wrong and the first is faster (ir3 may push a UBO into constant
 * registers), and the condition is what makes the UBO's range -- the declared size --
 * provably enough: Metal has no buffer bounds, and a UBO range that cut a read short would
 * read zeros where the buffer has numbers. A `device` buffer is a STORAGE_BUFFER of the
 * whole rest of the buffer (VK_WHOLE_SIZE from the offset).
 *
 * OFFSETS. Metal's -setFragmentBuffer:offset: allows any multiple of 4; Vulkan's
 * descriptors want minUniformBufferOffsetAlignment (64 here) and
 * minStorageBufferOffsetAlignment (4). A misaligned offset is NEVER bound as it is, and
 * the fallback is the one air_metal.c has on Gallium, made deterministic: a buffer the
 * shader only reads is COPIED, on the GPU and in order with the work around it
 * (vkCmdCopyBuffer before the pass), into an aligned piece of an upload buffer the command
 * buffer owns, and bound there; a buffer the shader writes is refused by name, because
 * the writes would land in the copy and be thrown away. The other fallback (bind the
 * aligned-down base and add the remainder in the shader) was not taken: it would put a
 * run-time offset into every UBO load of every pipeline, aligned or not, which costs ir3
 * its constant-register pushes everywhere to serve a case that is rare.
 *
 * SYNCHRONISATION is explicit and wide, and does not lean on the queue's order: before a
 * pass, every earlier write is made visible to every later access (a global memory
 * barrier), every texture the pass samples is moved to SHADER_READ_ONLY_OPTIMAL and every
 * attachment to COLOR_ATTACHMENT_OPTIMAL with barriers whose source scope is all earlier
 * writes; every blit moves its images to TRANSFER layouts the same way; and every command
 * buffer ends with a barrier that makes the GPU's writes visible to the processor, which
 * is what a Metal buffer read after -waitUntilCompleted relies on.
 *
 * THE VERTEX STAGE THIS LIBRARY GENERATES is air_metal.c's quad_vertex_shader with one
 * difference, and the difference is the reason it needs no vertex buffer: the four
 * corners come from the vertex index (bit 0 is x, bit 1 is y) instead of from a buffer
 * of four vec4s, so the pipeline has no vertex input at all. The corners are the same
 * ones in the same order -- (-1,-1) (1,-1) (-1,1) (1,1), a TRIANGLE_STRIP of four
 * covering the whole attachment -- and every varying the fragment shader reads is the
 * same constant coordinate air_metal.c hands it, the centre of texel (1, 1) of a 4x4.
 *
 * Y, which is the one convention Vulkan and Gallium do not share. Metal's normalised y
 * points UP and its render target's row 0 is the TOP; Vulkan's clip y points DOWN onto
 * the same top-left origin. air_metal.c gives Gallium a negative y scale for exactly this
 * reason (its own long comment at set_viewport_states), and this file gives Vulkan the
 * same thing in Vulkan's words: a viewport whose height is negative (y = h, height = -h,
 * core since Vulkan 1.1). Clip y = +1 lands in row 0 on both back ends (verified on the
 * phone in M3a: fragPos read (0.5, 0.5) at (0, 0) and (3.5, 3.5) at (3, 3) on both).
 *
 * THE DRAW PATH (M5a), which is the part of a Metal draw that is not a shader. The functions
 * are BLENDING and VERTEX INPUT at blend_state_from_metal and build_vertex_state; what they
 * refuse is listed there. The rest, which is all in pipeline_for and rwa_render_draw:
 *
 *   ORIENTATION: why the winding is NOT reversed, although the viewport's height is negative
 *   and that looks as if it ought to reverse it. Metal's winding is what is seen on the
 *   attachment: x right, row 0 on top, and clip y = +1 in row 0, so a triangle that is
 *   counter-clockwise in Metal's clip coordinates (y up) is counter-clockwise as drawn.
 *   Vulkan computes a triangle's face AFTER the viewport, in framebuffer coordinates, whose y
 *   runs down: a = -1/2 * sum(x_i * y_(i+1) - x_(i+1) * y_i), and front when a > 0 for
 *   VK_FRONT_FACE_COUNTER_CLOCKWISE. Take the corners (0,10) (10,10) (5,0) of a framebuffer,
 *   bottom left, bottom right, top: counter-clockwise as seen. The sum is -100, a is +50, so
 *   that is what Vulkan means by counter-clockwise too. Both name what is drawn, so the two
 *   enumerations map straight across and the negative height has already been accounted for
 *   by being applied first: a positive height would have drawn the picture upside down and
 *   reversed every winding with it, which is the mistake this could be taken for. Turnip
 *   writes GRAS_SU_CNTL.front_cw from frontFace with no reference to the viewport's sign, as
 *   Gallium writes it from front_ccw (fd6_rasterizer.cc), and Gallium's zeroed rasterizer
 *   state means the register's value for Metal's default, clockwise: the same register value
 *   as here, and the one thing this ever changed for a draw that culls nothing is that a
 *   clockwise default is now said, where this file used to say counter-clockwise and rely on
 *   the culling being off. [[front_facing]] and the two faces of a stencil test read it.
 *
 *   THE RECTANGLES: Metal's viewport is y_window = originY + height/2 * (1 - y_ndc), and
 *   Vulkan's with y = originY + height and a height of -height is that exactly (rwa_render_draw).
 *   The scissor is the attachment's whole unless asked: Metal's default is NOT the viewport.
 *   Turnip also cuts to the viewport (GRAS_SC_VIEWPORT_SCISSOR, tu6_emit_viewport), which is
 *   what a triangle's clipping would come to anyway; only a wide point or line straddling
 *   the viewport's edge can tell, and nothing here draws one on purpose.
 *
 *   [source: Apple has no statement of this that was found; two third parties agree. Dawn's commit b632bc58, "Vulkan:
 *   Don't enable primitiveRestart on list topologies", had it on unconditionally to match Metal and then only on strips,
 *   since on lists it does nothing (git.axiodl.com/encounter/dawn-cmake/commit/b632bc58ed373eb2020af65e0fdeaf07d46d1a9c);
 *   a CodeWeavers vkd3d test patch says Metal and MoltenVK cannot turn restart off (list.winehq.org, wine-gitlab,
 *   message IVYIDZRHGHKDC7ZVJMEY64TXIOYJYHEA). So Gallium's restart being off is the deviation.]
 *   INDEXED DRAWS: Metal's restart is always on for the two strip types and not there for the
 *   lists, and pipeline_for follows it per topology. air_metal.c leaves Gallium's
 *   primitive_restart off, and its comment reads Metal the other way; it is the back end that
 *   differs here, and one drawn with a restart index is refused or wrong there, which is what
 *   --draw-index --pixels prints against the model and not against this.
 *   [[vertex_id]] is nir_load_vertex_id, which is VertexIndex and already holds the base
 *   vertex on a6xx (ir3's vertex_id_zero_based is false there; VFD_INDEX_OFFSET is added by the
 *   hardware) and [[instance_id]] is the frontend's load_instance_id + load_base_instance,
 *   which is what spirv_to_nir makes of InstanceIndex and turnip fills in vs_params; so
 *   both come out with Metal's base included and neither needs anything from this file.
 *
 *   INDIRECT DRAWS, DEPTH BIAS, DEPTH CLAMP, FILL MODE: Gallium has none of these (hkmetal.m
 *   declines the indirect forms, drops -setDepthBias: and -setTriangleFillMode:, and says so
 *   once for MTLDepthClipModeClamp), so there is no reference for them yet and the contract
 *   does not carry them. Turnip has them all; it is the contract, not Vulkan, that stops them.
 *
 *   PASS STATE: every draw is a Vulkan pass of its own with its own barriers, as it has been
 *   since M3a: what makes several encoders in one command buffer ordered is barrier_all and
 *   image_barrier at the head of each, and a texture drawn into by one and sampled by the
 *   next is moved between the two layouts by the same. Load is LOAD, and Clear is CLEAR, per
 *   attachment since M5a (rwa_draw.color_clears). DontCare at the start of a pass arrives as
 *   a keep and at the end as a store, which Metal allows and hkmetal.m has always done.
 *
 * TEXTURES, LEVELS AND SLICES (M5b), which is air_metal.c's texture_of_shape, texture_box and
 * rwa_generate_mipmaps in Vulkan's words, and the rest of what Metal's texture object is that
 * Gallium's library never had a call for: a view, a border colour, a pass into a level.
 *
 *   THE IMAGE. A texture is one VkImage and every Metal type is one Vulkan one: 1D and 1DArray a
 *     VK_IMAGE_TYPE_1D, 2D, 2DArray, Cube and CubeArray a 2D (a cube is six layers, a cube array
 *     six per cube, and a slice is Metal's slice: 6 * cube + face), 2DMultisample and its array a
 *     2D of N samples, and 3D a 3D whose depth stays in its extent. The refusals are air_metal.c's
 *     (texture_of_shape), in its sentences, and then turnip is asked for what it makes of this
 *     format, type and usage (vkGetPhysicalDeviceImageFormatProperties: maxMipLevels,
 *     maxArrayLayers, maxExtent), which is what vkCreateImage would be held to.
 *   THE LAYOUT OF ONE IMAGE, which is the decision the rest hangs on. This file tracks one layout
 *     per image and not one per level and slice: every barrier covers the whole image, as one did
 *     before there was more than one level to cover. It is more than a pass needs, and it is what
 *     the order of things stands on (UPLOADS has an upload moving "the image" ahead of a recording
 *     that may have named it), so it stays: a mistake there is a torn picture, and a barrier on
 *     more of an image than it had to be is a cost nobody has measured here. Two subresources of
 *     one image used at once in two ways are in GENERAL together: a level copied into another
 *     (rwa_copy_texture), the chain of rwa_generate_mipmaps, and a pass that renders into one
 *     level of an image while sampling another through a view (rwa_render_draw). The same level
 *     and slice both ways is the old refusal.
 *   ITS VIEWS. A texture has a view of everything it has, of the type its shader's uniform is,
 *     for sampling; a view of one level (`store_view`) for a kernel's storage image, because Vulkan
 *     wants one level there; and a view of one level and one layer per (level, slice) a pass has
 *     rendered into (att_view_for), kept until the texture goes. A 3D image renders one depth plane
 *     through a 2D view, which needs the image made VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, so it
 *     is made so when it is a render target. Rendering through a view of a texture
 *     (rwa_texture_view_create) takes a view with the identity swizzle whatever the view's own
 *     is, because Vulkan requires one: Metal's swizzle is a sampling matter.
 *   rwa_texture_view_create is a second rwa_texture over the first's image, with `root` naming
 *     the one that owns the image, its memory, its layout and its record of use. Everything those
 *     things are read from goes through root; and every call that takes a level or a slice adds
 *     the view's base. The image goes with the last texture over it (refs). Metal's rule about the
 *     format is the Vulkan one: another format of the same texel size, on a texture made with
 *     MTLTextureUsagePixelFormatView, which is what makes the image MUTABLE_FORMAT here. That bit is
 *     not set unasked, because turnip can then refuse UBWC for the image (tu_image.cc,
 *     ubwc_possible), so an sRGB twin without it is refused by name with the rest. Metal's rule about
 *     the type is its own table: 1D with 1DArray, 2D with 2DArray and the cubes, and so on.
 *     A swizzle is VkComponentMapping, table for table (MTLTextureSwizzle runs Zero, One, Red.. and
 *     VkComponentSwizzle IDENTITY, Zero, One, R..), on the sampled view alone.
 *   THE PROCESSOR. A level and a slice in VkBufferImageCopy, as texture_box says: origin[2] and
 *     size[2] are z on a 3D and nothing else, the slice is `layer`; a 1D texture has one row. A
 *     COMPRESSED format is a block of bytes per block of texels: an origin on a block and a size of
 *     whole blocks or to the edge of the level (Metal's rule, and Vulkan's for the copies), a stride
 *     in bytes of a row of blocks. So `row` and the row count come from texture_region.
 *   THE MIPMAPS. rwa_generate_mipmaps is what util_gen_mipmap is to the Gallium library: each level
 *     from the one above with a linear filter, in order, every slice and for a 3D texture every
 *     depth. Here each is a vkCmdBlitImage, in GENERAL, with a memory barrier between levels. A
 *     format is judged by what Vulkan requires of the blit (BLIT_SRC, BLIT_DST, and
 *     SAMPLED_IMAGE_FILTER_LINEAR for the filter): an integer or a depth format is refused by name,
 *     where Metal's own rule is the same pair of words, "color-renderable and color-filterable".
 *   SAMPLERS. MTLSamplerAddressModeClampToBorderColor reads rwa_sampler_state.border_color, which
 *     the contract grew for it; clamp_to_zero stays Vulkan's transparent black. Vulkan has one
 *     border colour in a sampler, of one type, so two refusals: clamp_to_zero on one axis and an
 *     opaque border on another (the one colour cannot be both), and a sampler that reads an
 *     integer and a float texture in one stage through an opaque border. An integer texture
 *     reads VK_BORDER_COLOR_INT_* (turnip packs each builtin once for its own type,
 *     tu6_pack_border_color, and a float colour's int fields are not 1), which is a third
 *     VkSampler in struct rwa_sampler, made only for such a sampler. Vulkan's rule for a sampler in
 *     pixel coordinates (one level, one layer on the view) is kept by binding a one-level view with
 *     it (base_view), which is the level it reaches anyway. A shader's own sampler reads the
 *     word's border (air_decode_sampler_state, FIELD(56, 2)), which air_metal.c refuses by name:
 *     it has nowhere to put it. And the LOD clamp applies at an explicit level too, as Vulkan's
 *     does, so one VkSampler serves both: Apple's MTLSampler.h says otherwise ("Clamp values are
 *     ignored for texture sample variants that specify an explicit level of detail"), and real
 *     Metal on an M1 Max clamps, level() in a fragment and a kernel and gradient2d in a kernel:
 *     [2, 2] asking 0, 1, 2, 3 reads 2, 2, 2, 2; [2, 1000] reads 2, 2, 2, 3; [0, 1] reads 0, 1, 1, 1.
 *     A11 not measured.
 *   FORMATS. At the table's comment, each with where its bit layout comes from.
 *
 * 3D TEXTURES (M3c-1), and the one thing about them that is easy to get wrong: in Vulkan
 * a 3D image's depth is NOT array layers. A MTLTextureType3D is a VK_IMAGE_TYPE_3D image
 * of extent width x height x depth with ONE layer, and one VK_IMAGE_VIEW_TYPE_3D view.
 * So every subresource range and every VkImageSubresourceLayers in this file stays one
 * level and one layer for a 3D texture exactly as for a 2D one -- layer 0 of a 3D image
 * is all of its depth, which is what a barrier or a clear over it then covers -- and a
 * region's z goes in imageOffset.z / imageExtent.depth (and srcOffset.z / dstOffset.z /
 * extent.depth of an image copy), never in baseArrayLayer. Each place a region crosses
 * the contract carries z the way air_metal.c reads it on Gallium (texture_box,
 * buffer_layout):
 *
 *   rwa_texture_write/read   origin[2] and size[2] are z and depth. The caller's memory
 *                            is images of `stride * height` bytes back to back, because
 *                            the contract carries one stride (hkmetal.m passes Metal's
 *                            bytesPerRow and drops bytesPerImage) and air_metal.c lays the
 *                            images out that way (texture_subdata with row * box.height)
 *   the buffer copies        Metal's bytesPerImage is honoured, as Vulkan's
 *                            bufferImageHeight, which counts ROWS: one that is not a
 *                            whole number of bytesPerRow is refused by name
 *   rwa_copy_texture         z and depth on both sides; a 2D side has depth 1, so a 2D
 *                            texture and one slice of a 3D one copy into each other (a
 *                            copy Vulkan 1.1 allows with extent.depth equal to the 2D
 *                            side's one layer)
 *
 * The shader side needs nothing new, and why is worth a line: the frontend makes a
 * texture3d sample a nir_tex_instr of GLSL_SAMPLER_DIM_3D (air_intrinsics.c, the "3d"
 * suffix of air.sample_texture_3d), stage_bindings keeps that dim, and lower_stage makes
 * the uniform glsl_texture_type(GLSL_SAMPLER_DIM_3D, ...) -- which is the image type
 * Vulkan requires to match the bound view's type, and the view is 3D. Two things are
 * refused by name with "(M3c)": a 3D texture as a colour attachment (the contract carries
 * no depthPlane, and a 2D view of one slice would need VK_IMAGE_CREATE_2D_ARRAY_
 * COMPATIBLE_BIT and a view per slice; air_metal.c refuses rendering into one too), and a
 * sampler in pixel coordinates sampling one (Vulkan allows unnormalizedCoordinates on 1D
 * and 2D views only).
 *
 * 3D TEXTURES IN PIXEL COORDINATES (M3d-2), which is the second of those two and is no
 * longer refused for the shader's own `constexpr sampler`. Gallium passes the sampler's
 * unnormalized_coords to freedreno as it is, and the Adreno's A6XX_TEX_SAMP_1_UNNORM_COORDS
 * takes it on a 3D view too (fd6_texture.cc) -- which is why air_metal.c ran
 * MetalFaceMask_DilateCube* and ErodeCube* and this file refused them. Vulkan's rule
 * (VkSamplerCreateInfo.unnormalizedCoordinates: a 1D or 2D view, one level, no
 * anisotropy, no comparison) is a rule of the API and not of this GPU, and turnip would not
 * enforce it; it is kept anyway, because a shader that only works by breaking valid usage
 * works on one driver. So the same sample is said in normalised coordinates instead:
 *
 *   stage_bindings       a sampler that is the shader's own, in pixel coordinates, and
 *                        samples a texture3d anywhere in its stage is `normalize`
 *   stage_own_samplers   makes it through sampler_info's pixel-coordinate rules -- one
 *                        filter, no mip filter, clamping on s and t, no comparison -- and
 *                        then with unnormalizedCoordinates off; r must clamp as well, the
 *                        one axis a 2D view never read
 *   lower_stage          multiplies every coordinate sampled through it by
 *                        1 / float(txs(texture, 0)), at pixel_to_normalized, after the
 *                        M3b rewrite of an implicit LOD to txl LOD 0, which stays
 *
 * No half-texel offset: Metal's pixel coordinate is already the texel-space coordinate
 * (texel i centred at i + 0.5) that a normalised coordinate times the size is, so a plain
 * division keeps nearest and linear on the same centres and the same weights. Texel offsets
 * stay as they are (texels in both spaces); clamp to edge in pixel space is clamp to edge
 * in normalised space, and clamp_to_zero's transparent-black border sits at the same
 * place. pixel_to_normalized has the long form of each of these. A 2D-only pixel-coordinate
 * sampler keeps unnormalizedCoordinates exactly as since M3b; an API sampler is refused at
 * the draw with "(M3d)" (resolve_binding says why).
 *
 * DEPTH AND STENCIL (M3c-2), which is air_metal.c's depth path call for call, in Vulkan's
 * words. Five things decide how it looks, and each is a place the two APIs differ:
 *
 *   FORMATS. The four air_metal.c maps, and no others: Depth16Unorm is VK_FORMAT_D16_UNORM,
 *     Depth32Float is D32_SFLOAT, Stencil8 is S8_UINT and Depth32Float_Stencil8 is
 *     D32_SFLOAT_S8_UINT -- the same bytes per aspect under both names. What each can do is
 *     asked of turnip (vkGetPhysicalDeviceFormatProperties) at rwa_texture_create and
 *     rwa_render_create, not assumed; turnip has all four as depth/stencil attachments
 *     (tu6_pipe2depth, tu_formats.cc), including S8_UINT, which freedreno's Gallium driver
 *     declines -- so a Stencil8 texture is made here and refused there, and that difference
 *     is the GPU drivers' and not this file's. Refused by name, as air_metal.c refuses them:
 *     Depth24Unorm_Stencil8 (255), which turnip has as D24_UNORM_S8_UINT but which iOS
 *     marks unavailable and this device answers NO for, and the X32_Stencil8 / X24_Stencil8
 *     views (261, 262), which come from -newTextureViewWithPixelFormat:.
 *   ASPECTS. Every Vulkan call that touches an image names an aspect, and a depth image
 *     has DEPTH, STENCIL or both where a colour one has COLOR. A texture carries its
 *     format's aspect mask (struct rwa_texture.aspects), and every barrier covers ALL of
 *     it: without separateDepthStencilLayouts (not enabled) the two aspects of
 *     D32_SFLOAT_S8_UINT are in one layout and move together. Image-to-image copies name
 *     both aspects at once, which vkCmdCopyImage allows; a buffer<->image copy may name
 *     only one, which is why rwa_depth_read copies each aspect it reads as its own region.
 *   VIEWS. Two of them for a depth texture, because Vulkan wants a different one for each
 *     use: the render pass takes a view of EVERY aspect (struct rwa_texture.zs_view), and a
 *     shader samples a view of ONE (`view`: the depth aspect, or the stencil aspect of a
 *     Stencil8 texture), which is what Metal's depth2d<float> reads -- one float, the
 *     depth. A colour texture keeps the one view it always had.
 *   THE ATTACHMENT. Dynamic rendering's pDepthAttachment and pStencilAttachment, both the
 *     same zs_view when the format has both aspects (Vulkan requires one view there), in
 *     DEPTH_STENCIL_ATTACHMENT_OPTIMAL, each with its own load action: Metal keeps
 *     clearDepth and clearStencil apart and so does rwa_draw, and air_metal.c clears only
 *     the aspects the attachment has. The pipeline is made for that format
 *     (VkPipelineRenderingCreateInfo depth/stencilAttachmentFormat, by aspect).
 *   THE STATE. MTLDepthStencilState is created once and bound at every draw, and Vulkan's
 *     depth/stencil state is pipeline state -- so it is DYNAMIC state here (core in Vulkan
 *     1.3: depth test, write, compare op, stencil test and ops; and 1.0's masks and
 *     reference), set at each draw out of the rwa_depth_stencil the caller passes. One
 *     pipeline per topology stays one pipeline per topology. A device below 1.3 has none of
 *     the first five, and a depth attachment on it is refused by name (M3c).
 *
 * There is no depth bias and no depth clamp in this contract (Metal's -setDepthBias: and
 * -setDepthClipMode: are encoder state rwa_draw does not carry, and air_metal.c's
 * rasterizer has depth_clip_near/far on and no offset), so the pipeline has
 * depthBiasEnable and depthClampEnable off -- the same state.
 *
 * MULTISAMPLE (M3c-3), which is air_metal.c's multisample path call for call -- that
 * back end has had it since docs/metal.md 10.34, the --blit check's 4-sample clear and
 * draw resolved on the phone -- in Vulkan's words. Metal spells "more than one sample"
 * twice, in the texture's TYPE and in its sampleCount, and Vulkan spells it once, in
 * VkImageCreateInfo.samples; everything below is keeping the two in step:
 *
 *   THE TEXTURE. A MTLTextureType2DMultisample is a VK_IMAGE_TYPE_2D image of N samples
 *     with a VK_IMAGE_VIEW_TYPE_2D view (Vulkan has no multisample view type: the samples
 *     are the image's). Metal's own rules are air_metal.c's refusals, sentence for
 *     sentence: a multisample type of one sample, a plain type of more, a mip chain, and
 *     MTLTextureUsageShaderWrite. WHICH counts exist is turnip's answer and not a list
 *     here: vkGetPhysicalDeviceImageFormatProperties for that format and that usage, whose
 *     sampleCounts on an a6xx are 1, 2 and 4 for a 2D optimal image of any format that can
 *     be a colour or depth attachment and is not a storage image (tu_formats.cc,
 *     tu_get_image_format_properties; 8 is a7xx's), and 1 for every other. That answer is
 *     printed once a process at rwa_device_open, by format, so a log carries it.
 *   THE PASS. A pipeline of N samples (rwa_render_shape.sample_count, which hkmetal.m fills
 *     from MTLRenderPipelineDescriptor.sampleCount -- rasterSampleCount is the same
 *     property under its newer name) has VkPipelineMultisampleStateCreateInfo
 *     rasterizationSamples N, no sample shading and no alpha-to-coverage (rwa_render_shape
 *     carries neither), and every attachment of a draw with it has to be a multisample
 *     texture of exactly N, colour and depth alike: air_metal.c's attachment_shape_fits,
 *     both directions, and Vulkan's own rule too (without VK_AMD_mixed_attachment_samples
 *     every attachment has rasterizationSamples samples). framebufferColorSampleCounts and
 *     framebufferDepth/StencilSampleCounts are asked at pipeline creation.
 *   THE RESOLVE. Metal resolves at the end of a pass (MTLStoreActionMultisampleResolve or
 *     StoreAndMultisampleResolve with a resolveTexture); this contract carries it as its
 *     own call after the pass's draws, rwa_resolve_texture, which hkmetal.m records at
 *     -endEncoding. Here it is a pass of its own that draws nothing: the multisample
 *     attachment loaded and stored, and dynamic rendering's resolveImageView and
 *     resolveMode on it. The mode is the one Vulkan requires of the format: AVERAGE for a
 *     floating-point or normalised colour format, SAMPLE_ZERO for an integer one
 *     (VkRenderingAttachmentInfo's valid usage for resolveMode) and for depth and stencil, which
 *     is the only depth/stencil resolve mode turnip offers (supportedDepthResolveModes,
 *     tu_device.cc). A colour texture that was not made a render target cannot be a
 *     resolve attachment in Vulkan, so a resolve into one -- Gallium takes it, and Maps
 *     asks for it -- is vkCmdResolveImage instead, the same resolve as a transfer. An
 *     empty pass is safe on turnip in both of its modes: in GMEM its resolves are stored
 *     unconditionally (tu6_emit_tile_store passes cond_exec_allowed false for them, so a
 *     tile with no geometry still resolves) and a load of an attachment that will be
 *     resolved is never conditional (tu_render_pass_cond_config).
 *   THE SHADER. A texture2d_ms or depth2d_ms read is a nir_tex_instr of txf_ms with an
 *     ms_index out of the frontend (air_intrinsics.c: read_texture_2d_ms and
 *     read_depth_2d_ms, with a sample index computed at run time throughout
 *     AppleMetalGLRenderer), and get_num_samples is texture_samples. stage_bindings keeps
 *     GLSL_SAMPLER_DIM_MS and lower_stage makes the uniform glsl_texture_type(
 *     GLSL_SAMPLER_DIM_MS, ...) -- a texture2DMS -- which is what Vulkan requires of a view
 *     of a multisample image; ir3 answers texture_samples out of the descriptor
 *     (emit_tex_info). Nothing new was needed there; the texture is what was missing.
 *   THE PROCESSOR. No multisample texture is written or read by the processor, which is
 *     Metal's rule (one cannot be MTLStorageModeShared on this device) and air_metal.c's
 *     refusals: rwa_texture_write, rwa_texture_read, rwa_depth_read and both buffer<->
 *     texture copies refuse one by name. A texture-to-texture copy of the same sample
 *     count is allowed, as on Gallium; of two different counts it is a resolve, and is
 *     refused for that reason.
 *
 * COMPUTE (M3d), which is air_metal.c's compute path call for call -- rwa_compute_create,
 * compute_state_for and rwa_dispatch_buffers there, each checked kernel by kernel against
 * softpipe (tools/air/metalcheck.py, docs/air.md) -- in Vulkan's words. Seven things decide
 * how it looks:
 *
 *   THE PIPELINE. rwa_compute_create is the render pipeline's front half for one stage:
 *     air_module_function_constants and air_to_nir with turnip's options, Gallium's two
 *     sentences for a module that is not a kernel, shape_like_spirv, and then the
 *     RESOURCES path unchanged -- stage_bindings, stage_own_samplers, lower_stage -- and the
 *     push set's layout. It compiles NOTHING, as air_metal.c compiles nothing there: Metal
 *     names the threadgroup size at the dispatch, and Vulkan fixes it in the shader
 *     (LocalSize, which is nir->info.workgroup_size here) exactly as both Gallium drivers
 *     do. So a VkPipeline is made per size at the first dispatch that asks for it
 *     (compute_pipeline_for), from a clone of the lowered NIR with workgroup_size set and
 *     workgroup_size_variable cleared -- compute_state_for's clone, for its reason: the size
 *     folds into the code (load_workgroup_size becomes a constant), so the translation has
 *     to stay untouched for the next size. One difference, and it is Vulkan's: a Gallium
 *     state may be deleted while a batch still holds it, a VkPipeline may not be destroyed
 *     before the GPU is done with it, so replacing one costs a wait for idle. Up to
 *     RWA_COMPUTE_SIZES sizes are therefore KEPT per pipeline, and only past that is the
 *     oldest replaced, after that wait.
 *
 *   THE WAVE. VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT on every
 *     compute stage (with subgroupSizeControl, core in 1.3), because it is the one setting
 *     under which turnip makes ir3 the same promise Gallium makes it: IR3_SINGLE_OR_DOUBLE
 *     for both the API's wave size and the real one (tu_shader_key_subgroup_size; freedreno's
 *     ir3_shader_compute_state_create passes exactly that). Without the flag turnip asks for
 *     double waves only, and a kernel that reads [[threads_per_simdgroup]] or shuffles would
 *     be compiled to a different contract on the two back ends. What a driver is told:
 *     -threadExecutionWidth is minSubgroupSize (64 on an a6xx: a wave is 64 or 128 threads,
 *     and 64 divides both), where Gallium says 32 (freedreno's PIPE_COMPUTE_CAP_SUBGROUP_SIZES,
 *     a TODO in Mesa beside the number); -maxTotalThreadsPerThreadgroup is
 *     maxComputeWorkGroupInvocations but never more than 1024, Metal's own ceiling and
 *     Gallium's answer, so an application sizes its groups the same on both.
 *
 *   THE BINDINGS. A kernel's are a range of their own in the one push set's numbering:
 *     binding = RWA_BIND_COMPUTE_BASE (512) + kind + Metal's index, the RESOURCES scheme
 *     with a third stage. A compute pipeline has a layout of its own, so 0 would have
 *     served; a range of its own makes a binding number say which stage it is wherever it
 *     is printed. maxPushDescriptors (32 here) bounds the kernel's bindings together -- its
 *     buffers, textures, API samplers and its own `constexpr sampler`s -- and a kernel that
 *     declares more is refused by name at rwa_compute_create (M3d): a second, ordinary
 *     descriptor set would serve it, and is not here. Nothing is dropped to make it fit.
 *
 *   STORAGE IMAGES. A texture a kernel WRITES (AIR_RESOURCE_IMAGE: access::write and
 *     access::read_write) is a VK_DESCRIPTOR_TYPE_STORAGE_IMAGE in VK_IMAGE_LAYOUT_GENERAL,
 *     at its [[texture(n)]] in the texture range (Metal has one index space for both kinds,
 *     and a slot is one or the other). The frontend gives its accesses Gallium's form --
 *     image_load, image_store, image_size, image_samples with an IMMEDIATE index -- and
 *     lower_stage rewrites each into the image_deref_* of a nir_var_image uniform of that
 *     binding, the form spirv_to_nir makes and turnip's lower_image_deref reads. The format
 *     stays PIPE_FORMAT_NONE, as the frontend leaves it -- the bound image's format decides
 *     the conversion, which is also what Gallium is given -- so the device enables
 *     shaderStorageImageReadWithoutFormat and ...WriteWithoutFormat. image_levels becomes
 *     the constant 1: Vulkan has no level query on a storage image (OpImageQueryLevels takes
 *     a sampled one, and turnip lowers no image_deref_levels), and every texture this back
 *     end makes has exactly one level. The texture's one view serves it (rwa_texture_create
 *     makes that view for MTLTextureUsageShaderWrite too, now). A texture a kernel only
 *     reads stays a SAMPLED_IMAGE, sampled or fetched (read() is a txf), as in a draw --
 *     unless the same dispatch also writes it through another slot, and then both of its
 *     descriptors say GENERAL, the one layout both kinds may be in at once. [M5c: and a texture
 *     written by a fragment or vertex shader is the same now: this said "is still the draw's
 *     refusal (M3b)".]
 *
 *   THREADGROUP MEMORY. The frontend lays out a kernel's threadgroup globals itself and
 *     reaches them with load_shared, store_shared and shared_atomic at explicit offsets,
 *     nir->info.shared_size their total (air_to_nir.c, plan_globals) -- which is already
 *     the form turnip's own shared path ends in: nir_lower_vars_to_explicit_types starts
 *     from info.shared_size and finds no variable to add, nir_lower_explicit_io finds
 *     nothing to lower, and tu6_emit_cs_config programs the size from what ir3 read off
 *     info.shared_size. -staticThreadgroupMemoryLength is that number (Gallium's
 *     static_shared_mem is the same one), and more than maxComputeSharedMemorySize is
 *     refused by name (M3d). A threadgroup BUFFER, sized by the dispatch
 *     (-setThreadgroupMemoryLength:atIndex:), is refused at the dispatch with Gallium's own
 *     sentence: the contract has no argument to carry the length, and hkmetal.m has
 *     nothing to pass.
 *
 *   SYSTEM VALUES. The frontend emits NIR's own intrinsics for Metal's attributes --
 *     load_global_invocation_id ([[thread_position_in_grid]]), load_local_invocation_id,
 *     load_workgroup_id, load_workgroup_size, load_num_workgroups, load_local_invocation_index,
 *     their products for [[threads_per_grid]], and load_subgroup_invocation, _id, _size and
 *     load_num_subgroups -- which is the form spirv_to_nir gives SPIR-V's built-ins, so
 *     turnip lowers them as it lowers those and nothing here touches them:
 *     nir_lower_system_values in tu_spirv_to_nir, then nir_lower_compute_system_values with
 *     has_base_workgroup_id in tu_shader_create (the base is a driver parameter turnip
 *     writes, 0 for vkCmdDispatch, so a grid starts at the origin as Metal's does), the
 *     workgroup size folded to the pipeline's constant, and load_num_workgroups a driver
 *     parameter turnip writes from the dispatch's own counts. finalize_for_gallium's trap
 *     (nir_lower_compute_system_values given NULL options) does not arise on this road.
 *
 *   THE DISPATCH AND ITS ORDER. rwa_dispatch_buffers makes Gallium's checks in Gallium's
 *     order and words -- an empty grid, more threads in a group than the pipeline takes, a
 *     threadgroup buffer, then every binding by resolve_binding (the draw's, which says
 *     "writes" for a storage image) -- and then Vulkan's two limits Gallium has none of,
 *     maxComputeWorkGroupSize per dimension and maxComputeWorkGroupCount, each refused by
 *     name (M3d). The grid is vkCmdDispatch's; the group size is the pipeline's. The
 *     contract has no non-uniform grid: hkmetal.m's -dispatchThreads: divides and refuses a
 *     grid that does not divide, for both back ends, and there is no indirect dispatch.
 *     ORDER is Metal's command-encoder ordering, kept the way the draws keep it: a serial
 *     compute encoder runs each dispatch after the one before it, and a command buffer
 *     each encoder after the last, for every resource it tracks -- so each dispatch, like
 *     each draw and blit, begins with barrier_all (every earlier write at every stage made
 *     visible to every later access) and moves its textures with image_barrier, whose
 *     source scope is every earlier write. The dispatch, draw or blit AFTER it begins the
 *     same way, so compute after render, render after compute, compute after compute and a
 *     blit or a processor read after compute are all ordered by the barrier at the head of
 *     the later command and by the host barrier that ends the command buffer.
 *     -memoryBarrierWithScope: stays a no-op in hkmetal.m, and is right here too: the
 *     barrier it would ask for between two dispatches is always there.
 *
 * TEXTURES OVER AN IOSURFACE (M3e), which is air_metal.c's import call for call, in
 * Vulkan's words. What arrives is what userland/src/hkdrawable.m hands either library:
 * the surface's ID (never the IOSurfaceRef, a GEM handle or an address), the offset and
 * the row length the kernel reported (kHKSurface's describe, which allocates nothing),
 * and the MTLTextureDescriptor's shape; the plane is always 0, hkdrawable.m refuses any
 * other. Five things decide how it looks:
 *
 *   THE MEMORY. A dma-buf import (VK_KHR_external_memory_fd, VK_EXT_external_memory_
 *     dma_buf) whose "file descriptor" is the surface's ID: exactly where Gallium puts it
 *     (air_metal.c's winsys_handle of WINSYS_HANDLE_TYPE_FD, which freedreno's
 *     fd_bo_from_dmabuf_drm takes as an ID on Darwin), and the slot the stand-in for libdrm
 *     answers -- drmPrimeFDToHandle(fd, ID) in tools/mesa/hackios-drm/hk_drm.c is kHKSurface,
 *     which makes the kernel's buffer of the surface and enters it into this process's GPU
 *     page tables (iokit/Kernel/HKGPU.cpp, surfaceBuffer). turnip-darwin.patch keeps the
 *     ID from ever being used as a descriptor of this process: msm_bo_init_dmabuf asks the
 *     kernel for the size instead of lseek()ing it, and tu_AllocateMemory does not close()
 *     it. vkGetMemoryFdKHR cannot hand an ID back out as a descriptor: drmPrimeHandleToFD
 *     is ENOSYS in hk_drm.c, and turnip refuses the export.
 *   THE LAYOUT. Linear, one plane, the kernel's offset and pitch: an image of
 *     VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT with an explicit DRM_FORMAT_MOD_LINEAR layout,
 *     which turnip gives fdl6_layout as it stands. The pitch must be a multiple of 16 texels
 *     above four bytes a texel and of 64 bytes otherwise (import_pitch_align), the same rule
 *     freedreno's import has; the kernel aligns rows to 64, so an RGBA16Float surface whose
 *     width is not a multiple of 16 is refused on both back ends.
 *   THE FORMATS. The colour formats of the table, by their LINEAR features and turnip's own
 *     answer for a linear dma-buf image (vkGetPhysicalDeviceImageFormatProperties2):
 *     BGRA8Unorm and RGBA8Unorm (and _sRGB), R8Unorm, RGBA16Float -- what hkdrawable.m's
 *     table sends -- and the rest of the table where turnip says yes. Refused by name
 *     (M3e): YCbCr in two planes (500-629, in neither back end) and depth (turnip has no
 *     linear depth attachment).
 *   OWNERSHIP. The kernel's buffer belongs to turnip's table from the import on, one per
 *     surface per process and counted (the kernel answers the same handle for the same
 *     surface again), and holds the surface while it exists; vkFreeMemory at
 *     rwa_texture_destroy gives it back (DRM_IOCTL_GEM_CLOSE, kDrmGemClose), as fd_bo_del
 *     does on Gallium. The surface's memory is its owner's and is never freed here.
 *   COHERENCY, which is the kernel's and not this file's, on both back ends alike. A surface
 *     is ordinary cached memory. At every submission naming its buffer, the kernel cleans
 *     and invalidates the processor's cache over it (HKGPU.cpp submit: when the surface's
 *     seed changed, when the program has mapped the buffer, or 4 ms after the last time),
 *     and turnip names every buffer of the device in every submission (submit_bo_list), as
 *     freedreno names every buffer its batch uses. After the GPU's writes nothing
 *     invalidates the processor's cache: not the kernel at the fence (kMsmWaitFence and
 *     kMsmGemCpuPrep wait only), not IOSurfaceLock (MiniIOSurface.cpp lockCommon moves the
 *     seed, no more). Gallium's path adds nothing either. So this file does what air_metal.c
 *     does -- every command buffer ends in a host barrier and is waited for -- and no more:
 *     a processor that read the surface after the last clean and reads it again after the
 *     GPU wrote it may see what it read before (air_metal_test --surface self measures it).
 *     [Run 061800: that happened on this back end alone. "When the program has mapped the
 *     buffer" is the difference: freedreno maps an imported resource the first time the
 *     processor touches it through the driver, turnip never did, so the kernel cleaned a vk
 *     surface only on a seed change or after 4 ms. The import now maps it once (THE SURFACE
 *     MARKED "SEEN", texture_of_shape). And sys_dcache_flush cannot stand in for a clean in a
 *     program: in iOS 16.5 on arm64 it is "dsb ish; ret" (libsystem_platform of 20F66, read
 *     in its dyld cache); DC CIVAC by hand works, EL0 being allowed it (SCTLR_EL1.UCI).]
 *
 * The image starts in GENERAL, not UNDEFINED (struct rwa_texture.layout), because the
 * surface already holds a picture and a barrier from UNDEFINED may discard it.
 *
 * ONE COMMAND BUFFER AT A TIME, AND A POOL OF THEM. Every recording call (a draw, a clear,
 * a blit, a read) records into the device's current command buffer, beginning one when
 * there is none. rwa_flush submits it with its own VkFence and hands the pair out as the
 * struct rwa_fence; rwa_fence_release puts the pair back, and the next recording takes a
 * returned pair whose fence has signalled before it makes a new one. The pair is reset
 * only by the recording side, because a command pool is externally synchronised and
 * rwa_fence_release is called from whatever thread drops the Metal command buffer
 * (-[HKMetalCommandBuffer dealloc]). Without rwa_device_defer_waits every draw, clear and
 * blit submits and waits for itself, which is air_metal.c's behaviour and air_run's. A
 * texture read or write always does: the processor's bytes are on the other side of it.
 * [M4-1, the cost part: a read still does. A write has its bytes in hand once they are copied, and
 * goes with the next flush, unless it is told otherwise (UPLOADS, at upload_begin).]
 *
 * [M4-1: AND WHICH THREAD MAY DO WHAT, because run 071937's GPU faults were a second thread
 * doing the first thread's part (the M4-1 entry at the top says how). Three rules:
 *
 *   RECORDING belongs to the thread that began it. record_begin notes that thread beside
 *     `current` (device->recorder, under the lock), and nothing submits a recording from any
 *     other thread. Every recording call of the contract is made on the contract's one
 *     thread at a time; the calls that also arrive from elsewhere -- rwa_finish from
 *     hkmetal.m's completion queue, and every destroy, which runs wherever the last
 *     reference to a Metal object goes -- now record nothing unless the calling thread is
 *     the one recording (wait_for_all).
 *   WAITING FOR EVERYTHING is waiting for the newest submission's fence. device->last holds
 *     a reference on the newest pair submitted, and a fence of vkQueueSubmit covers every
 *     command earlier in submission order, so that one fence is "everything submitted so
 *     far" -- what the empty command buffer submit_and_wait used to make was for, without
 *     making one. rwa_flush with nothing recorded hands that pair out too (freedreno's
 *     last_fence, which is what Gallium's flush gives for an empty batch), so an empty Metal
 *     command buffer completes when the work before it has and costs no submission.
 *   A PAIR IS REUSED only when nobody holds it and its fence has signalled. `refs` counts
 *     its holders (the recording, each fence rwa_flush handed out, device->last, and a
 *     waiter of wait_for_all for as long as it waits); the last to let go puts it on the
 *     `returned` list, and record_begin takes from that list only a pair whose
 *     vkGetFenceStatus is VK_SUCCESS -- which it always did. So a fence given back early,
 *     -[HKMetalCommandBuffer dealloc] of a buffer the GPU still runs included, is normal
 *     Metal and costs nothing: the pair waits in the list until the GPU is done with it.]
 * [M4-1, the cost part: and all three stand. The upload command buffer is not a recording: any
 * thread may fill it, and send it, under a lock of its own (UPLOADS); a pair's fence having
 * signalled is now a thing seen once, in the order of the queue (reap), and the same sight
 * frees what destroys left behind (when_done) and ring space (THE STAGING RING).]
 *
 * WHAT IS COUNTED, and why: command buffers and fences in use (made and not back in the
 * pool), rwa_buffers and rwa_textures alive. rwa_device_close prints the four numbers,
 * and rwa_vk_counts -- not part of air_metal.h, exported for tests -- returns them, so a
 * test that has released everything it made can say "nothing leaked" with a number
 * instead of with an absence of crashes. Memory this library allocates for itself
 * (staging for texture reads and writes, the upload buffers above) is not an rwa_buffer
 * and is not in the count; it belongs to a call or to a command buffer and goes with it.
 * [M4-1, the cost part: the counts go down at the destroy, when the application let go. What a
 * destroy left waiting for a fence is the library's now, and device->children counts it until it
 * has gone, for rwa_device_close.]
 */

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No prototypes: this library has turnip's entrypoints under turnip's names only, and it
 * links with -undefined dynamic_lookup (the entrypoint tables' weak references, see
 * meson.build). A call to vkSomething written here by mistake would then link, become a
 * flat-namespace lookup and fail at run time; without prototypes it is a compile error
 * instead. Everything below goes through vk_icdGetInstanceProcAddr, as a loader would.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

/* The contract's calls are the only symbols of this file the library exports (with the
 * three vk_icd* entry points, src/freedreno/vulkan/tu_hackios_exports.txt): everything
 * else in libvulkan_freedreno is built -fvisibility=hidden, so the declarations are
 * given default visibility here, where they are read, and nowhere else.
 */
#pragma GCC visibility push(default)
#include "air_metal.h"

/* NOT in air_metal.h, and not part of the Metal contract: the four counts above, for a
 * test (userland/src/metal_drv_test.m prints them when it ran on this back end). Command
 * buffers in use, fences in use, rwa_buffers alive, rwa_textures alive, in that order,
 * process-wide.
 */
void rwa_vk_counts(unsigned counts[4]);
#pragma GCC visibility pop

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "util/bitscan.h"           /* sample counts are bits (MULTISAMPLE) */
#include "util/format/u_format.h"   /* rwa_depth_read decodes with Mesa's own unpackers */
#include "util/half_float.h"
#include "util/u_math.h"            /* u_minify: the size of a level (M5b) */
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_atomic.h"
#include "vk_enum_to_str.h"
#include "vk_pipeline.h"
#include "drm-uapi/drm_fourcc.h"   /* DRM_FORMAT_MOD_LINEAR: an IOSurface's layout (M3e) */

/* turnip's own entry point for a loader, in this same library (tu_device.cc) */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                    const char *name);
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);

/* and the one thing the Vulkan API does not carry: turnip's NIR options (tu_hackios.cc) */
const struct nir_shader_compiler_options *tu_hackios_nir_options(VkDevice device);

/* Five seconds. air_metal.c waits two; the brief for this back end said five, and what
 * matters is that it is bounded: a submission nobody waited for is not a result, and on
 * this GPU one timeout wedges the core for the rest of the boot (docs/air.md section 5).
 */
#define RWA_FENCE_TIMEOUT_NS 5000000000ull

/* The binding numbers of the one push set (RESOURCES above). */
#define RWA_BIND_TEXTURE_BASE 32
#define RWA_BIND_SAMPLER_BASE 160
#define RWA_BIND_MEMBER_BASE  192    /* M5c: an argument buffer's members, as they come */
#define RWA_MAX_STAGE_MEMBERS 64
#define RWA_BIND_VERTEX_BASE  256
#define RWA_BIND_COMPUTE_BASE 512    /* a kernel's, in its own layout (COMPUTE, M3d) */
#define RWA_BIND_FETCH_BASE   512    /* M5d: a render layout's input attachments (FRAMEBUFFER FETCH) */
#define RWA_MAX_BUFFERS       31     /* Metal's [[buffer(0..30)]] */
#define RWA_MAX_TEXTURES      128    /* Metal's [[texture(0..127)]] */
#define RWA_MAX_SAMPLERS      32     /* Metal's 16, and the shader's own after them */

/* The turnip entry points this file calls, looked up once per device. One list, so a
 * name used below and not looked up is a compile error rather than a NULL call.
 */
#define RWA_VK_INSTANCE_FNS(X)                                                      \
   X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)    \
   X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures2)                    \
   X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties)   \
   X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(GetDeviceProcAddr)       \
   X(GetPhysicalDeviceImageFormatProperties)                                        \
   X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceImageFormatProperties2)

#define RWA_VK_DEVICE_FNS(X)                                                        \
   X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateCommandPool)        \
   X(DestroyCommandPool)                                                            \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(ResetCommandBuffer)            \
   X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateFence) X(DestroyFence)         \
   X(ResetFences) X(WaitForFences) X(GetFenceStatus) X(QueueSubmit)                 \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)                  \
   X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)  \
   X(CreateImageView) X(DestroyImageView) X(CreatePipelineLayout)                   \
   X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline)           \
   X(CreateSampler) X(DestroySampler) X(CreateDescriptorSetLayout)                  \
   X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool)   \
   X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)         \
   X(CmdBindDescriptorSets)                                                         \
   X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage)            \
   X(CmdCopyBuffer) X(CmdFillBuffer) X(CmdCopyImage) X(CmdClearColorImage)          \
   X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)                 \
   X(CmdClearDepthStencilImage) X(CmdSetStencilCompareMask)                         \
   X(CmdSetStencilWriteMask) X(CmdSetStencilReference) X(CmdResolveImage)            \
   X(CreateComputePipelines) X(CreatePipelineCache) X(GetPipelineCacheData)         \
   X(DestroyPipelineCache) X(CmdDispatch)                                          \
   X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed)                  \
   X(CmdSetBlendConstants) X(CmdBlitImage)

#define RWA_VK_DECLARE(name) PFN_vk##name name;

/* Memory this library owns for itself, mapped for the processor: staging for a texture
 * read or write, a piece of upload memory, and the storage behind an rwa_buffer.
 */
struct rwa_raw {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize size;
};

/* An upload buffer of one command buffer: a misaligned constant block is copied into it
 * (RESOURCES, OFFSETS). Bump-allocated, reset when the pair is reused, freed at close.
 */
struct rwa_upload {
   struct rwa_upload *next;
   struct rwa_raw raw;
   VkDeviceSize used;
};

/*
 * M4-1, WHAT GOES WHEN A FENCE HAS SIGNALLED: the last work for the GPU that named an object, as
 * two numbers (0: none ever did). `recording` is the number of a recording, `upload` of an upload
 * command buffer. Recordings go to the queue in the order of their numbers and uploads in the
 * order of theirs; the two orders are not one, because an upload goes AHEAD of the recording that
 * was already open when it began (UPLOADS, at upload_begin) -- which is why there are two. An
 * object is free of the GPU when both have been passed: device->recording_done and
 * device->upload_done, which only a pair's signalled fence moves (reap).
 */
struct rwa_use {
   uint64_t recording;
   uint64_t upload;
};

/* An upload command buffer, out of device->upload_pool, kept for use again. */
struct rwa_ucmd {
   struct rwa_ucmd *next;
   VkCommandBuffer cmd;
};

/* What a destroy left behind: the object, and the work it waits for (when_done). */
typedef void (*rwa_free_fn)(struct rwa_device *device, void *object);
struct rwa_after {
   struct rwa_after *next;
   struct rwa_use use;
   rwa_free_fn free;
   void *object;
};

/* Where the bytes of one upload or read-back are for this call (THE STAGING RING). */
struct rwa_staging {
   VkBuffer buffer;
   VkDeviceSize offset;
   uint8_t *map;                  /* the processor's address of `offset` */
   struct rwa_raw *own;           /* a buffer of its own: too big for the ring, or the ring full */
   bool reading;                  /* a read-back out of the ring, holding the ring's tail still */
};

/*
 * A command buffer and the fence its submission signals: the unit the pool keeps, and
 * what rwa_flush hands out as `struct rwa_fence` -- the header declares that struct
 * without a body, so this IS its body. `next` links it into the device's list of pairs
 * given back by rwa_fence_release; `all` links every pair the device ever made, so
 * rwa_device_close can destroy each one exactly once whatever state it is in.
 */
struct rwa_fence {
   struct rwa_fence *next;
   struct rwa_fence *all;
   VkCommandBuffer cmd;
   VkFence fence;
   struct rwa_upload *uploads;
   /* M4-1, under the device's lock. Who holds this pair: the recording side from
    * record_begin on, each fence rwa_flush handed out, device->last, and a waiter of
    * wait_for_all while it waits. It is on `returned` exactly when this is 0. */
   unsigned refs;
   /* Its present recording reached the queue, so its fence means something. Only the exit
    * line reads it: a pair whose submission failed has a fence that never signals, and that
    * is not GPU work in flight. */
   bool submitted;
   /* M4-1, the cost part. `cmd` may be NULL (a pair that has only ever carried uploads), and
    * the three below are this pair's submission, 0 where it had none: the recording, the
    * upload command buffer that went to the queue ahead of it (and `ucmd`, which comes back
    * for use again with this pair), and how far the ring was handed out by then. Under the
    * lock: `flight_next` strings the pairs on the queue in the order they went, and `signalled`
    * says it has come off that list, its fence seen signalled. It is what record_begin takes a
    * pair by now and not a fence's status (reap). */
   uint64_t recording;
   uint64_t upload;
   uint64_t ring_end;
   struct rwa_ucmd *ucmd;
   struct rwa_fence *flight_next;
   bool signalled;
   /* M5c: where a recording's ordinary descriptor sets come from (ARGUMENT BUFFERS, MORE THAN A
    * PUSH SET HOLDS): reset as the command buffer is, and those it filled, kept until then */
   VkDescriptorPool sets;
   VkDescriptorPool *full_sets;
   unsigned full_set_count;
};

struct rwa_device {
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice dev;
   VkQueue queue;
   uint32_t family;
   uint32_t api_version;          /* what the physical device says it is */
   VkCommandPool pool;
   VkPhysicalDeviceMemoryProperties memory;
   VkPipelineLayout empty_layout; /* for a pipeline whose stages bind nothing */
   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

   /* the limits RESOURCES is decided by, read once (and printed once a process) */
   VkDeviceSize ubo_align, ssbo_align, max_ubo_range;
   unsigned max_push;
   /* M5c: the buffers that hold an encoder's records (ARGUMENT BUFFERS), under argbuf_lock */
   struct rwa_buffer *encoded;
   pthread_mutex_t argbuf_lock;
   bool anisotropy;               /* samplerAnisotropy was there to be enabled */
   /* M5b: VK_KHR_maintenance5 was listed and enabled, which is what VK_FORMAT_A8_UNORM_KHR is
    * named by; and imageCubeArray was there. */
   bool maintenance5;
   bool cube_array;
   float max_anisotropy;
   unsigned max_3d;               /* maxImageDimension3D: a side of a 3D texture (M3c) */

   /* MULTISAMPLE (M3c-3): the sample counts a pass of each kind of attachment may have
    * (framebuffer{Color,Depth,Stencil}SampleCounts), and the resolve modes a depth and a
    * stencil aspect may be resolved with (VkPhysicalDeviceDepthStencilResolveProperties,
    * core in 1.2; 0 below it, where a depth resolve is refused by name). Which counts an
    * IMAGE of a format may have is asked per format, at rwa_texture_create. */
   VkSampleCountFlags fb_color_samples, fb_depth_samples, fb_stencil_samples;
   VkResolveModeFlags depth_resolve_modes, stencil_resolve_modes;

   /* COMPUTE (M3d): Vulkan's limits a dispatch is judged by, what a pipeline reports
    * (THE WAVE: max_threads is maxComputeWorkGroupInvocations capped at Metal's 1024,
    * simd_width minSubgroupSize), and whether a compute stage may be given
    * ALLOW_VARYING_SUBGROUP_SIZE (subgroupSizeControl was there to be enabled). */
   unsigned max_group_size[3], max_group_count[3];
   unsigned max_threads, max_shared, simd_width;
   bool varying_subgroups;

   /* TEXTURES OVER AN IOSURFACE (M3e): whether the device was made with the three
    * extensions the import is said in -- VK_KHR_external_memory_fd,
    * VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier -- which turnip
    * lists on every device (tu_device.cc) and this library enables only when listed. */
   bool can_import;

   /* AIR_VK_NOSEEN=1, read once at rwa_device_open: an imported surface is NOT mapped once at
    * import (TEXTURES OVER AN IOSURFACE, COHERENCY), which is this back end's behaviour before
    * run 061800 -- kept so a test can still show what that mapping fixes. Off by default. */
   bool no_seen;

   /* turnip's NIR options, the ones its own compiler uses (tu_hackios.cc) */
   const struct nir_shader_compiler_options *nir_options;

   struct { RWA_VK_INSTANCE_FNS(RWA_VK_DECLARE) RWA_VK_DEVICE_FNS(RWA_VK_DECLARE) } vk;
   /* core in 1.3 and the KHR extension's names otherwise; the same functions */
   PFN_vkCmdBeginRendering CmdBeginRendering;
   PFN_vkCmdEndRendering CmdEndRendering;
   PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSet;
   /* The depth/stencil state a draw sets (DEPTH AND STENCIL, THE STATE): core in Vulkan
    * 1.3 and looked up only there, so all five are NULL on an older device, and a
    * pipeline with a depth attachment is refused there by name rather than made with a
    * state it cannot set. (The stencil masks and reference are 1.0's, in the list above.)
    */
   PFN_vkCmdSetDepthTestEnable CmdSetDepthTestEnable;
   PFN_vkCmdSetDepthWriteEnable CmdSetDepthWriteEnable;
   PFN_vkCmdSetDepthCompareOp CmdSetDepthCompareOp;
   PFN_vkCmdSetStencilTestEnable CmdSetStencilTestEnable;
   PFN_vkCmdSetStencilOp CmdSetStencilOp;
   /* THE DRAW PATH (M5a): the two states of a triangle's face, core in 1.3 like the five
    * above and NULL below it, where a draw that culls is refused by name. */
   PFN_vkCmdSetCullMode CmdSetCullMode;
   PFN_vkCmdSetFrontFace CmdSetFrontFace;
   /* ... what a pipeline's blend and vertex input are judged by, read once: whether two
    * attachments may blend differently, whether a per-instance stream may step at a rate
    * above one (VK_KHR_vertex_attribute_divisor) and whether a one-sample line may be
    * Bresenham's, as Gallium's is (VK_KHR_line_rasterization; both enabled only when
    * turnip lists them), and Vulkan's limits for streams and viewports. */
   bool independent_blend, divisors, bresenham;
   unsigned max_vi_bindings, max_vi_offset, max_vi_stride;
   float max_viewport[2], viewport_bounds[2];

   bool defer_waits;              /* rwa_device_defer_waits */

   /* The command buffer being recorded, begun by the first call that records anything
    * and ended by the submission that hands it out. NULL: nothing is recorded. Touched
    * only by the thread the rest of the contract belongs to.
    * [M4-1: and written under the lock, with `recorder` beside it, because a thread that is
    * not recording reads the two to find that out (wait_for_all). Only the recorder writes
    * it, so the recorder alone may read it without the lock.]
    */
   struct rwa_fence *current;
   pthread_t recorder;

   /* Pairs rwa_fence_release has given back, which any thread may do -- so this list
    * and nothing else is under the lock. They are reset and reused only by the
    * recording side, and only once their fence has signalled.
    */
   pthread_mutex_t lock;
   struct rwa_fence *returned;
   struct rwa_fence *all;

   /* M4-1, under the same lock: the newest pair that reached the queue, with a reference of
    * its own. Its fence covers every earlier submission, so it is what "wait for everything"
    * and an empty command buffer's fence both are. NOT counted in pairs_out or rwa_vk_counts:
    * it is the library's own, nobody has to give it back, and a test that has released
    * everything still reads 0. */
   struct rwa_fence *last;

   /* M4-1, the cost part, under the same lock. The pairs on the queue, oldest first, and the
    * numbers their fences have reached (struct rwa_use, reap); what destroys left behind; the
    * upload command buffers free for use again; the numbers of the ring (THE STAGING RING). */
   struct rwa_fence *flight_first, *flight_last;
   uint64_t recordings;           /* (the last number given) */
   uint64_t recording_done, upload_done, ring_done;
   unsigned ring_readers;
   struct rwa_after *after;
   struct rwa_ucmd *ucmd_free;

   /* M4-1, the cost part: UPLOADS (at upload_begin). A lock of its own and always taken before
    * `lock`: around the upload command buffer being recorded, its pool, the ring's two ends,
    * and every vkQueueSubmit, because a VkQueue is externally synchronised and an upload may
    * reach the queue from any thread. `uploads` is the number of the newest upload, the one in
    * `uploading` while that is not NULL. */
   pthread_mutex_t upload_lock;
   VkCommandPool upload_pool;
   struct rwa_ucmd *uploading;
   uint64_t uploads;
   struct rwa_raw ring;           /* made at its first use; .buffer 0 until then */
   uint64_t ring_head, ring_tail;

   /* The next device open in this process, for the line printed at exit (exit_line). */
   struct rwa_device *open_next;

   /* rwa_buffers, rwa_textures, rwa_samplers and rwa_renders of this device still alive,
    * and pairs taken out of the pool and not given back (the one being recorded, and
    * every fence rwa_flush handed out that nobody has released). A device closed with any
    * of either is kept (see rwa_device_close). The second is atomic: rwa_fence_release
    * may run on any thread.
    * [M4-1: the first is atomic too: a destroy runs wherever the last reference to a Metal
    * object goes, which includes hkmetal.m's completion queue.]
    */
   int32_t children;
   int32_t pairs_out;
};

struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
static void argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
                         const struct rwa_texture *texture, const struct rwa_sampler *sampler);
static void argbuf_forget_all(struct rwa_buffer *dest);
static bool argbuf_mask_of(const struct air_shader_info *info, const char *name, unsigned *mask,
                           char *why, size_t why_size);

/* M5c: what an MTLArgumentEncoder recorded (ARGUMENT BUFFERS): air_metal.c's two structures. */
struct rwa_argbuf_slot {
   bool set;                      /* this member was encoded at all */
   bool destroyed;                /* ... and the resource it named has been destroyed */
   unsigned kind;                 /* enum rwa_argbuf_kind: which call set it */
   struct rwa_buffer *buffer;     /* RWA_ARGBUF_BUFFER, with Metal's own offset */
   size_t offset;
   struct rwa_texture *texture;   /* RWA_ARGBUF_TEXTURE */
   struct rwa_sampler *sampler;   /* RWA_ARGBUF_SAMPLER */
};

struct rwa_argbuf_encoding {
   struct rwa_argbuf_encoding *next;
   size_t offset;
   unsigned slots;                /* how many the array holds */
   unsigned members;              /* how many of them have been set */
   struct rwa_argbuf_slot *slot;
};

struct rwa_buffer {
   struct rwa_device *device;
   struct rwa_raw raw;
   size_t size;
   struct rwa_use use;            /* M4-1: the last work that named it */
   /* M5c: the encodings recorded into this buffer, and its place on device->encoded, which it
    * joins when the first member is encoded: a buffer that is only a buffer costs nothing */
   struct rwa_argbuf_encoding *encodings;
   struct rwa_buffer *next_encoded;
   bool listed;
};

/* What a colour MTLPixelFormat is in Vulkan's terms, and only where both name the same
 * bytes in the same order, as in air_metal.c's table: every entry is whole bytes or
 * whole 16- or 32-bit channels, so the translation is the identity on memory. `kind` is
 * what a clear colour is converted to and what element type a shader must read it as,
 * which is decided by the format and nothing else.
 */
enum rwa_kind { RWA_KIND_FLOAT, RWA_KIND_UINT, RWA_KIND_SINT };

struct rwa_vk_format {
   unsigned mtl;
   const char *name;
   VkFormat vk;
   unsigned bytes;                /* per texel */
   enum rwa_kind kind;
   /* The depth and stencil aspects of a depth/stencil format (M3c-2). Left out -- zero --
    * for every colour entry, whose one aspect is COLOR: format_aspects() is the one
    * reader of this field and says so, so no caller ever sees the zero. */
   VkImageAspectFlags zs_aspects;
   /* M5b: a compressed format's block, in texels, with `bytes` the bytes of one; left out -- zero
    * -- for everything else, whose block is one texel. fmt_block_w and _h are the one reader. */
   unsigned char block_w, block_h;
   /* M5b: only a view may have this format (MTLPixelFormatX32_Stencil8). */
   bool view_only;
};

/* A view of one level and one layer for a pass to render into, once it has (att_view_for). */
struct rwa_att_view {
   unsigned level, layer;
   VkImageView view;
};

struct rwa_texture {
   struct rwa_device *device;
   VkImage image;
   VkDeviceMemory memory;
   /* M5b: which texture owns the image, itself for one that was made. A view
    * (rwa_texture_view_create) has `image` copied and no memory, and everything about the image
    * as a whole is read through this: its layout, its record of use, whether it may be
    * reinterpreted. `refs`, on the root, counts the textures over the image. */
   struct rwa_texture *root;
   int32_t refs;
   /* M5b: this texture's window on the image, in the image's own numbers: `level` and `layer`
    * of every call add these. A 3D image has one layer. A MTLTextureType2D over one depth plane
    * of a 3D image (the view a pass's clear of it needs, air_metal.h) says so, and its plane is
    * base_layer. */
   unsigned base_level, levels;
   unsigned base_layer, layers;
   bool plane_view;
   bool cube_compatible, plane_compatible, mutable_format;   /* what the root's image was made */
   VkImageViewType view_type;
   VkComponentMapping swizzle;
   bool swizzled;
   /* M5b, TEXTURES, LEVELS AND SLICES, ITS VIEWS */
   VkImageView store_view;        /* a kernel's storage image: `view` where that is one level */
   VkImageView base_view;         /* one level, for a sampler in pixel coordinates; made when asked */
   struct rwa_att_view *att;
   unsigned att_count;
   /* For rendering into and for sampling a colour texture; NULL if neither. For a depth
    * texture it is the SAMPLING view alone, of one aspect (DEPTH AND STENCIL, VIEWS), and
    * the render pass takes zs_view. */
   VkImageView view;
   VkImageView zs_view;           /* a depth texture's every aspect, for the render pass */
   VkImageAspectFlags aspects;    /* COLOR, or the format's DEPTH and/or STENCIL */
   const struct rwa_vk_format *format;
   struct rwa_texture_shape shape;
   unsigned depth;                /* texels deep: shape.depth of a 3D texture, else 1 */
   /* Samples per texel (MULTISAMPLE, M3c-3): shape.sample_count of a
    * MTLTextureType2DMultisample, 1 for every other type, and the same as Vulkan's flag
    * bit -- the number air_metal.c reads back off res->nr_samples. */
   unsigned samples;
   VkSampleCountFlagBits sample_bit;

   /* The layout the image was last left in by a command this library recorded, which
    * is the old layout of the next barrier. Recording order and execution order are
    * the same thing on one queue, so this is true at the point where the next command
    * is recorded -- and the barrier itself, not that order, is what orders the accesses.
    */
   VkImageLayout layout;
   /* [M4-1, the cost part: that sentence holds for recordings, and an upload goes to the queue
    * AHEAD of the recording open while it is recorded (UPLOADS, at upload_begin). So, all under
    * device->lock: `layout` is where the image is left by everything recorded so far;
    * `layout_at_start` is what the open recording's first barrier on this image took for its
    * old layout, when use.recording says the open recording has named it, and an upload then
    * starts from that and goes back to it; and an image still in TRANSFER_DST in the open upload
    * needs no second barrier for a write that does not overlap what it was written with:
    * `upload_id` says which upload that is, and `upload_box` what has been written since its
    * last barrier (min x y z, end x y z).] */
   VkImageLayout layout_at_start;
   uint64_t upload_id;
   unsigned upload_box[6];
   struct rwa_use use;            /* M4-1: the last work that named it */
   bool render_target;
   bool shader_read, shader_write;   /* what MTLTextureUsage allowed */

   /* An IOSurface's memory and not this library's (TEXTURES OVER AN IOSURFACE, M3e): the
    * surface's ID, for sentences, or 0. The memory is still `memory`, imported, and
    * vkFreeMemory gives it back the same way. */
   int surface_id;
};

/* A sampler: two Vulkan samplers, for air_metal.c's reason (struct rwa_sampler there).
 * Metal decides per INSTRUCTION whether a sample compares (sample vs sample_compare over
 * one MTLSamplerState); Vulkan, like Gallium, decides it in the sampler (compareEnable).
 * So `compare` is the same state with the comparison on, bound into exactly the slots a
 * shadow instruction samples with. NULL where Vulkan forbids one: a sampler in pixel
 * coordinates may not compare (unnormalizedCoordinates), and a shader that tries is
 * refused at the draw by name.
 */
struct rwa_sampler {
   struct rwa_device *device;
   VkSampler plain;
   VkSampler compare;
   /* M5b: the same state with VK_BORDER_COLOR_INT_* for an integer texture, made only for an
    * opaque border colour that some address mode reads (TEXTURES, LEVELS AND SLICES, SAMPLERS).
    * NULL where plain serves both: transparent black is all zeros in both of Vulkan's types. */
   VkSampler plain_int;
   struct rwa_use use;            /* M4-1: the last work that named it */
   bool pixel_coords;             /* unnormalizedCoordinates: never on a 3D view (M3c) */
   /* [M3d-2: and so false for a shader's own pixel-coordinate sampler that samples a
    * texture3d, which is made normalised (3D TEXTURES IN PIXEL COORDINATES): this flag
    * says what the Vulkan sampler is, and that one is not unnormalised.] */
};

/* MTLPrimitiveType, which has five values; a pipeline is made per type the first time it
 * is drawn with (Vulkan fixes the topology in the pipeline), the strip at creation.
 */
#define RWA_PRIMITIVE_TYPES 5

/* One binding of a stage: a buffer, a texture or a sampler the module declares, where it
 * sits in the push set, and what the instructions say about it.
 */
enum rwa_bind_kind { RWA_BIND_BUFFER, RWA_BIND_TEXTURE, RWA_BIND_SAMPLER };

struct rwa_binding {
   enum rwa_bind_kind kind;
   unsigned index;                /* Metal's [[buffer/texture/sampler(n)]], or a member's flat one */
   unsigned binding;              /* in the push set */
   /* M5c: a member of an argument buffer (r->argbuf_set is not ~0u), and whether an instruction
    * reaches it: a member nothing reads is not bound, so it need not have been encoded */
   bool member;
   bool used;
   /* M5c: how many descriptors, an array's length or 1, at `binding`, from `index`; and whether a
    * function constant gates it, so that nothing bound there is not a refusal (ARGUMENT BUFFERS,
    * ARRAYS OF DESCRIPTORS) */
   unsigned span;
   bool gated;
   VkDescriptorType type;
   const struct air_resource *r;  /* the declaration (the first, where there are two) */

   /* a buffer */
   bool constant;                 /* `constant` address space; `device` otherwise */
   bool written;                  /* a store or an atomic reaches it */
   bool ubo_ok;                   /* every load is at a constant offset inside r->size */
   unsigned ubo_size;             /* UNIFORM_BUFFER: the declared size, the range bound */

   /* a texture: what the instructions and Apple's metadata say, as air_metal.c's
    * struct rwa_slot holds it */
   bool sampled, stored, read, write;
   bool shape_known;
   enum glsl_sampler_dim dim;
   bool is_array;
   bool shadow;
   bool type_known;
   nir_alu_type el_type;          /* from the instructions */
   const char *declared_element;  /* "half", "uint", ...; NULL when none was read */
   nir_alu_type declared_type;
   unsigned declared_bits;
   /* M5b: which samplers sample it, by index, so that a sampler in pixel coordinates gets its
    * one level (base_view); and whether the shader asks a storage image how many levels it has,
    * which Vulkan has no query for: answered 1, so a chain bound there is refused. */
   uint64_t sampler_mask;
   bool asks_levels;

   /* a sampler */
   bool compare;                  /* a shadow instruction samples with it */
   bool samples_3d;               /* ... and an instruction samples a texture3d with it */
   /* M5b: and what it samples. An integer texture reads an integer border colour, and one
    * sampler in one stage reading both kinds through an opaque border is refused by name. */
   bool samples_int, samples_float;
   bool inline_sampler;           /* the shader's own constexpr sampler */
   bool pixel_coords;             /* ... in pixel coordinates */
   /* ... and, being all three (its own, in pixel coordinates, sampling a texture3d), made
    * a NORMALISED Vulkan sampler, with every coordinate sampled through it divided by the
    * texture's size in the shader (M3d-2, 3D TEXTURES IN PIXEL COORDINATES) */
   bool normalize;
   struct rwa_sampler own;        /* ... made for it at pipeline creation */

   nir_variable *var;             /* a texture's or sampler's uniform, once made */
};

struct rwa_stage {
   const char *name;              /* the entry point, for sentences */
   const char *word;              /* "vertex" or "fragment" */
   VkShaderStageFlags flags;
   unsigned base;                 /* where this stage's bindings start */
   unsigned count;
   struct rwa_binding *b;         /* one per declared resource at most */
};

/*
 * What a vertex descriptor became (THE DRAW PATH, VERTEX INPUT): Vulkan's two tables and the
 * divisors of the streams that step at a rate above one, packed as Vulkan takes them, and
 * what a draw checks its streams against -- air_metal.c's struct rwa_vertex_state, whose
 * fields past the first three keep their names and meanings. `count` and `attribute` are by
 * driver location, which is what rwa_render_info reports; Vulkan itself matches by location,
 * so nothing here depends on that order the way Gallium's element array does.
 */
struct rwa_vertex_state {
   VkVertexInputBindingDescription binding[32];
   VkVertexInputAttributeDescription attribute_vk[16];
   VkVertexInputBindingDivisorDescriptionKHR divisor[32];
   unsigned bindings, attributes, divisors;
   unsigned count;
   unsigned attribute[16];        /* Metal's [[attribute(n)]] per driver location */
   unsigned stream_mask;          /* Metal buffer indices fetched from */
   unsigned stream_count;         /* the highest of them plus one */
   unsigned stride[RWA_MAX_VERTEX_BUFFERS];
   unsigned per_vertex;           /* which of them step per vertex */
   unsigned bytes[RWA_MAX_VERTEX_BUFFERS];   /* the largest element end in each */
   bool placeholder;              /* a gated attribute with nothing configured: see there */
};

struct rwa_render {
   unsigned fetch_targets;        /* M5d: the targets read as [[color(n)]] (FRAMEBUFFER FETCH) */
   struct rwa_device *device;
   bool ordinary_set;             /* M5c: more than a push set holds */
   void *mem_ctx;                 /* the two NIR shaders and the frontend's info */
   nir_shader *vs, *fs;           /* kept: a pipeline for another topology is made later */
   struct air_shader_info finfo;
   struct air_shader_info vinfo;  /* valid only when a vertex module was supplied */
   bool generated_vertex;
   bool vs_writes_point_size;
   struct rwa_render_shape shape;
   VkFormat color_format[8];
   VkColorComponentFlags write_mask[8];
   /* What the shape's blend state became, per attachment (THE DRAW PATH, BLENDING): Vulkan's
    * own struct, its write mask write_mask[i]; blending off and every channel written for a
    * shape that carries none. */
   VkPipelineColorBlendAttachmentState blend[8];
   /* What its vertex descriptor became, empty for a stage that fetches nothing (THE DRAW
    * PATH, VERTEX INPUT), and the sixteen bytes a gated attribute with nothing configured
    * reads, made only for such a pipeline. */
   struct rwa_vertex_state vstate;
   struct rwa_raw placeholder;
   /* The depth/stencil attachment the pipeline is made for (M3c-2): its MTLPixelFormat
    * (depth_format, or stencil_format when that is the one set; 0 for none), its Vulkan
    * format and the aspects that format has, which decide whether the pipeline names a
    * depth attachment format, a stencil one or both. */
   unsigned zs_mtl;
   VkFormat zs_format;
   VkImageAspectFlags zs_aspects;
   /* The samples per pixel of a pass the pipeline draws in (MULTISAMPLE, M3c-3):
    * shape.sample_count, 0 read as 1, and its Vulkan flag bit, which is the pipeline's
    * rasterizationSamples. Every attachment of a draw has exactly this many. */
   unsigned samples;
   VkSampleCountFlagBits sample_bit;
   struct rwa_stage vstage, fstage;
   VkDescriptorSetLayout set_layout;   /* NULL when neither stage binds anything */
   VkPipelineLayout layout;            /* the device's empty one in that case */
   VkPipeline pipeline[RWA_PRIMITIVE_TYPES];
   struct rwa_use use;                 /* M4-1: the last draw of it */
};

/* How many threadgroup sizes one compute pipeline keeps a VkPipeline for (COMPUTE, THE
 * PIPELINE): a caller that alternates sizes pays a compile per size once and never a wait,
 * up to this many; past it the oldest is replaced after waiting for the GPU. */
#define RWA_COMPUTE_SIZES 8

/* A compute pipeline (COMPUTE, M3d): the translation, lowered once, and one VkPipeline per
 * threadgroup size a dispatch has asked for. */
struct rwa_compute {
   struct rwa_device *device;
   void *mem_ctx;                 /* the NIR, the frontend's info and the stage's bindings */
   nir_shader *nir;               /* lowered and untouched after; a size compiles a clone */
   struct air_shader_info info;
   struct rwa_compute_info caps;  /* what rwa_compute_create reported */
   bool dynamic_threadgroup;      /* a threadgroup buffer sized by the dispatch */
   struct rwa_stage stage;
   VkDescriptorSetLayout set_layout;   /* NULL when the kernel binds nothing */
   VkPipelineLayout layout;            /* the device's empty one in that case */
   bool ordinary_set;                  /* M5c: more than a push set holds */
   unsigned sizes;                /* how many of size[] are made */
   unsigned replace;              /* the one replaced next once all of them are */
   struct {
      unsigned block[3];
      VkPipeline pipeline;
   } size[RWA_COMPUTE_SIZES];
   struct rwa_use use;            /* M4-1: the last dispatch of it, whatever its size */
};

/* The counts rwa_vk_counts returns: command buffers in use, fences in use, buffers,
 * images. Process-wide and atomic, because rwa_fence_release may run on any thread.
 */
static int32_t rwa_live[4];

enum { RWA_LIVE_CMD, RWA_LIVE_FENCE, RWA_LIVE_BUFFER, RWA_LIVE_IMAGE };

/* PRINTFLIKE, as in air_metal.c: these strings are the product, and nothing else checks
 * that their arguments match.
 */
static void say(char *why, size_t why_size, const char *fmt, ...) PRINTFLIKE(3, 4);

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
   if (!why || !why_size)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

/* What every call outside M3b answers. One sentence shape for all of them, so a log can
 * be searched for "(M3b)" and every refusal of this kind found at once. (The three depth
 * calls that used to answer it -- rwa_clear_depth_stencil, rwa_depth_read and
 * rwa_depth_stencil_create -- are real since M3c-2, and rwa_resolve_texture since M3c-3.)
 */
static void
not_yet(char *why, size_t why_size, const char *what, const char *call)
{
   say(why, why_size, "%s (%s) is not in the Vulkan back end yet (M3b)", what, call);
}

void
rwa_vk_counts(unsigned counts[4])
{
   if (!counts)
      return;
   for (unsigned i = 0; i < 4; i++)
      counts[i] = (unsigned)MAX2(p_atomic_read(&rwa_live[i]), 0);
}

/* ---------------------------------------------------------------------------------- */
/* Memory this library owns, the command buffer, its fence and the pool              */
/* ---------------------------------------------------------------------------------- */

/* A memory type out of `bits` that has every flag of `need`, preferring one that also has
 * `want`; UINT32_MAX when there is none.
 */
static uint32_t
memory_type(const struct rwa_device *device, uint32_t bits, VkMemoryPropertyFlags need,
            VkMemoryPropertyFlags want)
{
   uint32_t found = UINT32_MAX;
   for (uint32_t i = 0; i < device->memory.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = device->memory.memoryTypes[i].propertyFlags;
      if (!(bits & (1u << i)) || (f & need) != need)
         continue;
      if ((f & want) == want)
         return i;
      if (found == UINT32_MAX)
         found = i;
   }
   return found;
}

/* A buffer both sides see, coherently, and the GPU's own memory where it is both: on this
 * Adreno memory type 0 is DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT, so the processor
 * writes what the GPU reads with nothing to flush, which is a Metal buffer's promise
 * (-didModifyRange: has nothing to do here either). Mapped once, for its whole life.
 */
static bool
raw_create(struct rwa_device *device, VkDeviceSize size, VkBufferUsageFlags usage,
           struct rwa_raw *raw, char *why, size_t why_size)
{
   memset(raw, 0, sizeof(*raw));
   raw->size = size;
   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult r = device->vk.CreateBuffer(device->dev, &bci, NULL, &raw->buffer);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a buffer of %llu bytes (%s)",
          (unsigned long long)size, vk_Result_to_str(r));
      raw->buffer = VK_NULL_HANDLE;
      return false;
   }
   VkMemoryRequirements req;
   device->vk.GetBufferMemoryRequirements(device->dev, raw->buffer, &req);
   uint32_t type = memory_type(device, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (type == UINT32_MAX) {
      say(why, why_size, "no memory type of %s both holds a buffer and is seen coherently by "
                         "the processor", device->name);
      device->vk.DestroyBuffer(device->dev, raw->buffer, NULL);
      raw->buffer = VK_NULL_HANDLE;
      return false;
   }
   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if ((r = device->vk.AllocateMemory(device->dev, &mai, NULL, &raw->memory)) != VK_SUCCESS ||
       (r = device->vk.BindBufferMemory(device->dev, raw->buffer, raw->memory, 0)) !=
          VK_SUCCESS ||
       (r = device->vk.MapMemory(device->dev, raw->memory, 0, VK_WHOLE_SIZE, 0,
                                 &raw->map)) != VK_SUCCESS) {
      say(why, why_size, "%llu bytes could not be allocated, bound and mapped for the "
                         "processor (%s)", (unsigned long long)size, vk_Result_to_str(r));
      device->vk.DestroyBuffer(device->dev, raw->buffer, NULL);
      if (raw->memory)
         device->vk.FreeMemory(device->dev, raw->memory, NULL);
      memset(raw, 0, sizeof(*raw));
      return false;
   }
   return true;
}

static void
raw_destroy(struct rwa_device *device, struct rwa_raw *raw)
{
   if (!raw->buffer)
      return;
   device->vk.UnmapMemory(device->dev, raw->memory);
   device->vk.DestroyBuffer(device->dev, raw->buffer, NULL);
   device->vk.FreeMemory(device->dev, raw->memory, NULL);
   memset(raw, 0, sizeof(*raw));
}

/* Every write recorded so far made visible to every access recorded after it, whatever
 * the stages: the wide barrier RESOURCES, SYNCHRONISATION speaks of. Cheap at M3b's
 * handful of commands a frame, and never the narrow one that is sometimes right.
 */
static void
barrier_all(struct rwa_device *device, VkCommandBuffer cmd)
{
   const VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
   };
   device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0,
                                 NULL);
}

static void pair_unref_locked(struct rwa_device *device, struct rwa_fence *s);

/* Whether the work that named an object is still to finish, by the numbers reap has reached. */
static bool
use_busy_locked(const struct rwa_device *device, struct rwa_use use)
{
   return use.recording > device->recording_done || use.upload > device->upload_done;
}

/*
 * ONE MECHANISM, M4-1: everything that waits for a fence, waits here. A pair comes off the flight
 * list when its fence has signalled, oldest first, and a fence covers every earlier submission,
 * so the first one not yet signalled ends the look: at most one request of the kernel's goes
 * unanswered per call, where record_begin used to ask of every pair given back. That moves three
 * numbers, and each is what somebody is waiting for: a pair is reused only when `signalled`
 * says so (record_begin, as it only ever reused one whose fence had signalled); what a destroy
 * left behind goes when its numbers are passed (when_done); and the ring may be written again as
 * far as it was handed out by then (THE STAGING RING). Any thread may call this: everything it
 * frees is the library's alone by then, and nothing here touches a pool or the queue. A fence's
 * status is a question of the kernel's at worst, and short, so it is asked under the lock.
 */
static void
reap(struct rwa_device *device)
{
   struct rwa_after *go = NULL;
   bool moved = false;

   pthread_mutex_lock(&device->lock);
   for (struct rwa_fence *s; (s = device->flight_first) != NULL;) {
      if (device->vk.GetFenceStatus(device->dev, s->fence) != VK_SUCCESS)
         break;                   /* (not yet; or a lost device, which close deals with) */
      device->flight_first = s->flight_next;
      if (!device->flight_first)
         device->flight_last = NULL;
      s->flight_next = NULL;
      s->signalled = true;
      if (s->recording)
         device->recording_done = s->recording;
      if (s->upload)
         device->upload_done = s->upload;
      if (s->ring_end)
         device->ring_done = s->ring_end;
      moved = true;
   }
   if (moved) {
      for (struct rwa_after **p = &device->after; *p;) {
         struct rwa_after *a = *p;
         if (use_busy_locked(device, a->use)) {
            p = &a->next;
         } else {
            *p = a->next;
            a->next = go;
            go = a;
         }
      }
   }
   pthread_mutex_unlock(&device->lock);

   while (go) {
      struct rwa_after *a = go;
      go = a->next;
      a->free(device, a->object);
      free(a);
   }
}

/* A pair for a submission, new or reused, its fence reset, held by nobody yet. Its command buffer
 * is not looked at: that is the recording side's alone (record_begin), and an upload's pair does
 * not use it. A pair is reused once nobody holds it and its fence has signalled. NULL and a
 * sentence. */
static struct rwa_fence *
pair_take(struct rwa_device *device, char *why, size_t why_size)
{
   reap(device);

   struct rwa_fence *s = NULL;
   pthread_mutex_lock(&device->lock);
   for (struct rwa_fence **p = &device->returned; *p; p = &(*p)->next) {
      if ((*p)->signalled) {
         s = *p;
         *p = s->next;
         s->next = NULL;
         s->signalled = false;
         s->submitted = false;
         if (s->ucmd) {
            s->ucmd->next = device->ucmd_free;
            device->ucmd_free = s->ucmd;
            s->ucmd = NULL;
         }
         break;
      }
   }
   pthread_mutex_unlock(&device->lock);

   if (s) {
      device->vk.ResetFences(device->dev, 1, &s->fence);
      s->recording = s->upload = s->ring_end = 0;
      return s;
   }
   s = calloc(1, sizeof(*s));
   if (!s) {
      say(why, why_size, "out of memory making a command buffer");
      return NULL;
   }
   const VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkResult r = device->vk.CreateFence(device->dev, &fi, NULL, &s->fence);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a fence (%s)", vk_Result_to_str(r));
      free(s);
      return NULL;
   }
   pthread_mutex_lock(&device->lock);
   s->all = device->all;
   device->all = s;
   pthread_mutex_unlock(&device->lock);
   return s;
}

/* M5c: the ordinary descriptor sets of one recording (ARGUMENT BUFFERS, MORE THAN A PUSH SET HOLDS).
 * The pools it filled go when its pair comes round again, and the last one is kept and reset. */
static void
sets_reset(struct rwa_device *device, struct rwa_fence *s)
{
   for (unsigned i = 0; i < s->full_set_count; i++)
      device->vk.DestroyDescriptorPool(device->dev, s->full_sets[i], NULL);
   s->full_set_count = 0;
   if (s->sets)
      device->vk.ResetDescriptorPool(device->dev, s->sets, 0);
}

static VkDescriptorSet
ordinary_set(struct rwa_device *device, struct rwa_fence *s, VkDescriptorSetLayout layout, char *why,
             size_t why_size)
{
   for (unsigned attempt = 0; attempt < 2; attempt++) {
      if (s->sets) {
         const VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = s->sets,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
         };
         VkDescriptorSet set = VK_NULL_HANDLE;
         VkResult r = device->vk.AllocateDescriptorSets(device->dev, &ai, &set);
         if (r == VK_SUCCESS)
            return set;
         if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            say(why, why_size, "turnip would not allocate a descriptor set (%s)",
                vk_Result_to_str(r));
            return VK_NULL_HANDLE;
         }
         VkDescriptorPool *grown = realloc(s->full_sets, (s->full_set_count + 1) * sizeof(*grown));
         if (!grown) {
            say(why, why_size, "out of memory keeping a descriptor pool");
            return VK_NULL_HANDLE;
         }
         s->full_sets = grown;
         s->full_sets[s->full_set_count++] = s->sets;
         s->sets = VK_NULL_HANDLE;
      }
      static const VkDescriptorPoolSize sizes[] = {
         { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 },
         { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 },
         { VK_DESCRIPTOR_TYPE_SAMPLER, 2048 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 512 },
      };
      const VkDescriptorPoolCreateInfo ci = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 64,
         .poolSizeCount = ARRAY_SIZE(sizes),
         .pPoolSizes = sizes,
      };
      VkResult r = device->vk.CreateDescriptorPool(device->dev, &ci, NULL, &s->sets);
      if (r != VK_SUCCESS) {
         s->sets = VK_NULL_HANDLE;
         say(why, why_size, "turnip would not make a descriptor pool (%s)", vk_Result_to_str(r));
         return VK_NULL_HANDLE;
      }
   }
   say(why, why_size, "a new descriptor pool would not give one set of this layout");
   return VK_NULL_HANDLE;
}

/* The descriptors of a draw or a dispatch, in whichever kind of set its layout is. */
static bool
bind_descriptors(struct rwa_device *device, struct rwa_fence *cmd, VkPipelineBindPoint point,
                 bool ordinary, VkDescriptorSetLayout set_layout, VkPipelineLayout layout,
                 unsigned total, VkWriteDescriptorSet *writes, char *why, size_t why_size)
{
   /* [M5c: a descriptor of an array or a gated binding that nothing fills has no write
    * (rwa_draw_binding.absent): the shader does not reach it at run time] */
   unsigned n = 0;
   for (unsigned i = 0; i < total; i++)
      if (writes[i].sType)
         writes[n++] = writes[i];
   total = n;
   if (!total)
      return true;
   if (!ordinary) {
      device->CmdPushDescriptorSet(cmd->cmd, point, layout, 0, total, writes);
      return true;
   }
   VkDescriptorSet set = ordinary_set(device, cmd, set_layout, why, why_size);
   if (!set)
      return false;
   for (unsigned i = 0; i < total; i++)
      writes[i].dstSet = set;
   device->vk.UpdateDescriptorSets(device->dev, total, writes, 0, NULL);
   device->vk.CmdBindDescriptorSets(cmd->cmd, point, layout, 0, 1, &set, 0, NULL);
   return true;
}

/* A pair, new or reused, begun for recording. NULL and a sentence. */
static struct rwa_fence *
record_begin(struct rwa_device *device, char *why, size_t why_size)
{
   if (device->current) {
      /* [M4-1, the cost part: a recording stopped at an event's wait goes on from whichever thread
       * signalled it (-hkRun), and that thread is the recorder from now on, or the first one
       * would still take it for its own and submit it from under this one. There is one narrow
       * road left, and it is the application's hazard twice over: the first thread writing or
       * reading a texture that recording has named, in the instant the signal arrives elsewhere.
       * rw_pipe_lock around -replaceRegion: and -getBytes: would close it, and is hkmetal.m's.] */
      if (!pthread_equal(device->recorder, pthread_self())) {
         pthread_mutex_lock(&device->lock);
         device->recorder = pthread_self();
         pthread_mutex_unlock(&device->lock);
      }
      return device->current;
   }

   /* A pair given back whose work is done, if there is one: its fence has signalled, so
    * nothing on the GPU still reads the command buffer or its upload memory, and nobody
    * else holds it.
    */
   struct rwa_fence *s = pair_take(device, why, why_size);
   if (!s)
      return NULL;

   VkResult r;
   if (s->cmd) {
      device->vk.ResetCommandBuffer(s->cmd, 0);
      for (struct rwa_upload *u = s->uploads; u; u = u->next)
         u->used = 0;
      sets_reset(device, s);
   } else {
      const VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = device->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      if ((r = device->vk.AllocateCommandBuffers(device->dev, &ai, &s->cmd)) != VK_SUCCESS) {
         say(why, why_size, "turnip would not allocate a command buffer (%s)",
             vk_Result_to_str(r));
         s->cmd = VK_NULL_HANDLE;
         /* back on the list, where close finds it: its fence is unsignalled, so it stays there */
         s->refs = 1;
         pthread_mutex_lock(&device->lock);
         pair_unref_locked(device, s);
         pthread_mutex_unlock(&device->lock);
         return NULL;
      }
   }
   s->refs = 1;                   /* the recording's; it becomes the fence rwa_flush hands out */
   p_atomic_inc(&device->pairs_out);
   p_atomic_inc(&rwa_live[RWA_LIVE_CMD]);
   p_atomic_inc(&rwa_live[RWA_LIVE_FENCE]);

   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if ((r = device->vk.BeginCommandBuffer(s->cmd, &bi)) != VK_SUCCESS) {
      say(why, why_size, "turnip would not begin a command buffer (%s)", vk_Result_to_str(r));
      /* back where it came from; its fence is unsignalled now, so it waits there for
       * close rather than being reused */
      rwa_fence_release(device, s);
      return NULL;
   }
   pthread_mutex_lock(&device->lock);
   s->recording = ++device->recordings;
   device->current = s;
   device->recorder = pthread_self();
   pthread_mutex_unlock(&device->lock);
   return s;
}

/* Under the lock: one holder lets go of a pair, and the last one puts it back on the list
 * record_begin takes from -- which reuses it only once its fence has signalled, whoever let
 * go and however early (the rules in ONE COMMAND BUFFER AT A TIME).
 */
static void
pair_unref_locked(struct rwa_device *device, struct rwa_fence *s)
{
   assert(s->refs > 0);
   if (--s->refs == 0) {
      s->next = device->returned;
      device->returned = s;
   }
}

/* What this file's own holder of a pair does to let go of it: the fence rwa_flush hands out is
 * counted (pairs_out, rwa_live) and goes back through rwa_fence_release; this is not, and does
 * not. */
static void
pair_release(struct rwa_device *device, struct rwa_fence *s)
{
   pthread_mutex_lock(&device->lock);
   pair_unref_locked(device, s);
   pthread_mutex_unlock(&device->lock);
}

/* Who is named by the recording being made, for when_done. */
static inline void
named(struct rwa_use *use, const struct rwa_fence *s)
{
   use->recording = s->recording;
}

/*
 * UPLOADS, AHEAD OF THE RECORDING (M4-1, the cost part): -replaceRegion: without a submission and a
 * wait of its own.
 *
 * WHY. run 071517's --draw-all made 2021 submissions where Gallium made 278, and --compute-all
 * 11.8k and 36.1k against 349 and 895: every rwa_texture_write was a submission and a wait, and the
 * test writes a texture a row at a time, as applications do. A submission costs the kernel's whole
 * path. So the bytes go into the ring now, and the buffer->image copy into a command buffer
 * of its own, the upload command buffer, and that goes to the queue with whatever goes next.
 *
 * THE ORDER. Metal's rule is that a -replaceRegion: has taken effect for any work encoded after it
 * and not committed yet, and for work encoded BEFORE it and not committed yet as well: to the
 * application the write is a memcpy into shared memory, and a command buffer runs after its
 * -commit, seeing what is in memory then. hkmetal.m gives this file a command buffer's work only
 * at -commit (-hkRun, under rw_pipe_lock), so to this file both are simply work recorded after the
 * write. So the upload goes AHEAD of the recording: at the next flush, on the same queue, one
 * vkQueueSubmit of two command buffers, upload first, one fence, one kernel submission -- ending in
 * a barrier that makes the copies visible to everything after (submit_locked). That is
 * air_metal.c's order: its texture_subdata comes before whatever is recorded next as well.
 *
 * What goes first must then be what came first, and three things stand in the way of that.
 *
 *   What the same thread has already recorded in the open recording and named this texture in
 *   (a test using the contract directly; a command buffer stopped at an event's wait,
 *   which leaves its recording open). That work came before the write, so it must run before: the
 *   recording is submitted first (flush_mine) and the upload goes after it, in the next
 *   submission, in order. A recording that has not named the texture has nothing to order against.
 *
 *   What ANOTHER thread has recorded and named this texture in (a -commit in progress, or stopped
 *   at an event). This thread may not touch that recording (the rule below), and no Metal
 *   application can tell: it has a texture in use by committed work and is writing it, which Metal
 *   makes the application's own hazard. So the write goes ahead of that recording too, and the one
 *   thing that must stay true is the layouts, which are this library's own promise to Vulkan
 *   whatever the application did: that recording's first barrier on the image said it found the
 *   image in `layout_at_start`, so the upload moves it from there and BACK there
 *   (upload_image_begin, upload_image_end). Two threads writing one texture at once get the same
 *   treatment and the application's own answer.
 *
 *   A texture over an IOSurface. Its memory is also read by whoever holds the surface -- the
 *   processor through IOSurfaceGetBaseAddress, CoreAnimation, another process -- and none of them
 *   comes through a flush of this library. And with no rwa_device_defer_waits (air_run,
 *   air_metal_test, boot-arg metal_sync=1) every call has promised to be done when it returns. In
 *   both cases the upload goes to the queue at once and is waited for, which is the old promise,
 *   at the old cost: "the texture holds the bytes when this returns".
 *
 * AND A READ, rwa_texture_read and rwa_depth_read, must see every earlier write, and returns bytes:
 * the copy out goes into the upload command buffer too, after the writes already there, and the
 * upload goes to the queue and is waited for right then; this thread's own recording first
 * (flush_mine), because a read also comes after the draws this thread recorded. It does not wait
 * for a recording open on another thread, which is not "earlier" -- its -commit has not returned.
 * On -getBytes: from a thread that is not recording this closes what M4-1's first part left open:
 * it no longer goes near `current`.
 *
 * THREADS. -replaceRegion: and -getBytes: come from any thread and take no rw_pipe_lock, so
 * none of this may touch a recording, and the rule of ONE COMMAND BUFFER AT A TIME is kept as it
 * was: only the thread that began a recording records into it or submits it. The upload command
 * buffer is the one thing any thread may record into, under upload_lock and from its own pool
 * (recording takes the pool's external synchronisation, and the recording thread is using the
 * other one meanwhile). upload_lock is also around every vkQueueSubmit, the recording's included,
 * since an upload alone may go to the queue from any thread (upload_submit_locked: a read, a
 * surface, a flush with nothing recorded, a full ring). Order of the locks: upload_lock, then
 * `lock`. image_barrier takes `lock`, because a layout may now be read from a second thread.
 *
 * Because a flush takes upload_lock before it clears `current`, what an upload reads of the open
 * recording is true all the way to the queue: either the recording is still open and goes after
 * this upload, or it has gone and this upload is the next one.
 */

/* Open the upload command buffer if it is not, under upload_lock. NULL and a sentence. */
static struct rwa_ucmd *
upload_begin(struct rwa_device *device, char *why, size_t why_size)
{
   if (device->uploading)
      return device->uploading;

   pthread_mutex_lock(&device->lock);
   struct rwa_ucmd *u = device->ucmd_free;
   if (u)
      device->ucmd_free = u->next;
   pthread_mutex_unlock(&device->lock);

   VkResult r;
   if (u) {
      device->vk.ResetCommandBuffer(u->cmd, 0);
   } else {
      u = calloc(1, sizeof(*u));
      if (!u) {
         say(why, why_size, "out of memory making an upload command buffer");
         return NULL;
      }
      const VkCommandBufferAllocateInfo ai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = device->upload_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      if ((r = device->vk.AllocateCommandBuffers(device->dev, &ai, &u->cmd)) != VK_SUCCESS) {
         say(why, why_size, "turnip would not allocate an upload command buffer (%s)",
             vk_Result_to_str(r));
         free(u);
         return NULL;
      }
   }
   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if ((r = device->vk.BeginCommandBuffer(u->cmd, &bi)) != VK_SUCCESS) {
      say(why, why_size, "turnip would not begin an upload command buffer (%s)",
          vk_Result_to_str(r));
      pthread_mutex_lock(&device->lock);
      u->next = device->ucmd_free;
      device->ucmd_free = u;
      pthread_mutex_unlock(&device->lock);
      return NULL;
   }
   device->uploads++;
   device->uploading = u;
   return u;
}

/* What the GPU wrote made visible to the processor (record_submit says why). */
static void
barrier_to_host(struct rwa_device *device, VkCommandBuffer cmd)
{
   const VkMemoryBarrier host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
}

/*
 * To the queue, under upload_lock, as ONE submission: the upload command buffer if one is open,
 * and then the pair's recording if it is one (its `recording` number says so, and it has ended).
 * Then the pair is the newest submission, and on the flight list. A failure leaves the pair with
 * its one holder, its fence never to signal; and what it carried is lost, which the sentence says.
 * *out is the pair, or NULL.
 */
static int
submit_locked(struct rwa_device *device, struct rwa_fence *s, struct rwa_fence **out, char *why,
              size_t why_size)
{
   VkCommandBuffer cmds[2];
   uint32_t n = 0;
   VkResult r = VK_SUCCESS;
   struct rwa_ucmd *u = device->uploading;

   *out = NULL;
   if (u) {
      /* Everything the copies wrote visible to everything after them, whatever the stages: the
       * recording that follows starts every use with a barrier of its own as well, and this one
       * is what a later submission of anything else can rely on. And to the processor, for a
       * read-back: the last act of this command buffer in either case. */
      barrier_all(device, u->cmd);
      barrier_to_host(device, u->cmd);
      r = device->vk.EndCommandBuffer(u->cmd);
      device->uploading = NULL;
      s->ucmd = u;
      s->upload = device->uploads;
      s->ring_end = device->ring_head;
      cmds[n++] = u->cmd;
   }
   if (s->recording)
      cmds[n++] = s->cmd;
   if (r == VK_SUCCESS) {
      const VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = n,
         .pCommandBuffers = cmds,
      };
      r = device->vk.QueueSubmit(device->queue, 1, &si, s->fence);
   }
   if (r != VK_SUCCESS) {
      /* Never submitted, so its fence never signals and the pair cannot be reused; it
       * goes back to the list, where rwa_device_close finds it, and the recorded work is
       * lost -- which is what the sentence says.
       */
      say(why, why_size, "turnip would not submit the recorded work (%s); none of it ran",
          vk_Result_to_str(r));
      return -1;
   }
   /* The newest submission now, so its fence is what waiting for everything means. Only a
    * submission that reached the queue gets here: a fence that was never submitted never
    * signals, and nobody may be sent to wait for one. */
   pthread_mutex_lock(&device->lock);
   s->submitted = true;
   s->refs++;
   struct rwa_fence *older = device->last;
   device->last = s;
   if (older)
      pair_unref_locked(device, older);
   if (device->flight_last)
      device->flight_last->flight_next = s;
   else
      device->flight_first = s;
   device->flight_last = s;
   pthread_mutex_unlock(&device->lock);
   *out = s;
   return 0;
}

/* Whatever is in the upload command buffer to the queue on its own, under upload_lock, from any
 * thread: ahead of a recording still open, which is where it always goes. *out is its pair, held
 * by the caller for pair_release, or NULL when there was nothing to send. */
static int
upload_submit_locked(struct rwa_device *device, struct rwa_fence **out, char *why, size_t why_size)
{
   *out = NULL;
   if (!device->uploading)
      return 0;
   struct rwa_fence *s = pair_take(device, why, why_size);
   if (!s)
      return -1;
   s->refs = 1;
   if (submit_locked(device, s, out, why, why_size) != 0) {
      pair_release(device, s);
      return -1;
   }
   return 0;
}

/* Submit what is recorded and hand out its pair; *out NULL when nothing was. */
static int
record_submit(struct rwa_device *device, struct rwa_fence **out, char *why, size_t why_size)
{
   struct rwa_fence *s = device->current;
   *out = NULL;
   if (!s)
      return 0;
   /* [M4-1, the cost part: upload_lock from before `current` goes, UPLOADS says why.] */
   pthread_mutex_lock(&device->upload_lock);
   pthread_mutex_lock(&device->lock);
   device->current = NULL;
   pthread_mutex_unlock(&device->lock);

   /* The GPU's writes made visible to the processor, at the end of every command buffer:
    * a Metal buffer the GPU wrote is read through its pointer after -waitUntilCompleted,
    * and on Vulkan the fence alone does not make device writes visible to the host.
    */
   barrier_to_host(device, s->cmd);

   VkResult r = device->vk.EndCommandBuffer(s->cmd);
   int rc;
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not end the recorded work (%s); none of it ran",
          vk_Result_to_str(r));
      rc = -1;
   } else {
      rc = submit_locked(device, s, out, why, why_size);
   }
   pthread_mutex_unlock(&device->upload_lock);
   if (rc != 0)
      rwa_fence_release(device, s);
   return rc;
}

/* This thread's recording, if it is recording, to the queue with no wait: the same fence as any,
 * given straight back. What comes next from this thread then runs after what it recorded. */
static int
flush_mine(struct rwa_device *device, char *why, size_t why_size)
{
   pthread_mutex_lock(&device->lock);
   bool mine = device->current && pthread_equal(device->recorder, pthread_self());
   pthread_mutex_unlock(&device->lock);
   struct rwa_fence *s = NULL;
   if (!mine || record_submit(device, &s, why, why_size) != 0)
      return mine ? -1 : 0;
   rwa_fence_release(device, s);
   return 0;
}

/* Submit what is recorded -- or, when nothing is, an empty command buffer, because its
 * fence then covers every earlier submission (a fence of vkQueueSubmit includes all
 * commands earlier in submission order) -- wait for it, and give the pair back. This is
 * air_metal.c's finish_or_say_why.
 */
static int
submit_and_wait(struct rwa_device *device, const char *what, char *why, size_t why_size)
{
   struct rwa_fence *s;
   if (!record_begin(device, why, why_size) || record_submit(device, &s, why, why_size) != 0)
      return -1;
   int rc = rwa_fence_wait(device, s, why, why_size);
   if (rc != 0 && why && why_size) {
      char inner[256];
      snprintf(inner, sizeof(inner), "%s", why);
      say(why, why_size, "the %s: %s", what, inner);
   }
   rwa_fence_release(device, s);
   return rc;
}

/* The end of a call that recorded something: nothing more when the driver collects its
 * own fence at rwa_flush, otherwise this call's own submission and wait.
 */
static int
recorded(struct rwa_device *device, const char *what, char *why, size_t why_size)
{
   return device->defer_waits ? 0 : submit_and_wait(device, what, why, why_size);
}

/*
 * Everything submitted so far finished, from ANY thread, and the only way in that may be on a
 * thread other than the contract's (M4-1, ONE COMMAND BUFFER AT A TIME).
 *
 * A thread that is recording submits what it recorded and waits for that, as submit_and_wait
 * always did: an object it is about to destroy may be named in its own recording. Any other
 * thread records nothing and waits for the newest submission's fence, under a reference so
 * that the pair cannot be reset under the wait when a newer submission takes over
 * device->last. A recording in progress on some other thread is not waited for and is not
 * touched: it is not submitted yet, so it is not "so far", and what that thread names it holds
 * a reference to, so it cannot be what the caller is destroying. Nothing submitted at all is
 * nothing to wait for.
 *
 * This is the same promise the empty command buffer made: its fence covered every earlier
 * submission for the same reason device->last's does, and every command buffer ends in the
 * host barrier, so a pair already waited for has made its writes visible as well.
 *
 * [M4-1, the cost part: and an upload, which any thread may send, goes to the queue first: "so far"
 * has included every -replaceRegion: that has returned, and it still does.]
 */
static int
wait_for_all(struct rwa_device *device, const char *what, char *why, size_t why_size)
{
   pthread_mutex_lock(&device->lock);
   bool mine = device->current && pthread_equal(device->recorder, pthread_self());
   pthread_mutex_unlock(&device->lock);

   if (mine)
      return submit_and_wait(device, what, why, why_size);

   struct rwa_fence *u;
   pthread_mutex_lock(&device->upload_lock);
   int sent = upload_submit_locked(device, &u, why, why_size);
   pthread_mutex_unlock(&device->upload_lock);
   if (sent != 0)
      return -1;
   if (u)
      pair_release(device, u);

   pthread_mutex_lock(&device->lock);
   struct rwa_fence *s = device->last;
   if (s)
      s->refs++;
   pthread_mutex_unlock(&device->lock);

   if (!s)
      return 0;
   int rc = rwa_fence_wait(device, s, why, why_size);
   if (rc != 0 && why && why_size) {
      char inner[256];
      snprintf(inner, sizeof(inner), "%s", why);
      say(why, why_size, "the %s: %s", what, inner);
   }
   pthread_mutex_lock(&device->lock);
   pair_unref_locked(device, s);
   pthread_mutex_unlock(&device->lock);
   return rc;
}

int
rwa_finish(struct rwa_device *device, char *why, size_t why_size)
{
   return wait_for_all(device, "work", why, why_size);
}

void
rwa_device_defer_waits(struct rwa_device *device, int defer)
{
   device->defer_waits = defer != 0;
}

int
rwa_flush(struct rwa_device *device, struct rwa_fence **fence, char *why, size_t why_size)
{
   if (device->current)
      return record_submit(device, fence, why, why_size);
   /* [M4-1, the cost part: nothing recorded, and an upload waiting, goes now, and that is the
    * fence: a Metal command buffer with no work in it still completes after every
    * -replaceRegion: before its -commit.] */
   struct rwa_fence *u;
   pthread_mutex_lock(&device->upload_lock);
   int sent = upload_submit_locked(device, &u, why, why_size);
   pthread_mutex_unlock(&device->upload_lock);
   if (sent != 0)
      return -1;
   if (u) {
      p_atomic_inc(&device->pairs_out);
      p_atomic_inc(&rwa_live[RWA_LIVE_CMD]);
      p_atomic_inc(&rwa_live[RWA_LIVE_FENCE]);
      *fence = u;
      return 0;
   }
   /* Nothing recorded, and the fence is the newest submission's -- what freedreno's flush
    * gives for an empty batch (ctx->last_fence), and the reason Gallium never reached the
    * road this back end faulted on: a NULL here sent hkmetal.m's completion queue to
    * rwa_finish for every empty command buffer. NULL now only when nothing was ever
    * submitted, and then "nothing to wait for" is simply true. It is counted like any
    * fence handed out: rwa_fence_release gives it back.
    */
   pthread_mutex_lock(&device->lock);
   struct rwa_fence *s = device->last;
   if (s)
      s->refs++;
   pthread_mutex_unlock(&device->lock);
   if (s) {
      p_atomic_inc(&device->pairs_out);
      p_atomic_inc(&rwa_live[RWA_LIVE_CMD]);
      p_atomic_inc(&rwa_live[RWA_LIVE_FENCE]);
   }
   *fence = s;
   return 0;
}

int
rwa_fence_wait(struct rwa_device *device, struct rwa_fence *fence, char *why,
               size_t why_size)
{
   if (!fence)
      return 0;
   /* vkWaitForFences needs no external synchronisation, so this may run on any thread
    * and more than once, which is what the contract promises */
   VkResult r = device->vk.WaitForFences(device->dev, 1, &fence->fence, VK_TRUE,
                                         RWA_FENCE_TIMEOUT_NS);
   if (r == VK_SUCCESS)
      return 0;
   if (r == VK_TIMEOUT)
      say(why, why_size, "the GPU did not finish a submission within five seconds; what it "
                         "wrote is not a result, and one timeout wedges this GPU for the "
                         "rest of the boot (docs/air.md section 5)");
   else
      say(why, why_size, "waiting for a submission failed (%s), so whether the GPU finished "
                         "it is not known and what it wrote is not a result",
          vk_Result_to_str(r));
   return -1;
}

void
rwa_fence_release(struct rwa_device *device, struct rwa_fence *fence)
{
   if (!fence)
      return;
   pthread_mutex_lock(&device->lock);
   pair_unref_locked(device, fence);
   /* [M4-1, the cost part: often the moment the work a destroy left behind has been waiting for] */
   bool waiting = device->after != NULL;
   pthread_mutex_unlock(&device->lock);
   p_atomic_dec(&device->pairs_out);
   p_atomic_dec(&rwa_live[RWA_LIVE_CMD]);
   p_atomic_dec(&rwa_live[RWA_LIVE_FENCE]);
   if (waiting)
      reap(device);
}

/* [M4-1, the cost part: this wait is no longer what a destroy does: when_done, below, is. It is left
 * for rwa_device_close, and for a destroy with no memory to wait with.]
 * Everything submitted so far finished, before an object the GPU may still be reading is
 * destroyed. Vulkan has no reference counts on its objects -- Gallium's resources do, and
 * that is what lets air_metal.c destroy a texture a batch still holds -- so a texture,
 * buffer, sampler or pipeline this library destroys is waited for first. A wait per
 * destroy is slow and M3b does not care; a GPU reading freed memory is a fault on this
 * device.
 * [M4-1: wait_for_all and no longer submit_and_wait: a destroy runs wherever the last
 * reference goes, hkmetal.m's completion queue included, and that thread may not record.
 * The wait is the same one, for everything submitted so far, and what it destroys still goes
 * only after it: a Metal application releases a resource right after -commit, the command
 * buffer's work items hold it until the buffer goes, and the buffer goes after its fence.]
 */
static void
idle(struct rwa_device *device)
{
   char why[256];
   (void)wait_for_all(device, "wait before a destroy", why, sizeof(why));
}

/*
 * DESTROYS WITH NO WAIT (M4-1, the cost part). idle above made every destroy a wait for everything
 * submitted so far, and on the way out of a --draw-all that was a wait per object; the old
 * comment over it says this library did not care yet. The promise is the same -- the GPU never
 * reads what has been freed -- and the mechanism is the pairs': `object` goes now if no unfinished
 * work ever named it, and otherwise when the fence of the last work that did has signalled
 * (reap). Nothing here waits, records or goes near the queue, so any thread may destroy
 * anything; a resource that was never in any submission never waited at all. The one wait left
 * is rwa_device_close's, and the one here that is only for memory running out, where the object
 * has nowhere to wait but here.
 *
 * What names it, and must say so (struct rwa_use, `named`): each recording call for what it
 * uses, image_barrier for a texture, every use of one goes through it; and the upload paths.
 * `use` is read here without the lock because the caller is the object's last owner, and no
 * further use of it can be on its way.
 */
static struct rwa_after *
after_add_locked(struct rwa_device *device, struct rwa_use use, rwa_free_fn fn, void *object)
{
   struct rwa_after *a = calloc(1, sizeof(*a));
   if (a) {
      a->use = use;
      a->free = fn;
      a->object = object;
      a->next = device->after;
      device->after = a;
   }
   return a;
}

static void
when_done(struct rwa_device *device, struct rwa_use use, rwa_free_fn fn, void *object)
{
   pthread_mutex_lock(&device->lock);
   bool busy = use_busy_locked(device, use);
   pthread_mutex_unlock(&device->lock);
   if (busy)
      reap(device);               /* it may be done by now, and nobody has looked */

   struct rwa_after *a = NULL;
   pthread_mutex_lock(&device->lock);
   busy = use_busy_locked(device, use);
   if (busy)
      a = after_add_locked(device, use, fn, object);
   pthread_mutex_unlock(&device->lock);
   if (a)
      return;
   if (busy)
      idle(device);
   fn(device, object);
}

/* The VkPipeline of a replaced size, and a buffer of the staging path's own: when_done's objects. */
static void
pipeline_free(struct rwa_device *device, void *object)
{
   device->vk.DestroyPipeline(device->dev, (VkPipeline)object, NULL);
}

static void
raw_free(struct rwa_device *device, void *object)
{
   raw_destroy(device, object);
   free(object);
}

/*
 * THE STAGING RING (M4-1, the cost part): one buffer, used over and over, for what the old code
 * made, mapped and closed a buffer for at every texture write and read.
 *
 * SIZE. 4 MB, made at the first use and not before, so a process that never uploads costs
 * nothing; it is one more buffer among what turnip names at each submission, O(1) in the kernel,
 * and it is no surface, so nothing cleans it there (HKGPU.cpp, submit). Not grown: what is too
 * big for it is rare and costs what it always did. A piece is on 64 bytes, which turnip's
 * copies like best (tu_copy_buffer_to_image) and every format's texel divides.
 *
 * WHO USES WHAT. All of it is handed out under upload_lock to the open upload command buffer: its
 * range lies between where the last upload ended and where its own does, and pair->ring_end
 * says that. Uploads reach the queue in that order, so the ring frees in the order it was used.
 *
 * RECLAIMED BY THE FENCE, reap, as a pair is and a destroyed object: when a pair comes off the
 * flight list, what it carried may be written again. A read-back holds the tail still from the
 * moment it takes its piece until its bytes are out (ring_readers), because signalled is when
 * the reader can START reading, and the fence would otherwise hand its bytes to the next writer.
 *
 * OVERFLOW. A piece over a quarter of the ring (1 MB) gets a buffer of its own, as before, freed
 * by the same mechanism: an upload's when its fence has signalled, a read-back's when the bytes
 * are out. A piece that fits when the ring is full: the upload command buffer goes to the queue
 * first (the ring is full of what it holds, perhaps, and it cannot finish while it stays here),
 * and the oldest submission that holds ring space is waited for, without upload_lock, again until
 * there is room: backpressure on whoever writes faster than the GPU runs, and the only wait in
 * the write path. Nothing left to wait for and still no room (only readers hold it) is a buffer
 * of its own too. It never fails while memory lasts, and never spins.
 *
 * On success upload_lock is HELD, and staging_done or the caller's recording comes before it goes.
 */
#define RWA_RING_SIZE (4ull << 20)
#define RWA_RING_BIGGEST (RWA_RING_SIZE / 4)
#define RWA_RING_ALIGN 64ull

static bool
ring_take(struct rwa_device *device, VkDeviceSize size, bool reading, struct rwa_staging *st)
{
   const VkDeviceSize n = (size + RWA_RING_ALIGN - 1) / RWA_RING_ALIGN * RWA_RING_ALIGN;
   const VkDeviceSize whole = device->ring.size;

   pthread_mutex_lock(&device->lock);
   if (device->ring_readers == 0 && device->ring_done > device->ring_tail)
      device->ring_tail = device->ring_done;
   pthread_mutex_unlock(&device->lock);

   /* a piece is one run of bytes: what is left before the end is skipped when it does not fit */
   const VkDeviceSize at = device->ring_head % whole;
   const VkDeviceSize skip = at + n > whole ? whole - at : 0;
   if (device->ring_head + skip + n - device->ring_tail > whole)
      return false;
   device->ring_head += skip;
   st->buffer = device->ring.buffer;
   st->offset = device->ring_head % whole;
   st->map = (uint8_t *)device->ring.map + st->offset;
   device->ring_head += n;
   if (reading) {
      st->reading = true;
      pthread_mutex_lock(&device->lock);
      device->ring_readers++;
      pthread_mutex_unlock(&device->lock);
   }
   return true;
}

static bool
staging_get(struct rwa_device *device, VkDeviceSize size, bool reading, struct rwa_staging *st,
            char *why, size_t why_size)
{
   memset(st, 0, sizeof(*st));
   pthread_mutex_lock(&device->upload_lock);
   if (size <= RWA_RING_BIGGEST &&
       (device->ring.buffer ||
        raw_create(device, RWA_RING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, &device->ring, NULL, 0))) {
      for (;;) {
         if (ring_take(device, size, reading, st))
            return true;
         reap(device);
         if (ring_take(device, size, reading, st))
            return true;
         if (device->uploading) {
            struct rwa_fence *u;
            if (upload_submit_locked(device, &u, why, why_size) != 0) {
               pthread_mutex_unlock(&device->upload_lock);
               return false;
            }
            pair_release(device, u);
            continue;
         }
         pthread_mutex_lock(&device->lock);
         struct rwa_fence *oldest = device->flight_first;
         while (oldest && !oldest->ring_end)
            oldest = oldest->flight_next;
         if (oldest)
            oldest->refs++;
         pthread_mutex_unlock(&device->lock);
         if (!oldest)
            break;
         pthread_mutex_unlock(&device->upload_lock);
         int rc = rwa_fence_wait(device, oldest, NULL, 0);
         pair_release(device, oldest);
         pthread_mutex_lock(&device->upload_lock);
         if (rc != 0)
            break;
      }
   }
   st->own = calloc(1, sizeof(*st->own));
   if (!st->own) {
      say(why, why_size, "out of memory making a staging buffer");
      pthread_mutex_unlock(&device->upload_lock);
      return false;
   }
   if (!raw_create(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, st->own, why, why_size)) {
      free(st->own);
      st->own = NULL;
      pthread_mutex_unlock(&device->upload_lock);
      return false;
   }
   st->buffer = st->own->buffer;
   st->map = st->own->map;
   return true;
}

/* A read-back's bytes are out, or a piece was never used: back it goes. Any time, no lock held. */
static void
staging_done(struct rwa_device *device, struct rwa_staging *st)
{
   if (st->reading) {
      pthread_mutex_lock(&device->lock);
      device->ring_readers--;
      pthread_mutex_unlock(&device->lock);
      st->reading = false;
   }
   if (st->own) {
      raw_free(device, st->own);
      st->own = NULL;
   }
}

/* `size` bytes of the current command buffer's upload memory, at an offset every
 * descriptor kind accepts. The memory lives until that command buffer's fence has
 * signalled and the pair is reused, which is exactly as long as the GPU reads it.
 */
static bool
upload_alloc(struct rwa_device *device, struct rwa_fence *s, VkDeviceSize size,
             VkBuffer *buffer, VkDeviceSize *offset, char *why, size_t why_size)
{
   VkDeviceSize align = MAX3(device->ubo_align, device->ssbo_align, 16);
   for (struct rwa_upload *u = s->uploads; u; u = u->next) {
      VkDeviceSize at = (u->used + align - 1) / align * align;
      if (at + size <= u->raw.size) {
         u->used = at + size;
         *buffer = u->raw.buffer;
         *offset = at;
         return true;
      }
   }
   struct rwa_upload *u = calloc(1, sizeof(*u));
   if (!u) {
      say(why, why_size, "out of memory making upload memory");
      return false;
   }
   if (!raw_create(device, MAX2(size, 65536),
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &u->raw, why, why_size)) {
      free(u);
      return false;
   }
   u->used = size;
   u->next = s->uploads;
   s->uploads = u;
   *buffer = u->raw.buffer;
   *offset = 0;
   return true;
}

/* ---------------------------------------------------------------------------------- */
/* The device                                                                         */
/* ---------------------------------------------------------------------------------- */

/* The one-time line of sample counts by format (MULTISAMPLE), which reads the format
 * table and so is written beside it, below. */
static void say_sample_counts(struct rwa_device *device);

#define RWA_VK_LOAD_INSTANCE(name)                                                    \
   if (!(device->vk.name = (PFN_vk##name)vk_icdGetInstanceProcAddr(device->instance,  \
                                                                   "vk" #name))) {     \
      say(why, why_size, "turnip has no vk" #name);                                   \
      goto fail;                                                                      \
   }

#define RWA_VK_LOAD_DEVICE(name)                                                      \
   if (!(device->vk.name = (PFN_vk##name)device->vk.GetDeviceProcAddr(device->dev,    \
                                                                      "vk" #name))) {  \
      say(why, why_size, "turnip has no vk" #name);                                   \
      goto fail;                                                                      \
   }

/*
 * What was still on the GPU when the process ended (M4-1): one line at exit, because run 071937
 * could not say whether a process left work behind it for the next one, and that was a question
 * (each --time was a new process, and the first of five did not fault).
 *
 *   pending-pairs N   given back by everyone and unsignalled: waiting on `returned` until the
 *                     GPU is done with them. Normal Metal, and exactly what must never be reset
 *   in-flight M       every pair submitted and unsignalled, N included, or whose device is lost
 *
 * Registered at the first rwa_device_open, which is after turnip's own exit work is registered
 * and so runs before it: atexit runs last-in first-out, and a fence's status needs a live
 * device. The same two numbers go into the environment, AIR_VK_EXIT, because that is the one
 * way to hand them to a test without an export (rwa_vk_counts is the only one a test has, and
 * its four numbers are read by logs back to M3a): metal_drv_test's own exit line, registered
 * before any device exists and so run after this one, reads them there.
 */
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rwa_device *open_devices;
static bool exit_line_registered;

static void
exit_line(void)
{
   unsigned pending = 0, in_flight = 0;

   pthread_mutex_lock(&open_lock);
   for (struct rwa_device *d = open_devices; d; d = d->open_next) {
      pthread_mutex_lock(&d->lock);
      for (struct rwa_fence *s = d->all; s; s = s->all) {
         if (!s->submitted || d->vk.GetFenceStatus(d->dev, s->fence) == VK_SUCCESS)
            continue;
         in_flight++;
         if (s->refs == 0)
            pending++;
      }
      pthread_mutex_unlock(&d->lock);
   }
   pthread_mutex_unlock(&open_lock);

   unsigned n[4];
   rwa_vk_counts(n);
   printf("air_vk: exit: pending-pairs %u in-flight %u (%u command buffers and %u fences "
          "still held, %u buffers and %u images alive)\n", pending, in_flight, n[RWA_LIVE_CMD],
          n[RWA_LIVE_FENCE], n[RWA_LIVE_BUFFER], n[RWA_LIVE_IMAGE]);
   fflush(stdout);

   char both[32];
   snprintf(both, sizeof(both), "%u %u", pending, in_flight);
   setenv("AIR_VK_EXIT", both, 1);
}

struct rwa_device *
rwa_device_open(char *why, size_t why_size)
{
   struct rwa_device *device = calloc(1, sizeof(*device));
   if (!device) {
      say(why, why_size, "out of memory making a device");
      return NULL;
   }
   pthread_mutex_init(&device->lock, NULL);
   pthread_mutex_init(&device->argbuf_lock, NULL);
   pthread_mutex_init(&device->upload_lock, NULL);

   /* NIR's type system, which the frontend builds types in. The instance holds a
    * reference of its own (vk_instance.c), so this one is for the frontend's sake and
    * is dropped at close, exactly as air_metal.c does it.
    */
   glsl_type_singleton_init_or_ref();

   VkResult r;
   /* Version 5 of the loader interface, as userland/src/vk_test.c asks for it: there is
    * no loader on iOS, so this library is its own, and the call costs nothing.
    */
   uint32_t icd_version = 5;
   vk_icdNegotiateLoaderICDInterfaceVersion(&icd_version);

   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "hkmetal",
      .pEngineName = "air_vk",
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (!create_instance ||
       (r = create_instance(&ici, NULL, &device->instance)) != VK_SUCCESS) {
      say(why, why_size, "turnip made no Vulkan instance (%s)",
          create_instance ? vk_Result_to_str(r) : "no vkCreateInstance");
      device->instance = VK_NULL_HANDLE;
      goto fail;
   }
   RWA_VK_INSTANCE_FNS(RWA_VK_LOAD_INSTANCE)

   /* The one physical device. None is the ordinary case of a kernel booted without
    * gpu=1: there is no HKGPU service for the stand-in for libdrm to open
    * (tools/mesa/hackios-drm), so turnip finds no msm device.
    */
   uint32_t count = 1;
   r = device->vk.EnumeratePhysicalDevices(device->instance, &count, &device->physical);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
      say(why, why_size, "there is no GPU here: turnip found no physical device, which is "
                         "what a kernel booted without gpu=1 looks like");
      goto fail;
   }

   /* The limits the resource path is decided by (RESOURCES), and maxPushDescriptors with
    * them, which only the properties2 chain carries.
    */
   /* And the depth/stencil resolve modes (MULTISAMPLE, M3c-3), which are 1.2's: asked
    * only of a device that says 1.2, because a structure of a version the device does not
    * have is not one it reads. The version comes from the plain properties first. */
   VkPhysicalDeviceProperties plain;
   device->vk.GetPhysicalDeviceProperties(device->physical, &plain);
   /* And the wave sizes a compute stage may run at (COMPUTE, THE WAVE): 1.3's
    * subgroup-size-control properties, asked only of a 1.3 device for the same reason; a
    * 1.1 device's one subgroupSize stands in below it. */
   VkPhysicalDeviceSubgroupSizeControlProperties ssc = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,
   };
   VkPhysicalDeviceSubgroupProperties sgp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
      .pNext = plain.apiVersion >= VK_API_VERSION_1_3 ? &ssc : NULL,
   };
   VkPhysicalDeviceDepthStencilResolveProperties dsr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
      .pNext = &sgp,
   };
   VkPhysicalDevicePushDescriptorPropertiesKHR pushp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
      .pNext = plain.apiVersion >= VK_API_VERSION_1_2 ? (void *)&dsr
             : plain.apiVersion >= VK_API_VERSION_1_1 ? (void *)&sgp : NULL,
   };
   VkPhysicalDeviceProperties2 p2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &pushp,
   };
   device->vk.GetPhysicalDeviceProperties2(device->physical, &p2);
   const VkPhysicalDeviceProperties *props = &p2.properties;
   snprintf(device->name, sizeof(device->name), "%s", props->deviceName);
   device->api_version = props->apiVersion;
   device->ubo_align = MAX2(props->limits.minUniformBufferOffsetAlignment, 1);
   device->ssbo_align = MAX2(props->limits.minStorageBufferOffsetAlignment, 1);
   device->max_ubo_range = props->limits.maxUniformBufferRange;
   device->max_push = pushp.maxPushDescriptors;
   device->max_anisotropy = props->limits.maxSamplerAnisotropy;
   device->max_3d = props->limits.maxImageDimension3D;
   device->max_vi_bindings = props->limits.maxVertexInputBindings;
   device->max_vi_offset = props->limits.maxVertexInputAttributeOffset;
   device->max_vi_stride = props->limits.maxVertexInputBindingStride;
   device->max_viewport[0] = (float)props->limits.maxViewportDimensions[0];
   device->max_viewport[1] = (float)props->limits.maxViewportDimensions[1];
   device->viewport_bounds[0] = props->limits.viewportBoundsRange[0];
   device->viewport_bounds[1] = props->limits.viewportBoundsRange[1];
   device->fb_color_samples = props->limits.framebufferColorSampleCounts;
   device->fb_depth_samples = props->limits.framebufferDepthSampleCounts;
   device->fb_stencil_samples = props->limits.framebufferStencilSampleCounts;
   device->depth_resolve_modes = plain.apiVersion >= VK_API_VERSION_1_2
                                    ? dsr.supportedDepthResolveModes : 0;
   device->stencil_resolve_modes = plain.apiVersion >= VK_API_VERSION_1_2
                                      ? dsr.supportedStencilResolveModes : 0;
   /* COMPUTE (M3d): the limits a dispatch is held to, and the two numbers a pipeline
    * reports (THE WAVE says why each is what it is) */
   for (unsigned i = 0; i < 3; i++) {
      device->max_group_size[i] = props->limits.maxComputeWorkGroupSize[i];
      device->max_group_count[i] = props->limits.maxComputeWorkGroupCount[i];
   }
   device->max_threads = MIN2(props->limits.maxComputeWorkGroupInvocations, 1024);
   device->max_shared = props->limits.maxComputeSharedMemorySize;
   device->simd_width = plain.apiVersion >= VK_API_VERSION_1_3 && ssc.minSubgroupSize
                           ? ssc.minSubgroupSize
                           : plain.apiVersion >= VK_API_VERSION_1_1 && sgp.subgroupSize
                                ? sgp.subgroupSize : 1;
   device->vk.GetPhysicalDeviceMemoryProperties(device->physical, &device->memory);

   /* Said once a process, because every decision below about an offset or a binding
    * count turns on these four numbers and a log should carry them beside the result.
    */
   static bool limits_said;
   if (!limits_said) {
      limits_said = true;
      /* Which source this library was built from, first, because a stale library -- one
       * built before the last edit of this file -- was once taken for the new one (M3d,
       * 2026-11-05). __TIMESTAMP__ is the compiler's reading of THIS file's last
       * modification time, __DATE__ and __TIME__ the moment it was compiled, so a log line
       * whose first is older than the edit it is meant to test says so by itself. */
      printf("air_vk: built from air_vk.c of %s, compiled %s %s\n", __TIMESTAMP__, __DATE__,
             __TIME__);
      printf("air_vk: %s, Vulkan %u.%u: minUniformBufferOffsetAlignment %llu, "
             "minStorageBufferOffsetAlignment %llu, maxUniformBufferRange %llu, "
             "maxPushDescriptors %u\n", device->name,
             VK_API_VERSION_MAJOR(device->api_version),
             VK_API_VERSION_MINOR(device->api_version),
             (unsigned long long)device->ubo_align, (unsigned long long)device->ssbo_align,
             (unsigned long long)device->max_ubo_range, device->max_push);
      /* and the sample counts (MULTISAMPLE, M3c-3), by format, for the same reason: every
       * multisample refusal below turns on them */
      say_sample_counts(device);
      fflush(stdout);
   }

   VkQueueFamilyProperties families[8];
   uint32_t nfam = ARRAY_SIZE(families);
   device->vk.GetPhysicalDeviceQueueFamilyProperties(device->physical, &nfam, families);
   device->family = UINT32_MAX;
   for (uint32_t i = 0; i < nfam; i++) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         device->family = i;
         break;
      }
   }
   if (device->family == UINT32_MAX) {
      say(why, why_size, "%s has no graphics queue", device->name);
      goto fail;
   }
   if (memory_type(device, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0) == UINT32_MAX) {
      say(why, why_size, "%s has no memory both sides see coherently, and a Metal buffer "
                         "is exactly that", device->name);
      goto fail;
   }

   /*
    * The features, each one that a translated Metal shader or a Metal sampler can need
    * and the device has, and nothing else: the 16- and 8-bit buffer accesses and
    * arithmetic AIR's half/short/char types become, stores from the vertex and fragment
    * stages (a `device` buffer is writable there), anisotropy, and mirror-clamp-to-edge
    * (MTLSamplerAddressModeMirrorClampToEdge). NOT robustBufferAccess or
    * robustImageAccess, which change what an out-of-bounds read returns: Gallium's
    * freedreno does not bound reads, and the two back ends must read the same bytes.
    * Dynamic rendering is core in 1.3, and the KHR extension with the same functions
    * otherwise; push descriptors are an extension either way.
    */
   bool core13 = device->api_version >= VK_API_VERSION_1_3;
   bool core12 = device->api_version >= VK_API_VERSION_1_2;
   VkPhysicalDeviceVulkan13Features h13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceVulkan12Features h12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = core13 ? &h13 : NULL,
   };
   VkPhysicalDeviceVulkan11Features h11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &h12,
   };
   VkPhysicalDeviceFeatures2 have = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = core12 ? &h11 : NULL,
   };
   device->vk.GetPhysicalDeviceFeatures2(device->physical, &have);

   VkPhysicalDeviceDynamicRenderingFeatures edr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
      .dynamicRendering = VK_TRUE,
   };
   /* [M3d: and subgroupSizeControl, so a compute stage may be made with
    * ALLOW_VARYING_SUBGROUP_SIZE, the wave contract Gallium gives ir3 (COMPUTE, THE WAVE).] */
   VkPhysicalDeviceVulkan13Features e13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE,
      .subgroupSizeControl = core13 ? h13.subgroupSizeControl : VK_FALSE,
   };
   VkPhysicalDeviceVulkan12Features e12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = core13 ? (void *)&e13 : (void *)&edr,
      .shaderFloat16 = h12.shaderFloat16,
      .shaderInt8 = h12.shaderInt8,
      .storageBuffer8BitAccess = h12.storageBuffer8BitAccess,
      .uniformAndStorageBuffer8BitAccess = h12.uniformAndStorageBuffer8BitAccess,
      .samplerMirrorClampToEdge = h12.samplerMirrorClampToEdge,
   };
   VkPhysicalDeviceVulkan11Features e11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &e12,
      .storageBuffer16BitAccess = h11.storageBuffer16BitAccess,
      .uniformAndStorageBuffer16BitAccess = h11.uniformAndStorageBuffer16BitAccess,
   };
   VkPhysicalDeviceFeatures2 enable = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = core12 ? (void *)&e11 : (void *)&edr,
      .features = {
         .samplerAnisotropy = have.features.samplerAnisotropy,
         .fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics,
         .vertexPipelineStoresAndAtomics = have.features.vertexPipelineStoresAndAtomics,
         .shaderInt16 = have.features.shaderInt16,
         /* MULTISAMPLE (M3c-3): a Metal fragment shader that reads [[sample_id]] runs
          * once per sample, and in Vulkan a shader that reads SampleId needs this
          * feature to be made at all. No shader of the corpus reads one; the feature
          * costs nothing a shader does not ask for. */
         .sampleRateShading = have.features.sampleRateShading,
         /* COMPUTE (M3d): a kernel's storage image is loaded and stored with no format in
          * the shader (STORAGE IMAGES: the frontend leaves PIPE_FORMAT_NONE, the bound
          * image's format decides), which is exactly what these two features allow, and
          * the extended formats are the RGBA16 ones AV1SW's kernels write. A shader that
          * writes no image asks nothing of any of them. */
         .shaderStorageImageReadWithoutFormat =
            have.features.shaderStorageImageReadWithoutFormat,
         .shaderStorageImageWriteWithoutFormat =
            have.features.shaderStorageImageWriteWithoutFormat,
         /* M5c: an array of descriptors chosen at run time (ARRAYS OF DESCRIPTORS) */
         .shaderUniformBufferArrayDynamicIndexing =
            have.features.shaderUniformBufferArrayDynamicIndexing,
         .shaderSampledImageArrayDynamicIndexing =
            have.features.shaderSampledImageArrayDynamicIndexing,
         .shaderStorageBufferArrayDynamicIndexing =
            have.features.shaderStorageBufferArrayDynamicIndexing,
         .shaderStorageImageArrayDynamicIndexing =
            have.features.shaderStorageImageArrayDynamicIndexing,
         .shaderStorageImageExtendedFormats = have.features.shaderStorageImageExtendedFormats,
         /* THE DRAW PATH (M5a): two attachments that blend differently (CoreAnimation's own
          * multiple-attachment shape, air_metal.h), and a point of any size but one, which
          * is what a vertex stage's [[point_size]] asks for. Neither costs anything a
          * pipeline does not ask for. Dual-source blending is not asked for: nothing here
          * carries a second output (BLENDING, in that section). */
         .independentBlend = have.features.independentBlend,
         .largePoints = have.features.largePoints,
         /* TEXTURES, LEVELS AND SLICES (M5b): a MTLTextureTypeCubeArray's view, and the two
          * families of compressed format the table holds. A device without one refuses
          * that texture by name (what turnip answers for the format), and nothing else. */
         .imageCubeArray = have.features.imageCubeArray,
         .textureCompressionETC2 = have.features.textureCompressionETC2,
         .textureCompressionASTC_LDR = have.features.textureCompressionASTC_LDR,
      },
   };
   if (core13 && !h13.dynamicRendering) {
      say(why, why_size, "%s says Vulkan 1.3 and has no dynamic rendering", device->name);
      goto fail;
   }
   device->anisotropy = have.features.samplerAnisotropy;
   device->varying_subgroups = core13 && h13.subgroupSizeControl;
   device->independent_blend = have.features.independentBlend;
   device->cube_array = have.features.imageCubeArray;

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = device->family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *ext[16] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
   uint32_t ext_count = 1;
   if (!core13)
      ext[ext_count++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
   /* TEXTURES OVER AN IOSURFACE (M3e): the import is a dma-buf import whose "file
    * descriptor" is the surface's ID (hk_drm.c's drmPrimeFDToHandle, as on Gallium), into
    * an image of an explicit linear layout, so it needs these three -- and they are
    * enabled only when turnip lists them, and only on a 1.2 device, where the extensions
    * the modifier one depends on (image_format_list, bind_memory2, ycbcr...) are core.
    * Without them the device is made as before and rwa_texture_from_surface refuses by
    * name: no other path of this library needs them. */
   static const char *const import_ext[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
   };
   unsigned listed = 0;
   /* THE DRAW PATH (M5a): the two extensions a pipeline's vertex input and lines may ask for,
    * by whichever name turnip lists them (it lists both of each), and each only when its
    * feature is there too. A device without them is made as before: a stream stepping at a
    * rate above one is refused by name, and a line is whatever that device's default is. */
   const char *divisor_ext = NULL, *line_ext = NULL;
   /* M5b: and maintenance5, for the format it names (TEXTURES, LEVELS AND SLICES, FORMATS) */
   const char *maint5_ext = NULL;
   if (core12) {
      VkExtensionProperties have_ext[512];
      uint32_t have_count = ARRAY_SIZE(have_ext);
      r = device->vk.EnumerateDeviceExtensionProperties(device->physical, NULL, &have_count,
                                                         have_ext);
      if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
         for (unsigned i = 0; i < ARRAY_SIZE(import_ext); i++)
            for (uint32_t j = 0; j < have_count; j++)
               if (!strcmp(have_ext[j].extensionName, import_ext[i])) {
                  listed++;
                  break;
               }
         for (uint32_t j = 0; j < have_count; j++) {
            const char *n = have_ext[j].extensionName;
            if (!strcmp(n, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
               divisor_ext = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
            else if (!divisor_ext && !strcmp(n, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
               divisor_ext = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
            else if (!strcmp(n, VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME))
               line_ext = VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME;
            else if (!line_ext && !strcmp(n, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME))
               line_ext = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
            else if (!strcmp(n, VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
               maint5_ext = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
         }
      }
   }
   VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR hdiv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR,
   };
   VkPhysicalDeviceLineRasterizationFeaturesKHR hline = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR,
   };
   VkPhysicalDeviceMaintenance5FeaturesKHR hmaint5 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
   };
   {
      VkPhysicalDeviceFeatures2 more = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      };
      if (divisor_ext) {
         hdiv.pNext = more.pNext;
         more.pNext = &hdiv;
      }
      if (line_ext) {
         hline.pNext = more.pNext;
         more.pNext = &hline;
      }
      if (maint5_ext) {
         hmaint5.pNext = more.pNext;
         more.pNext = &hmaint5;
      }
      if (more.pNext)
         device->vk.GetPhysicalDeviceFeatures2(device->physical, &more);
   }
   VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR ediv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR,
      .vertexAttributeInstanceRateDivisor = VK_TRUE,
   };
   VkPhysicalDeviceLineRasterizationFeaturesKHR eline = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR,
      .bresenhamLines = VK_TRUE,
      .rectangularLines = hline.rectangularLines,
   };
   VkPhysicalDeviceMaintenance5FeaturesKHR emaint5 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
      .maintenance5 = VK_TRUE,
   };
   /* M5c: an element chosen per pixel (ARRAYS OF DESCRIPTORS): core since 1.2, and turnip says yes
    * to each of these, natively. Nothing refuses a shader for it, so a device without them would
    * only be out of the specification: this asks for what it has. */
   VkPhysicalDeviceDescriptorIndexingFeatures eindexing = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
   };
   if (device->api_version >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceDescriptorIndexingFeatures hindexing = eindexing;
      VkPhysicalDeviceFeatures2 more = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
         .pNext = &hindexing,
      };
      device->vk.GetPhysicalDeviceFeatures2(device->physical, &more);
      eindexing.shaderSampledImageArrayNonUniformIndexing =
         hindexing.shaderSampledImageArrayNonUniformIndexing;
      eindexing.shaderStorageBufferArrayNonUniformIndexing =
         hindexing.shaderStorageBufferArrayNonUniformIndexing;
      eindexing.shaderStorageImageArrayNonUniformIndexing =
         hindexing.shaderStorageImageArrayNonUniformIndexing;
      eindexing.pNext = enable.pNext;
      enable.pNext = &eindexing;
   }
   device->maintenance5 = maint5_ext && hmaint5.maintenance5;
   if (device->maintenance5) {
      emaint5.pNext = enable.pNext;
      enable.pNext = &emaint5;
      ext[ext_count++] = maint5_ext;
   }
   device->divisors = divisor_ext && hdiv.vertexAttributeInstanceRateDivisor;
   device->bresenham = line_ext && hline.bresenhamLines;
   if (device->divisors) {
      ediv.pNext = enable.pNext;
      enable.pNext = &ediv;
      ext[ext_count++] = divisor_ext;
   }
   if (device->bresenham) {
      eline.pNext = enable.pNext;
      enable.pNext = &eline;
      ext[ext_count++] = line_ext;
   }
   device->can_import = listed == ARRAY_SIZE(import_ext);
   if (device->can_import)
      for (unsigned i = 0; i < ARRAY_SIZE(import_ext); i++)
         ext[ext_count++] = import_ext[i];
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &enable,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = ext_count,
      .ppEnabledExtensionNames = ext,
   };
   if ((r = device->vk.CreateDevice(device->physical, &dci, NULL, &device->dev)) != VK_SUCCESS) {
      say(why, why_size, "%s gave no Vulkan device with dynamic rendering and push "
                         "descriptors (%s)", device->name, vk_Result_to_str(r));
      device->dev = VK_NULL_HANDLE;
      goto fail;
   }
   RWA_VK_DEVICE_FNS(RWA_VK_LOAD_DEVICE)
   device->CmdBeginRendering = (PFN_vkCmdBeginRendering)device->vk.GetDeviceProcAddr(
      device->dev, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
   device->CmdEndRendering = (PFN_vkCmdEndRendering)device->vk.GetDeviceProcAddr(
      device->dev, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
   device->CmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)
      device->vk.GetDeviceProcAddr(device->dev, "vkCmdPushDescriptorSetKHR");
   if (!device->CmdBeginRendering || !device->CmdEndRendering ||
       !device->CmdPushDescriptorSet) {
      say(why, why_size, "%s has no vkCmdBeginRendering or no vkCmdPushDescriptorSetKHR",
          device->name);
      goto fail;
   }
   /* Vulkan 1.3's dynamic depth/stencil state (DEPTH AND STENCIL, THE STATE), core there
    * and so always present on a 1.3 device; left NULL below it, where rwa_render_create
    * refuses a depth attachment by name. Not a reason to fail the device: everything
    * without a depth attachment draws as it did. */
   if (core13) {
      device->CmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetDepthTestEnable");
      device->CmdSetDepthWriteEnable = (PFN_vkCmdSetDepthWriteEnable)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetDepthWriteEnable");
      device->CmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetDepthCompareOp");
      device->CmdSetStencilTestEnable = (PFN_vkCmdSetStencilTestEnable)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetStencilTestEnable");
      device->CmdSetStencilOp = (PFN_vkCmdSetStencilOp)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetStencilOp");
      /* and the two of a triangle's face (THE DRAW PATH, M5a), core there as those are */
      device->CmdSetCullMode = (PFN_vkCmdSetCullMode)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetCullMode");
      device->CmdSetFrontFace = (PFN_vkCmdSetFrontFace)
         device->vk.GetDeviceProcAddr(device->dev, "vkCmdSetFrontFace");
   }
   device->vk.GetDeviceQueue(device->dev, device->family, 0, &device->queue);

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = device->family,
   };
   if ((r = device->vk.CreateCommandPool(device->dev, &pci, NULL, &device->pool)) != VK_SUCCESS) {
      say(why, why_size, "%s gave no command pool (%s)", device->name, vk_Result_to_str(r));
      goto fail;
   }
   /* [M4-1, the cost part: and one for the upload command buffer, which any thread may record
    * into while the recording thread uses the other one: a pool is externally synchronised.] */
   if ((r = device->vk.CreateCommandPool(device->dev, &pci, NULL, &device->upload_pool)) !=
       VK_SUCCESS) {
      say(why, why_size, "%s gave no second command pool (%s)", device->name, vk_Result_to_str(r));
      goto fail;
   }
   const VkPipelineLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
   };
   if ((r = device->vk.CreatePipelineLayout(device->dev, &lci, NULL, &device->empty_layout)) !=
       VK_SUCCESS) {
      say(why, why_size, "%s gave no pipeline layout (%s)", device->name, vk_Result_to_str(r));
      goto fail;
   }

   device->nir_options = tu_hackios_nir_options(device->dev);
   if (!device->nir_options) {
      say(why, why_size, "turnip's device has no shader compiler, so no Metal shader can be "
                         "given to it");
      goto fail;
   }
   /* The compute limits (COMPUTE, M3d), once a process and after the features, because
    * whether a compute stage gets varying waves is a feature: every dispatch refusal of
    * Vulkan's own turns on one of these numbers, and the log should carry them. */
   static bool compute_said;
   if (!compute_said) {
      compute_said = true;
      printf("air_vk: compute: maxComputeWorkGroupInvocations %u (reported as %u), "
             "maxComputeWorkGroupSize %u %u %u, maxComputeWorkGroupCount %u %u %u, "
             "maxComputeSharedMemorySize %u, subgroups %u..%u, varying subgroup size %s\n",
             props->limits.maxComputeWorkGroupInvocations, device->max_threads,
             device->max_group_size[0], device->max_group_size[1], device->max_group_size[2],
             device->max_group_count[0], device->max_group_count[1],
             device->max_group_count[2], device->max_shared, device->simd_width,
             plain.apiVersion >= VK_API_VERSION_1_3 && ssc.maxSubgroupSize
                ? ssc.maxSubgroupSize : device->simd_width,
             device->varying_subgroups ? "yes" : "no");
      /* and what a draw is judged by (THE DRAW PATH, M5a), one line for the same reason.
       * The last word is whether this GPU says it blends RGBA32Float at all, which decides
       * what --draw-blend's exact arithmetic can be measured in. */
      VkFormatProperties rgba32f;
      device->vk.GetPhysicalDeviceFormatProperties(device->physical,
                                                   VK_FORMAT_R32G32B32A32_SFLOAT, &rgba32f);
      printf("air_vk: draw: maxVertexInputBindings %u, maxVertexInputAttributeOffset %u, "
             "maxVertexInputBindingStride %u, independentBlend %s, instance divisor %s, "
             "Bresenham lines %s, dynamic cull and winding %s, RGBA32Float blends %s\n",
             device->max_vi_bindings, device->max_vi_offset, device->max_vi_stride,
             device->independent_blend ? "yes" : "no", device->divisors ? "yes" : "no",
             device->bresenham ? "yes" : "no",
             device->CmdSetCullMode && device->CmdSetFrontFace ? "yes" : "no",
             rgba32f.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                ? "yes" : "no");
      fflush(stdout);
   }
   /* AIR_VK_NOSEEN=1: imported surfaces are not mapped at import (M3e, run 061800), which is
    * the old behaviour, for a test to show; off by default. */
   const char *noseen = getenv("AIR_VK_NOSEEN");
   device->no_seen = noseen && !strcmp(noseen, "1");
   static bool noseen_said;
   if (!noseen_said && device->no_seen) {
      noseen_said = true;
      printf("air_vk: AIR_VK_NOSEEN=1: a texture over an IOSurface is NOT mapped once at "
             "import, so the kernel cleans the surface out of the processor's cache at a "
             "submission only when its seed moved or 4 ms have passed\n");
      fflush(stdout);
   }
   /* And whether an IOSurface can be a texture here (M3e), once a process. */
   static bool import_said;
   if (!import_said) {
      import_said = true;
      printf("air_vk: textures over an IOSurface: %s\n",
             device->can_import
                ? "yes (a dma-buf import by surface ID into a linear image of the surface's "
                  "own row pitch)"
                : "no: the device lacks VK_KHR_external_memory_fd, "
                  "VK_EXT_external_memory_dma_buf or VK_EXT_image_drm_format_modifier, or is "
                  "below Vulkan 1.2");
      fflush(stdout);
   }
   pthread_mutex_lock(&open_lock);
   device->open_next = open_devices;
   open_devices = device;
   if (!exit_line_registered) {
      exit_line_registered = true;
      atexit(exit_line);
   }
   pthread_mutex_unlock(&open_lock);
   return device;

fail:
   if (device->dev) {
      if (device->empty_layout)
         device->vk.DestroyPipelineLayout(device->dev, device->empty_layout, NULL);
      if (device->pool)
         device->vk.DestroyCommandPool(device->dev, device->pool, NULL);
      if (device->upload_pool)
         device->vk.DestroyCommandPool(device->dev, device->upload_pool, NULL);
      if (device->vk.DestroyDevice)
         device->vk.DestroyDevice(device->dev, NULL);
   }
   if (device->instance && device->vk.DestroyInstance)
      device->vk.DestroyInstance(device->instance, NULL);
   pthread_mutex_destroy(&device->lock);
   pthread_mutex_destroy(&device->argbuf_lock);
   pthread_mutex_destroy(&device->upload_lock);
   free(device);
   glsl_type_singleton_decref();
   return NULL;
}

void
rwa_device_close(struct rwa_device *device)
{
   if (!device)
      return;
   /* everything recorded runs, and everything submitted is waited for */
   idle(device);
   /* [M4-1: and the whole device idle before anything of it goes, whatever idle's wait
    * answered and whatever thread recorded last. Then the newest submission's pair is the
    * library's own to let go of.]
    * [M4-1, the cost part: upload_lock, because this reaches the VkQueue like a submission.] */
   pthread_mutex_lock(&device->upload_lock);
   device->vk.DeviceWaitIdle(device->dev);
   pthread_mutex_unlock(&device->upload_lock);
   pthread_mutex_lock(&device->lock);
   if (device->last) {
      pair_unref_locked(device, device->last);
      device->last = NULL;
   }
   /* [M4-1, the cost part: and everything a destroy left behind goes now, whatever its fence
    * says: the device is idle (a failed submission's fence never will say).] */
   struct rwa_after *after = device->after;
   device->after = NULL;
   pthread_mutex_unlock(&device->lock);
   while (after) {
      struct rwa_after *a = after;
      after = a->next;
      a->free(device, a->object);
      free(a);
   }

   unsigned n[4];
   rwa_vk_counts(n);
   printf("air_vk: still in use: %u command buffers, %u fences, %u buffers, %u images\n",
          n[RWA_LIVE_CMD], n[RWA_LIVE_FENCE], n[RWA_LIVE_BUFFER], n[RWA_LIVE_IMAGE]);
   int32_t out = p_atomic_read(&device->pairs_out);
   int32_t children = p_atomic_read(&device->children);
   if (children || out > 0) {
      /* A buffer, texture, sampler or pipeline of this device is still alive, or a fence
       * handed out by rwa_flush was never released, and its destroy or release will come
       * later and reach for this device: Vulkan requires every child gone before
       * vkDestroyDevice, and a caller's later call would read freed memory. So the
       * device is KEPT -- a leak, said out loud, rather than a fault.
       */
      printf("air_vk: %d objects and %d fences of this device are still held, so the "
             "Vulkan device is kept rather than destroyed under them\n", (int)children,
             (int)out);
      fflush(stdout);
      return;
   }
   fflush(stdout);

   /* out of the exit line's list, before its pairs and its device go */
   pthread_mutex_lock(&open_lock);
   for (struct rwa_device **d = &open_devices; *d; d = &(*d)->open_next) {
      if (*d == device) {
         *d = device->open_next;
         break;
      }
   }
   pthread_mutex_unlock(&open_lock);

   for (struct rwa_fence *s = device->all, *next; s; s = next) {
      next = s->all;
      /* a pair given back unsignalled (a failed submission) is not waited for: it was
       * never submitted, so there is nothing to wait for */
      for (struct rwa_upload *u = s->uploads, *un; u; u = un) {
         un = u->next;
         raw_destroy(device, &u->raw);
         free(u);
      }
      device->vk.DestroyFence(device->dev, s->fence, NULL);
      if (s->cmd)
         device->vk.FreeCommandBuffers(device->dev, device->pool, 1, &s->cmd);
      free(s->ucmd);              /* (its command buffer goes with the upload pool, below) */
      sets_reset(device, s);
      free(s->full_sets);
      if (s->sets)
         device->vk.DestroyDescriptorPool(device->dev, s->sets, NULL);
      free(s);
   }
   for (struct rwa_ucmd *u = device->ucmd_free, *un; u; u = un) {
      un = u->next;
      free(u);
   }
   free(device->uploading);       /* (never sent: a recording of a thread that went away) */
   raw_destroy(device, &device->ring);
   device->vk.DestroyPipelineLayout(device->dev, device->empty_layout, NULL);
   device->vk.DestroyCommandPool(device->dev, device->pool, NULL);
   device->vk.DestroyCommandPool(device->dev, device->upload_pool, NULL);
   device->vk.DestroyDevice(device->dev, NULL);
   device->vk.DestroyInstance(device->instance, NULL);
   pthread_mutex_destroy(&device->lock);
   pthread_mutex_destroy(&device->argbuf_lock);
   pthread_mutex_destroy(&device->upload_lock);
   free(device);
   glsl_type_singleton_decref();
}

const char *
rwa_device_name(struct rwa_device *device)
{
   /* turnip's own name for the chip ("Turnip Adreno (TM) 650") */
   return device && device->name[0] ? device->name : "unknown GPU";
}

/* ---------------------------------------------------------------------------------- */
/* Buffers                                                                            */
/* ---------------------------------------------------------------------------------- */

struct rwa_buffer *
rwa_buffer_create(struct rwa_device *device, size_t size, char *why, size_t why_size)
{
   if (size == 0 || size > UINT32_MAX) {
      say(why, why_size, "a buffer of %zu bytes: this library sizes a buffer in 32 bits, as "
                         "the Gallium one does, and zero is not a buffer", size);
      return NULL;
   }
   struct rwa_buffer *buffer = calloc(1, sizeof(*buffer));
   if (!buffer) {
      say(why, why_size, "out of memory making a buffer");
      return NULL;
   }
   buffer->device = device;
   buffer->size = size;

   /* Every use at once, for air_metal.c's reason: Metal has one buffer object, and what
    * the bytes are -- vertices, indices, a `constant` block, a `device` array, the source
    * of a blit -- is decided at the draw, not here.
    */
   if (!raw_create(device, size,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer->raw, why, why_size)) {
      free(buffer);
      return NULL;
   }
   p_atomic_inc(&device->children);
   p_atomic_inc(&rwa_live[RWA_LIVE_BUFFER]);
   return buffer;
}

void *
rwa_buffer_map(struct rwa_buffer *buffer)
{
   /* mapped once, for the buffer's whole life: the contract has no unmap */
   return buffer->raw.map;
}

size_t
rwa_buffer_size(struct rwa_buffer *buffer)
{
   return buffer->size;
}

/* [M4-1, the cost part: what a destroy frees, now or once its work is done (when_done). The
 * test's count goes down at the destroy, which is when the application let go of it; the device's
 * `children` when it is gone, which is what rwa_device_close needs to know.] */
static void
buffer_free(struct rwa_device *device, void *object)
{
   struct rwa_buffer *buffer = object;
   raw_destroy(device, &buffer->raw);
   p_atomic_dec(&device->children);
   free(buffer);
}

void
rwa_buffer_destroy(struct rwa_buffer *buffer)
{
   if (!buffer)
      return;
   struct rwa_device *device = buffer->device;
   p_atomic_dec(&rwa_live[RWA_LIVE_BUFFER]);
   argbuf_scrub(device, buffer, NULL, NULL);
   argbuf_forget_all(buffer);
   when_done(device, buffer->use, buffer_free, buffer);
}

/* Where [offset, offset + size) of a buffer lies inside it, or a sentence. */
static bool
buffer_range(const struct rwa_buffer *b, size_t offset, size_t size, const char *what,
             char *why, size_t why_size)
{
   if (offset > b->size || size > b->size - offset) {
      say(why, why_size, "%s: %zu bytes at offset %zu of a buffer that holds %zu", what, size,
          offset, b->size);
      return false;
   }
   return true;
}

/* ---------------------------------------------------------------------------------- */
/* Textures                                                                           */
/* ---------------------------------------------------------------------------------- */

enum {   /* MTLTextureType, as air_metal.c spells it */
   RWA_TYPE_1D = 0, RWA_TYPE_1D_ARRAY = 1,
   RWA_TYPE_2D = 2, RWA_TYPE_2D_ARRAY = 3, RWA_TYPE_2D_MULTISAMPLE = 4,
   RWA_TYPE_CUBE = 5, RWA_TYPE_CUBE_ARRAY = 6,
   RWA_TYPE_3D = 7, RWA_TYPE_2D_MULTISAMPLE_ARRAY = 8, RWA_TYPE_TEXTURE_BUFFER = 9,
};

enum {   /* MTLTextureUsage, a mask */
   RWA_USAGE_SHADER_READ = 0x1, RWA_USAGE_SHADER_WRITE = 0x2,
   RWA_USAGE_RENDER_TARGET = 0x4, RWA_USAGE_PIXEL_FORMAT_VIEW = 0x10,
};

static const char *
texture_type_name(unsigned type)
{
   static const char *const names[] = {
      "MTLTextureType1D", "MTLTextureType1DArray", "MTLTextureType2D",
      "MTLTextureType2DArray", "MTLTextureType2DMultisample", "MTLTextureTypeCube",
      "MTLTextureTypeCubeArray", "MTLTextureType3D", "MTLTextureType2DMultisampleArray",
      "MTLTextureTypeTextureBuffer",
   };
   return type < ARRAY_SIZE(names) ? names[type] : "a texture type Metal does not have";
}

/*
 * air_metal.c's table of colour formats, in Vulkan's names. The two vocabularies agree on
 * every entry here because each is whole bytes or whole 16- or 32-bit words in the order
 * the name gives: Metal's RGBA8Unorm is R, G, B, A in four bytes, and so is
 * VK_FORMAT_R8G8B8A8_UNORM; BGRA8Unorm is B, G, R, A, and so is VK_FORMAT_B8G8R8A8_UNORM.
 * It covers every format metal_drv_test --draw and --draw-all bind (RGBA32Uint, Sint and
 * Float for the corpus's declared textures, R32Float for --rtfmt) and the common ones of
 * the window server.
 *
 * What is NOT here, each refused by name below rather than mapped:
 *   - the four packed 16-bit formats (40-43). air_metal.c serves them with a reading that
 *     a boot of the home screen settled for GALLIUM's names, which count channels from
 *     [M5b: "settled" says too much: see the M5b note at the end of this comment]
 *     the least significant bit; Vulkan's _PACK16 names count from the most significant
 *     one, so the same Metal format is a different-looking Vulkan name, and that
 *     translation has not been drawn on this back end once. A swapped red and blue is a
 *     picture, not a failure.
 *   - A8Unorm (1): Vulkan has VK_FORMAT_A8_UNORM_KHR only with VK_KHR_maintenance5,
 *     which this device is not asked for.
 *   - depth and stencil (250-262): there is no depth attachment yet. [M3c-2: no longer
 *     so. The four of them air_metal.c maps are at the end of the table, and the rest of
 *     Metal's 250-262 is refused by name in format_refusal, for air_metal.c's reasons.]
 *
 * [M5b: THE FOUR PACKED 16-BIT FORMATS AND A8UNORM ARE IN THE TABLE NOW, each with what says so.
 *
 * 40-43 ARE REAL METAL'S LAYOUT AS MEASURED ON AN M1 MAX, NOT A11: the public Metal API, a word with
 * one field set, read back with texture.read. The first-named channel is in the LOW bits, the same
 * rule as the packed 32-bit formats. Vulkan's _PACK16 names count from the high end, so each is the
 * mirror of Metal's name, and MoltenVK's MVKPixelFormats.mm has exactly these four:
 *
 *   Metal           one-field words                               Vulkan
 *   40 B5G6R5       b 0x001F  g 0x07E0  r 0xF800                  R5G6B5_UNORM_PACK16
 *   41 A1BGR5       a 0x0001  b 0x003E  g 0x07C0  r 0xF800        R5G5B5A1_UNORM_PACK16
 *   42 ABGR4        a 0x000F  b 0x00F0  g 0x0F00  r 0xF000        R4G4B4A4_UNORM_PACK16
 *   43 BGR5A1       b 0x001F  g 0x03E0  r 0x7C00  a 0x8000        A1R5G5B5_UNORM_PACK16
 *
 * All four are core Vulkan and none needs an extension. In Mesa they are PIPE_FORMAT_B5G6R5_UNORM,
 * A1B5G5R5_UNORM, A4B4G4R4_UNORM and B5G5R5A1_UNORM (vk_format_map, src/vulkan/util/vk_format.c),
 * each _TC in fd6_format_table.c, so turnip gives them sampling, colour attachment, blending, copies
 * and the blit, and no storage image (tu_formats.cc: "We don't support writing into
 * VK_FORMAT_*_PACK16 images"), which is texture_of_shape's refusal if asked. That is read from the
 * source: what the device answers for each is the phone's.
 *
 * THIS IS NOT WHAT air_metal.c HAS, and that is deliberate for now. Its four are the mirror of these,
 * red and blue exchanged, because a boot through Apple's own compositor drew the wallpaper layer as
 * (255, 12, 12) under this mapping and blue under that one. That picture is a real measurement. What
 * it does not show is WHO did the exchange: a B,G,R,A swizzle on the compositor's texture or view
 * would look exactly like it, and hkmetal.m ignored every swizzle until this build. That is a
 * hypothesis and unverified; the source of the exchange is open. hkmetal.m logs what each 16-bit
 * texture asks for, for the boot that can tell, and says there what this table may do to the picture
 * meanwhile. An earlier version of this comment, of this same round, called the compositor's
 * reading settled; it is not.
 *
 * A8Unorm is VK_FORMAT_A8_UNORM_KHR with VK_KHR_maintenance5 and not R8 with a swizzle: R8 with
 * (0, 0, 0, R) is the usual way where the format is missing, and it would have to be undone for
 * every other thing a format is -- a pass renders through a view with no swizzle, so its alpha
 * output would land in a channel of nothing -- and turnip has the real one in one table with
 * freedreno's (fd6_format_table.c, _TC(A8_UNORM, A8_UNORM, WZYX)): the same texture and colour
 * format as on Gallium, with util_format's swizzle, (0, 0, 0, a), in the texture state.
 *
 * THE PACKED 32-BIT FORMATS, which air_metal.c declines for a reason it spells out (a name's order
 * and a bias it will not guess). Vulkan's names for them are exact and they cost nothing. Their
 * bit layout was measured in the same run on the M1 Max (A11 not measured): RGB10A2 R low and A on
 * top, BGR10A2 B low and A on top, RG11B10 R low with alpha reading 1, RGB9E5 R low, E on top and
 * alpha reading 1, and A8Unorm's 128 reads (0, 0, 0, 0.502). That is the layout only, not the
 * conversion of a value. So the first-named channel is in the low bits and Vulkan's name, counting
 * from the high end, is the mirror: RGB10A2Unorm A2B10G10R10, RGB10A2Uint its UINT, BGR10A2Unorm
 * A2R10G10B10, RG11B10Float B10G11R11, RGB9E5Float E5B9G9R9. The last is sampled and not rendered
 * on turnip (tu_formats.cc: no colour format), and it is the texture_of_shape refusal if asked.
 *
 * THE COMPRESSED FORMATS: air_metal.c refuses them all ("blocks this library cannot make"), Vulkan
 * has ETC2, EAC and ASTC's LDR in its core features, and rwa_texture_shape's format is any format,
 * so the contract allows them with nothing added but the arithmetic of a block (texture_region).
 * Not here, each refused by name: ASTC's HDR (turnip: textureCompressionASTC_HDR false), PVRTC
 * (no extension), BC (turnip has them and this milestone was not asked for them: the iOS SDK has
 * them from 16.4 and no shader or application here has been seen to want one), the two 4:2:2 ones
 * and the XR ones.]
 */
static const struct rwa_vk_format rwa_vk_formats[] = {
   {   1, "MTLPixelFormatA8Unorm",         VK_FORMAT_A8_UNORM_KHR,         1, RWA_KIND_FLOAT },
   {  10, "MTLPixelFormatR8Unorm",         VK_FORMAT_R8_UNORM,             1, RWA_KIND_FLOAT },
   {  11, "MTLPixelFormatR8Unorm_sRGB",    VK_FORMAT_R8_SRGB,              1, RWA_KIND_FLOAT },
   {  12, "MTLPixelFormatR8Snorm",         VK_FORMAT_R8_SNORM,             1, RWA_KIND_FLOAT },
   {  13, "MTLPixelFormatR8Uint",          VK_FORMAT_R8_UINT,              1, RWA_KIND_UINT },
   {  14, "MTLPixelFormatR8Sint",          VK_FORMAT_R8_SINT,              1, RWA_KIND_SINT },
   {  20, "MTLPixelFormatR16Unorm",        VK_FORMAT_R16_UNORM,            2, RWA_KIND_FLOAT },
   {  22, "MTLPixelFormatR16Snorm",        VK_FORMAT_R16_SNORM,            2, RWA_KIND_FLOAT },
   {  23, "MTLPixelFormatR16Uint",         VK_FORMAT_R16_UINT,             2, RWA_KIND_UINT },
   {  24, "MTLPixelFormatR16Sint",         VK_FORMAT_R16_SINT,             2, RWA_KIND_SINT },
   {  25, "MTLPixelFormatR16Float",        VK_FORMAT_R16_SFLOAT,           2, RWA_KIND_FLOAT },
   {  30, "MTLPixelFormatRG8Unorm",        VK_FORMAT_R8G8_UNORM,           2, RWA_KIND_FLOAT },
   {  31, "MTLPixelFormatRG8Unorm_sRGB",   VK_FORMAT_R8G8_SRGB,            2, RWA_KIND_FLOAT },
   {  32, "MTLPixelFormatRG8Snorm",        VK_FORMAT_R8G8_SNORM,           2, RWA_KIND_FLOAT },
   {  33, "MTLPixelFormatRG8Uint",         VK_FORMAT_R8G8_UINT,            2, RWA_KIND_UINT },
   {  34, "MTLPixelFormatRG8Sint",         VK_FORMAT_R8G8_SINT,            2, RWA_KIND_SINT },
   {  40, "MTLPixelFormatB5G6R5Unorm",     VK_FORMAT_R5G6B5_UNORM_PACK16,  2, RWA_KIND_FLOAT },
   {  41, "MTLPixelFormatA1BGR5Unorm",     VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2, RWA_KIND_FLOAT },
   {  42, "MTLPixelFormatABGR4Unorm",      VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, RWA_KIND_FLOAT },
   {  43, "MTLPixelFormatBGR5A1Unorm",     VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, RWA_KIND_FLOAT },
   {  53, "MTLPixelFormatR32Uint",         VK_FORMAT_R32_UINT,             4, RWA_KIND_UINT },
   {  54, "MTLPixelFormatR32Sint",         VK_FORMAT_R32_SINT,             4, RWA_KIND_SINT },
   {  55, "MTLPixelFormatR32Float",        VK_FORMAT_R32_SFLOAT,           4, RWA_KIND_FLOAT },
   {  60, "MTLPixelFormatRG16Unorm",       VK_FORMAT_R16G16_UNORM,         4, RWA_KIND_FLOAT },
   {  62, "MTLPixelFormatRG16Snorm",       VK_FORMAT_R16G16_SNORM,         4, RWA_KIND_FLOAT },
   {  63, "MTLPixelFormatRG16Uint",        VK_FORMAT_R16G16_UINT,          4, RWA_KIND_UINT },
   {  64, "MTLPixelFormatRG16Sint",        VK_FORMAT_R16G16_SINT,          4, RWA_KIND_SINT },
   {  65, "MTLPixelFormatRG16Float",       VK_FORMAT_R16G16_SFLOAT,        4, RWA_KIND_FLOAT },
   {  70, "MTLPixelFormatRGBA8Unorm",      VK_FORMAT_R8G8B8A8_UNORM,       4, RWA_KIND_FLOAT },
   {  71, "MTLPixelFormatRGBA8Unorm_sRGB", VK_FORMAT_R8G8B8A8_SRGB,        4, RWA_KIND_FLOAT },
   {  72, "MTLPixelFormatRGBA8Snorm",      VK_FORMAT_R8G8B8A8_SNORM,       4, RWA_KIND_FLOAT },
   {  73, "MTLPixelFormatRGBA8Uint",       VK_FORMAT_R8G8B8A8_UINT,        4, RWA_KIND_UINT },
   {  74, "MTLPixelFormatRGBA8Sint",       VK_FORMAT_R8G8B8A8_SINT,        4, RWA_KIND_SINT },
   {  80, "MTLPixelFormatBGRA8Unorm",      VK_FORMAT_B8G8R8A8_UNORM,       4, RWA_KIND_FLOAT },
   {  81, "MTLPixelFormatBGRA8Unorm_sRGB", VK_FORMAT_B8G8R8A8_SRGB,        4, RWA_KIND_FLOAT },
   {  90, "MTLPixelFormatRGB10A2Unorm",    VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, RWA_KIND_FLOAT },
   {  91, "MTLPixelFormatRGB10A2Uint",     VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, RWA_KIND_UINT },
   {  92, "MTLPixelFormatRG11B10Float",    VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, RWA_KIND_FLOAT },
   {  93, "MTLPixelFormatRGB9E5Float",     VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, RWA_KIND_FLOAT },
   {  94, "MTLPixelFormatBGR10A2Unorm",    VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, RWA_KIND_FLOAT },
   { 103, "MTLPixelFormatRG32Uint",        VK_FORMAT_R32G32_UINT,          8, RWA_KIND_UINT },
   { 104, "MTLPixelFormatRG32Sint",        VK_FORMAT_R32G32_SINT,          8, RWA_KIND_SINT },
   { 105, "MTLPixelFormatRG32Float",       VK_FORMAT_R32G32_SFLOAT,        8, RWA_KIND_FLOAT },
   { 110, "MTLPixelFormatRGBA16Unorm",     VK_FORMAT_R16G16B16A16_UNORM,   8, RWA_KIND_FLOAT },
   { 112, "MTLPixelFormatRGBA16Snorm",     VK_FORMAT_R16G16B16A16_SNORM,   8, RWA_KIND_FLOAT },
   { 113, "MTLPixelFormatRGBA16Uint",      VK_FORMAT_R16G16B16A16_UINT,    8, RWA_KIND_UINT },
   { 114, "MTLPixelFormatRGBA16Sint",      VK_FORMAT_R16G16B16A16_SINT,    8, RWA_KIND_SINT },
   { 115, "MTLPixelFormatRGBA16Float",     VK_FORMAT_R16G16B16A16_SFLOAT,  8, RWA_KIND_FLOAT },
   { 123, "MTLPixelFormatRGBA32Uint",      VK_FORMAT_R32G32B32A32_UINT,   16, RWA_KIND_UINT },
   { 124, "MTLPixelFormatRGBA32Sint",      VK_FORMAT_R32G32B32A32_SINT,   16, RWA_KIND_SINT },
   { 125, "MTLPixelFormatRGBA32Float",     VK_FORMAT_R32G32B32A32_SFLOAT, 16, RWA_KIND_FLOAT },

   /* M5b, THE COMPRESSED FORMATS, at the comment above: bytes per block, and the block. */
   { 170, "MTLPixelFormatEAC_R11Unorm",    VK_FORMAT_EAC_R11_UNORM_BLOCK,   8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 172, "MTLPixelFormatEAC_R11Snorm",    VK_FORMAT_EAC_R11_SNORM_BLOCK,   8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 174, "MTLPixelFormatEAC_RG11Unorm",   VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 176, "MTLPixelFormatEAC_RG11Snorm",   VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 178, "MTLPixelFormatEAC_RGBA8",       VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 179, "MTLPixelFormatEAC_RGBA8_sRGB",  VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 180, "MTLPixelFormatETC2_RGB8",       VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 181, "MTLPixelFormatETC2_RGB8_sRGB",  VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, 8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 182, "MTLPixelFormatETC2_RGB8A1",     VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, 8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 183, "MTLPixelFormatETC2_RGB8A1_sRGB", VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, 8, RWA_KIND_FLOAT, 0, 4, 4 },
   { 186, "MTLPixelFormatASTC_4x4_sRGB", VK_FORMAT_ASTC_4x4_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 204, "MTLPixelFormatASTC_4x4_LDR", VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 4, 4 },
   { 187, "MTLPixelFormatASTC_5x4_sRGB", VK_FORMAT_ASTC_5x4_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 5, 4 },
   { 205, "MTLPixelFormatASTC_5x4_LDR", VK_FORMAT_ASTC_5x4_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 5, 4 },
   { 188, "MTLPixelFormatASTC_5x5_sRGB", VK_FORMAT_ASTC_5x5_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 5, 5 },
   { 206, "MTLPixelFormatASTC_5x5_LDR", VK_FORMAT_ASTC_5x5_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 5, 5 },
   { 189, "MTLPixelFormatASTC_6x5_sRGB", VK_FORMAT_ASTC_6x5_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 6, 5 },
   { 207, "MTLPixelFormatASTC_6x5_LDR", VK_FORMAT_ASTC_6x5_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 6, 5 },
   { 190, "MTLPixelFormatASTC_6x6_sRGB", VK_FORMAT_ASTC_6x6_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 6, 6 },
   { 208, "MTLPixelFormatASTC_6x6_LDR", VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 6, 6 },
   { 192, "MTLPixelFormatASTC_8x5_sRGB", VK_FORMAT_ASTC_8x5_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 5 },
   { 210, "MTLPixelFormatASTC_8x5_LDR", VK_FORMAT_ASTC_8x5_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 5 },
   { 193, "MTLPixelFormatASTC_8x6_sRGB", VK_FORMAT_ASTC_8x6_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 6 },
   { 211, "MTLPixelFormatASTC_8x6_LDR", VK_FORMAT_ASTC_8x6_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 6 },
   { 194, "MTLPixelFormatASTC_8x8_sRGB", VK_FORMAT_ASTC_8x8_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 8 },
   { 212, "MTLPixelFormatASTC_8x8_LDR", VK_FORMAT_ASTC_8x8_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 8, 8 },
   { 195, "MTLPixelFormatASTC_10x5_sRGB", VK_FORMAT_ASTC_10x5_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 5 },
   { 213, "MTLPixelFormatASTC_10x5_LDR", VK_FORMAT_ASTC_10x5_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 5 },
   { 196, "MTLPixelFormatASTC_10x6_sRGB", VK_FORMAT_ASTC_10x6_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 6 },
   { 214, "MTLPixelFormatASTC_10x6_LDR", VK_FORMAT_ASTC_10x6_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 6 },
   { 197, "MTLPixelFormatASTC_10x8_sRGB", VK_FORMAT_ASTC_10x8_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 8 },
   { 215, "MTLPixelFormatASTC_10x8_LDR", VK_FORMAT_ASTC_10x8_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 8 },
   { 198, "MTLPixelFormatASTC_10x10_sRGB", VK_FORMAT_ASTC_10x10_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 10 },
   { 216, "MTLPixelFormatASTC_10x10_LDR", VK_FORMAT_ASTC_10x10_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 10, 10 },
   { 199, "MTLPixelFormatASTC_12x10_sRGB", VK_FORMAT_ASTC_12x10_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 12, 10 },
   { 217, "MTLPixelFormatASTC_12x10_LDR", VK_FORMAT_ASTC_12x10_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 12, 10 },
   { 200, "MTLPixelFormatASTC_12x12_sRGB", VK_FORMAT_ASTC_12x12_SRGB_BLOCK, 16, RWA_KIND_FLOAT, 0, 12, 12 },
   { 218, "MTLPixelFormatASTC_12x12_LDR", VK_FORMAT_ASTC_12x12_UNORM_BLOCK, 16, RWA_KIND_FLOAT, 0, 12, 12 },

   /* Depth and stencil (M3c-2): air_metal.c's four, each the same bytes per aspect under
    * both names -- 16-bit normalised depth, 32-bit float depth, an 8-bit stencil, and the
    * two together, which Gallium spells Z32_FLOAT_S8X24_UINT and Metal and Vulkan both
    * count as one texel of eight bytes (that is the size Metal's copy rule compares). The
    * `kind` is what a shader reads out of the sampled aspect: a float depth for the three
    * that have one, and an unsigned integer out of Stencil8 -- util_format_is_pure_uint's
    * answer for each on the Gallium side.
    */
   { 250, "MTLPixelFormatDepth16Unorm",    VK_FORMAT_D16_UNORM,            2, RWA_KIND_FLOAT,
     VK_IMAGE_ASPECT_DEPTH_BIT },
   { 252, "MTLPixelFormatDepth32Float",    VK_FORMAT_D32_SFLOAT,           4, RWA_KIND_FLOAT,
     VK_IMAGE_ASPECT_DEPTH_BIT },
   { 253, "MTLPixelFormatStencil8",        VK_FORMAT_S8_UINT,              1, RWA_KIND_UINT,
     VK_IMAGE_ASPECT_STENCIL_BIT },
   { 260, "MTLPixelFormatDepth32Float_Stencil8", VK_FORMAT_D32_SFLOAT_S8_UINT, 8, RWA_KIND_FLOAT,
     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT },
   /* M5b: and what a view of one's stencil is: the image's format, its stencil aspect, an
    * unsigned integer in a shader. No texture is made with it (Metal's rule). */
   { 261, "MTLPixelFormatX32_Stencil8",    VK_FORMAT_D32_SFLOAT_S8_UINT,   8, RWA_KIND_UINT,
     VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, true },
};

static const struct rwa_vk_format *
vk_format_of(unsigned mtl)
{
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_vk_formats); i++)
      if (rwa_vk_formats[i].mtl == mtl)
         return &rwa_vk_formats[i];
   return NULL;
}

/* The aspects of a format, which is what every subresource range and every copy region of
 * a texture of it names: COLOR for a colour format (the table leaves zs_aspects out for
 * those), its depth and/or stencil aspects otherwise. The one reader of zs_aspects.
 */
static unsigned
fmt_block_w(const struct rwa_vk_format *f)
{
   return f->block_w ? f->block_w : 1;
}

static unsigned
fmt_block_h(const struct rwa_vk_format *f)
{
   return f->block_h ? f->block_h : 1;
}

static VkImageAspectFlags
format_aspects(const struct rwa_vk_format *f)
{
   return f->zs_aspects ? f->zs_aspects : VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool
format_is_depth_stencil(const struct rwa_vk_format *f)
{
   return f->zs_aspects != 0;
}

/* What one aspect of a depth/stencil format is in a buffer, which is what
 * vkCmdCopyImageToBuffer writes for that aspect alone: the depth of D16_UNORM is 16-bit
 * normalised, the depth of D32_SFLOAT and of D32_SFLOAT_S8_UINT a 32-bit float (Vulkan
 * copies the depth aspect of the combined format as four bytes, the stencil apart), and a
 * stencil aspect is one byte. The Gallium format named is the one whose util_format
 * unpacker reads those bytes, so rwa_depth_read decodes with Mesa's own code exactly as
 * air_metal.c does, and never with arithmetic written here.
 */
static void
aspect_layout(const struct rwa_vk_format *f, VkImageAspectFlags aspect, unsigned *bytes,
              enum pipe_format *pipe)
{
   if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
      *bytes = 1;
      *pipe = PIPE_FORMAT_S8_UINT;
   } else if (f->vk == VK_FORMAT_D16_UNORM) {
      *bytes = 2;
      *pipe = PIPE_FORMAT_Z16_UNORM;
   } else {
      *bytes = 4;
      *pipe = PIPE_FORMAT_Z32_FLOAT;
   }
}

/* Why a format is not in the table, naming it: the reason differs per family, and
 * "unsupported" would hide which one it is.
 */
static void
format_refusal(char *why, size_t why_size, const char *what, unsigned f)
{
   if (f == 0)
      say(why, why_size, "%s: MTLPixelFormatInvalid, which is what a descriptor nobody "
                         "filled in holds", what);
   else if (f == 1)
      say(why, why_size, "%s: MTLPixelFormatA8Unorm, which Vulkan has only with "
                         "VK_KHR_maintenance5; it is not in the Vulkan back end yet (M3b)",
          what);
   /* [M5b: 1 and 40-43 are in the table now, so nothing reaches these two. They are kept for the
    * reason each gave.] */
   else if (f >= 40 && f <= 43)
      say(why, why_size, "%s: one of Metal's 16-bit packed formats (pixel format %u: "
                         "B5G6R5, A1BGR5, ABGR4 or BGR5A1), whose channel order was measured "
                         "against Gallium's names and has not been drawn against Vulkan's "
                         "_PACK16 names, which count from the other end; it is not in the "
                         "Vulkan back end yet (M3b)", what, f);
   /* The four depth and stencil formats iOS has are in the table (M3c-2); these are the
    * rest of Metal's enumeration between them, each declined for air_metal.c's own reason
    * (pixel_format_refusal there) rather than for being a depth format. */
   else if (f == 255)
      say(why, why_size, "%s: MTLPixelFormatDepth24Unorm_Stencil8, which Metal's own header "
                         "marks API_UNAVAILABLE(ios) and this device answers NO for "
                         "(-isDepth24Stencil8PixelFormatSupported). turnip has "
                         "VK_FORMAT_D24_UNORM_S8_UINT, but a format the device says it does "
                         "not have must not be allocatable through it, and the Gallium back "
                         "end declines it too", what);
   else if (f == 261 || f == 262)
      say(why, why_size, "%s: pixel format %u is one of the X*_Stencil8 formats, which is not "
                         "a texture but a view of the stencil aspect of one made as "
                         "Depth32Float_Stencil8: it comes from -newTextureViewWithPixelFormat: "
                         "of such a texture (M5b), and rwa_depth_read asks for an "
                         "aspect of the texture itself instead", what, f);
   else if (f >= 250 && f <= 262)
      say(why, why_size, "%s: pixel format %u is a depth or stencil format of Metal's "
                         "enumeration that this back end has no Vulkan format for by name "
                         "(it maps Depth16Unorm, Depth32Float, Stencil8 and "
                         "Depth32Float_Stencil8)", what, f);
   /* [M5b: the rest of Metal's enumeration by family, as air_metal.c's pixel_format_refusal
    * has it, with what each lacks on this side of the contract.] */
   else if (f >= 130 && f <= 153)
      say(why, why_size, "%s: pixel format %u is one of the BC compressed formats. turnip has "
                         "them, the Gallium back end refuses them too, and they are not in the "
                         "Vulkan back end (M5b)", what, f);
   else if (f >= 160 && f <= 167)
      say(why, why_size, "%s: pixel format %u is one of the PVRTC compressed formats, which "
                         "Vulkan has only with VK_IMG_format_pvrtc and this GPU's driver does "
                         "not (M5b)", what, f);
   else if (f >= 222 && f <= 236)
      say(why, why_size, "%s: pixel format %u is one of the ASTC HDR formats, and turnip has no "
                         "textureCompressionASTC_HDR (M5b)", what, f);
   else if (f == 240 || f == 241)
      say(why, why_size, "%s: pixel format %u is one of Metal's packed 4:2:2 video formats "
                         "(GBGR422, BGRG422), where one block holds two texels: neither back "
                         "end has them, and a Vulkan one would be a sampler conversion and not "
                         "a format (M5b)", what, f);
   else if (f >= 552 && f <= 555)
      say(why, why_size, "%s: pixel format %u is one of Apple's extended-range XR formats, "
                         "which has no Vulkan format at all (M5b)", what, f);
   else
      say(why, why_size, "%s: MTLPixelFormat %u has no Vulkan format in this back end "
                         "(M5b), and it is either a format newer than the iOS 16.5 SDK's or "
                         "none at all", what, f);
}

/* ---------------------------------------------------------------------------------- */
/* Sample counts (MULTISAMPLE, M3c-3)                                                 */
/* ---------------------------------------------------------------------------------- */

/* A count of samples as Vulkan's flag bit, or 0 for a count Vulkan has no bit for.
 * Vulkan's counts are the powers of two up to 64 and nothing else, and the bit IS the
 * count, which is why this is a check and not a table. */
static VkSampleCountFlagBits
sample_bit(unsigned n)
{
   return n >= 1 && n <= 64 && util_is_power_of_two_nonzero(n) ? (VkSampleCountFlagBits)n
                                                             : (VkSampleCountFlagBits)0;
}

/* A mask of counts in words, "1, 2 and 4" -- or "none" -- for a sentence. */
static const char *
sample_count_words(VkSampleCountFlags f, char *buf, size_t size)
{
   unsigned n = 0, total = util_bitcount(f & 0x7f);
   size_t at = 0;
   buf[0] = '\0';
   if (!total) {
      snprintf(buf, size, "none");
      return buf;
   }
   for (unsigned bit = 1; bit <= 64; bit <<= 1) {
      if (!(f & bit))
         continue;
      n++;
      int w = snprintf(buf + at, at < size ? size - at : 0, "%s%u",
                       n == 1 ? "" : n == total ? " and " : ", ", bit);
      if (w > 0)
         at += (size_t)w;
   }
   return buf;
}

/* What a format of this table is used as when it is rendered into: a colour or a
 * depth/stencil attachment, and 0 when the format cannot be either here (turnip's own
 * format features say, asked rather than assumed). */
static VkImageUsageFlags
attachment_usage_of(const struct rwa_device *device, const struct rwa_vk_format *fmt)
{
   VkFormatProperties fp;
   device->vk.GetPhysicalDeviceFormatProperties(device->physical, fmt->vk, &fp);
   if (format_is_depth_stencil(fmt))
      return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0;
   return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
             ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
}

/* The sample counts turnip makes a 2D optimal image of `vk` with, for exactly `usage`:
 * vkGetPhysicalDeviceImageFormatProperties's sampleCounts, which is what VkImageCreateInfo
 * .samples is checked against (VkImageCreateInfo's valid usage). 0 when turnip makes
 * no such image at all. On an a6xx that is 1, 2 and 4 for a format that can be an
 * attachment and an image that is not a storage image, and 1 otherwise
 * (tu_get_image_format_properties) -- but it is asked, not written down here. */
static VkSampleCountFlags
image_sample_counts(const struct rwa_device *device, VkFormat vk, VkImageUsageFlags usage)
{
   VkImageFormatProperties ifp;
   VkResult r = device->vk.GetPhysicalDeviceImageFormatProperties(
      device->physical, vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ifp);
   return r == VK_SUCCESS ? ifp.sampleCounts : 0;
}

/*
 * Said once a process at rwa_device_open, beside the limits: which sample counts turnip
 * offers for a texture of each format of the table, made the way a multisample texture of
 * this library is made -- sampled, copied, and rendered into where the format can be --
 * grouped by the answer so the line stays one line, and the pass limits after it. The
 * formats are named without their "MTLPixelFormat" prefix.
 */
static void
say_sample_counts(struct rwa_device *device)
{
   VkSampleCountFlags mask[ARRAY_SIZE(rwa_vk_formats)];
   bool said[ARRAY_SIZE(rwa_vk_formats)] = { false };
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_vk_formats); i++)
      mask[i] = image_sample_counts(device, rwa_vk_formats[i].vk,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    attachment_usage_of(device, &rwa_vk_formats[i]));
   printf("air_vk: sample counts for a sampled texture rendered into where its format can "
          "be:");
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_vk_formats); i++) {
      if (said[i])
         continue;
      char words[48];
      printf("%s %s:", i ? ";" : "", sample_count_words(mask[i], words, sizeof(words)));
      for (unsigned j = i; j < ARRAY_SIZE(rwa_vk_formats); j++) {
         if (said[j] || mask[j] != mask[i])
            continue;
         said[j] = true;
         const char *n = rwa_vk_formats[j].name;
         printf(" %s", strncmp(n, "MTLPixelFormat", 14) ? n : n + 14);
      }
   }
   char c[48], d[48], s[48], dr[16], sr[16];
   snprintf(dr, sizeof(dr), "%#x", device->depth_resolve_modes);
   snprintf(sr, sizeof(sr), "%#x", device->stencil_resolve_modes);
   printf(". A pass: colour %s, depth %s, stencil %s samples; depth resolve modes %s, "
          "stencil %s (SAMPLE_ZERO is 0x1)\n",
          sample_count_words(device->fb_color_samples, c, sizeof(c)),
          sample_count_words(device->fb_depth_samples, d, sizeof(d)),
          sample_count_words(device->fb_stencil_samples, s, sizeof(s)), dr, sr);
}

/* Where an imported texture's memory comes from (TEXTURES OVER AN IOSURFACE, M3e): an
 * IOSurface by its ID, with the offset and the row length the kernel reported about it,
 * which hkdrawable.m read with kHKSurface's describe and passes on. NULL everywhere below
 * means "allocate", which is rwa_texture_create's whole difference from
 * rwa_texture_from_surface -- air_metal.c's struct of the same name and use.
 */
struct rwa_surface_import {
   int id;
   unsigned offset, row_bytes;
};

/* The row pitch an imported surface's rows must be a multiple of, by the size of a texel:
 * 16 texels for a texel wider than four bytes, 64 bytes for every other. That is turnip's
 * rule for a linear image of an explicit layout -- fdl6_layout (src/freedreno/fdl/
 * fd6_layout.c) takes pitchalign 0, or fdl_cpp_shift - 2 above four bytes, plus 6, and
 * refuses a given pitch that is not aligned to it -- and it is the same number freedreno's
 * Gallium import requires (fd_resource_from_handle: fdl_cpp_shift + log2(gmem_align_w),
 * gmem_align_w 16 on a6xx, at least 6), byte for byte at 1, 2, 4, 8 and 16 bytes a texel.
 * Both come from one fact: the GPU stores a render target 16 texels at a time.
 */
static unsigned
import_pitch_align(unsigned bytes)
{
   return bytes > 4 ? 16 * bytes : 64;
}

/* The view type of a MTLTextureType, which is also the type of the uniform lower_stage makes of a
 * shader's texture of that kind (M5b). */
static VkImageViewType
view_type_of(unsigned type)
{
   switch (type) {
   case RWA_TYPE_1D:                   return VK_IMAGE_VIEW_TYPE_1D;
   case RWA_TYPE_1D_ARRAY:             return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
   case RWA_TYPE_2D_ARRAY:
   case RWA_TYPE_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   case RWA_TYPE_CUBE:                 return VK_IMAGE_VIEW_TYPE_CUBE;
   case RWA_TYPE_CUBE_ARRAY:           return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
   case RWA_TYPE_3D:                   return VK_IMAGE_VIEW_TYPE_3D;
   default:                            return VK_IMAGE_VIEW_TYPE_2D;
   }
}

static struct rwa_texture *
texture_of_shape(struct rwa_device *device, const struct rwa_texture_shape *shape,
                 const struct rwa_surface_import *import, char *why, size_t why_size)
{
   if (!shape) {
      say(why, why_size, "a texture with no descriptor");
      return NULL;
   }
   /*
    * TEXTURES OVER AN IOSURFACE (M3e): what an imported surface can be, in air_metal.c's
    * words, every one a refusal rather than an adjustment, and asked before anything about
    * the format: the surface is row-major memory somebody else laid out, and all this side
    * is told about that layout is one pitch and one offset, so a mip chain, an array, a
    * cube or a second sample would need an agreement with the allocator about where the
    * rest lives, and there is none. A drawable is none of those things.
    *
    * YCbCr first and by name, because the window server asks for it (MTLPixelFormat 500,
    * docs/metal.md 10.46) and because the honest reason is not "an unknown format": it is
    * two planes, and a per-plane import is in neither back end. Gallium has no biplanar
    * format in air_metal.c's table and the kernel reports no per-plane geometry
    * (kHKSurface answers the plane count only), so hkdrawable.m refuses 500-629 before
    * either library is asked; this is the same refusal for a caller that comes here
    * directly.
    */
   if (import) {
      const char *type_name = texture_type_name(shape->type);
      unsigned mips = shape->mip_levels ? shape->mip_levels : 1;
      unsigned layers = shape->array_length ? shape->array_length : 1;
      unsigned depth = shape->depth ? shape->depth : 1;
      if (shape->pixel_format >= 500 && shape->pixel_format <= 629) {
         say(why, why_size, "pixel format %u over IOSurface %d is %s: YCbCr in two planes, "
                            "and an import of one plane of a surface as a texture of its own "
                            "is in neither back end (the kernel reports no per-plane offset "
                            "or row length, and the Gallium library has no biplanar format "
                            "either). It is not in the Vulkan back end yet (M3e)",
             shape->pixel_format, import->id,
             shape->pixel_format == 500 ? "MTLPixelFormatYCBCR8_420_2P"
                                        : "one of Metal's YCbCr formats (500-629)");
         return NULL;
      }
      if (!device->can_import) {
         say(why, why_size, "%s was made without VK_KHR_external_memory_fd, "
                            "VK_EXT_external_memory_dma_buf and "
                            "VK_EXT_image_drm_format_modifier (it did not list all three, or "
                            "is below Vulkan 1.2), and an IOSurface is imported through "
                            "exactly those; a texture over IOSurface %d is not in the Vulkan "
                            "back end on this device (M3e)", device->name, import->id);
         return NULL;
      }
      if (shape->type != RWA_TYPE_2D) {
         say(why, why_size, "a %s over an IOSurface: an imported surface is row-major "
                            "memory with one pitch, and only a MTLTextureType2D can be laid "
                            "over it without an agreement about where further levels live",
             type_name);
         return NULL;
      }
      if (mips != 1) {
         say(why, why_size, "%u mip levels over an IOSurface: the import is told one pitch "
                            "and nothing about smaller levels, so there is nowhere for them "
                            "to be", mips);
         return NULL;
      }
      if (layers != 1 || depth != 1) {
         say(why, why_size, "%u slices and a depth of %u over an IOSurface: the import is "
                            "one plane of one image", layers, depth);
         return NULL;
      }
      if (import->row_bytes == 0) {
         say(why, why_size, "an IOSurface with no bytes a row: the import takes the pitch "
                            "from the caller and cannot guess it");
         return NULL;
      }
   }
   /* One 2D level of one sample: what M3b draws into, samples and copies. Each other
    * shape is a refusal by name, so the log of the first application that asks for one
    * says which. M3c-1 adds the 3D texture (3D TEXTURES above), with the same one level
    * and one sample, and one "slice" in Metal's sense (arrayLength): its depth is the
    * third extent, not a slice.
    */
   /* [M3c-3: and MTLTextureType2DMultisample (MULTISAMPLE above), one level and one slice
    * of N samples. Its array, MTLTextureType2DMultisampleArray, is refused with the other
    * arrays, by name.] */
   /* [M5b: and now every type air_metal.c makes (TEXTURES, LEVELS AND SLICES), with its
    * refusals in its order and its sentences: texture_of_shape there, from "Metal's descriptor
    * defaults these to 1". A texture buffer stays its refusal, here as there.] */
   if (shape->type > RWA_TYPE_TEXTURE_BUFFER) {
      say(why, why_size, "texture type %u is not one of MTLTextureType's ten", shape->type);
      return NULL;
   }
   const char *type_word = texture_type_name(shape->type);
   if (shape->type == RWA_TYPE_TEXTURE_BUFFER) {
      say(why, why_size, "a %s: this is a buffer behind a texture's interface, which the "
                         "frontend refuses on the shader side too", type_word);
      return NULL;
   }
   bool volume = shape->type == RWA_TYPE_3D;
   bool multisample = shape->type == RWA_TYPE_2D_MULTISAMPLE ||
                      shape->type == RWA_TYPE_2D_MULTISAMPLE_ARRAY;
   bool one_d = shape->type == RWA_TYPE_1D || shape->type == RWA_TYPE_1D_ARRAY;
   bool cube = shape->type == RWA_TYPE_CUBE || shape->type == RWA_TYPE_CUBE_ARRAY;
   bool arrayed = shape->type == RWA_TYPE_1D_ARRAY || shape->type == RWA_TYPE_2D_ARRAY ||
                  shape->type == RWA_TYPE_CUBE_ARRAY ||
                  shape->type == RWA_TYPE_2D_MULTISAMPLE_ARRAY;
   /* Metal's descriptor defaults sampleCount to 1, so a zero is a struct filled in only
    * as far as its caller cared, and is one sample -- air_metal.c's reading. */
   unsigned samples = shape->sample_count ? shape->sample_count : 1;
   unsigned levels = shape->mip_levels ? shape->mip_levels : 1;
   unsigned slices = shape->array_length ? shape->array_length : 1;
   unsigned tall = shape->height ? shape->height : 1;
   unsigned deep = shape->depth ? shape->depth : 1;
   if (one_d && (tall != 1 || deep != 1)) {
      say(why, why_size, "a %s of %ux%ux%u: a 1D texture has one row and one slice", type_word,
          shape->width, tall, deep);
      return NULL;
   }
   if (!one_d && !volume && !cube && deep != 1) {
      say(why, why_size, "a %s with a depth of %u: a 2D texture has one slice, and an "
                         "array of them is arrayLength and not depth", type_word, deep);
      return NULL;
   }
   if (cube && (shape->width != tall || deep != 1)) {
      say(why, why_size, "a %s of %ux%ux%u: a cube's faces are square", type_word, shape->width,
          tall, deep);
      return NULL;
   }
   if (volume && shape->array_length > 1) {
      say(why, why_size, "a %s with an arrayLength of %u: Metal has no array of 3D "
                         "textures", type_word, shape->array_length);
      return NULL;
   }
   if (shape->type == RWA_TYPE_CUBE_ARRAY && !device->cube_array) {
      say(why, why_size, "a %s on %s, which has no imageCubeArray (M5b)", type_word,
          device->name);
      return NULL;
   }
   /* The layers of the image: Gallium's array_size, a cube's six in it (air_metal.c: "a cube
    * array is six slices per cube"), and of a type with no array one, whatever arrayLength
    * said -- a descriptor's arrayLength there is ignored by both APIs. */
   unsigned layers = (arrayed ? slices : 1) * (cube ? 6 : 1);
   /* The sample count by Metal's own rules about it, which are air_metal.c's refusals and
    * its sentences: those quote Metal itself (its __cstring, not the validation layer),
    * and a one-sample multisample texture is refused for the reason given there -- with
    * one sample the index can only be 0, and a shader that ignored it would pass. */
   if (!multisample && samples != 1) {
      say(why, why_size, "a %s with a sampleCount of %u: only a multisample type has more "
                         "than one sample per texel", texture_type_name(shape->type), samples);
      return NULL;
   }
   if (multisample && samples < 2) {
      say(why, why_size, "a %s with a sampleCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor sampleCount must be > 1 for multisample "
                         "textures\", and a texture of one sample is a MTLTextureType2D",
          texture_type_name(shape->type), shape->sample_count);
      return NULL;
   }
   if (multisample && shape->mip_levels > 1) {
      say(why, why_size, "a %s with a mipmapLevelCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor mipmapLevelCount must equal 1 for "
                         "multisample textures\", and there is no filtered fetch from a "
                         "multisample texture for a smaller level to serve",
          texture_type_name(shape->type), shape->mip_levels);
      return NULL;
   }
   /* [M5b: "only one level and one slice are in the Vulkan back end yet (M3b)" stood here. And
    * air_metal.c's check in its place, which that one could not have needed: a mip chain no
    * larger than the texture. A shader asking for level 7 of a 4x4 texture is a shader that
    * believes something else about the image it was given.] */
   {
      unsigned halvings = 1;
      for (unsigned e = MAX3(shape->width, tall, volume ? deep : 1); e > 1; e >>= 1)
         halvings++;
      if (levels > halvings) {
         say(why, why_size, "%u mip levels of a %ux%ux%u texture, which has %u", levels,
             shape->width, tall, volume ? deep : 1, halvings);
         return NULL;
      }
   }
   /* A count Vulkan has no flag bit for is refused before turnip is asked about it: the
    * question would have no words. Metal's own counts are 1, 2, 4 and 8, and which of
    * them this GPU gives is turnip's answer, asked below once the usage is known. */
   VkSampleCountFlagBits sample_flag = sample_bit(samples);
   if (!sample_flag) {
      say(why, why_size, "a %s of %u samples per texel: Vulkan counts samples in powers of "
                         "two (VkSampleCountFlagBits), and Metal's own counts are 1, 2, 4 "
                         "and 8", texture_type_name(shape->type), samples);
      return NULL;
   }
   /* A multisample texture is the other kind no shader stores to, in Metal (texture2d_ms
    * takes access::read and nothing else) and in Vulkan (a storage image of more than one
    * sample needs shaderStorageImageMultisample, and turnip's storageImageSampleCounts is
    * 1). air_metal.c's refusal, sentence for sentence. */
   if (multisample && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s with MTLTextureUsageShaderWrite: Metal has no multisample "
                         "texture a shader stores to (texture2d_ms takes access::read and "
                         "nothing else) and neither has this GPU, which declines an image "
                         "binding for any sample count above one, so there is no binding "
                         "this could be given", texture_type_name(shape->type));
      return NULL;
   }
   /* Metal's descriptor defaults the depth to 1 and never to 0, so a 0 is a caller's
    * struct filled in only as far as it cared about, taken as 1 -- air_metal.c's reading
    * of the same field.
    */
   unsigned depth = volume && shape->depth ? shape->depth : 1;
   if (shape->width == 0 || (shape->height == 0 && !one_d)) {
      say(why, why_size, "a texture of %ux%u texels has no texels in it", shape->width,
          shape->height);
      return NULL;
   }
   /* A 3D image's limit is its own and is smaller than a 2D one's: 2048 a side on this
    * Adreno (tu_device.cc maxImageDimension3D; tu_formats.cc gives the same maxExtent for
    * VK_IMAGE_TYPE_3D), and Metal's own limit for a 3D texture on this class of GPU is the
    * same 2048. Past it vkCreateImage is not an error but undefined, so it is asked here.
    */
   if (volume && (shape->width > device->max_3d || shape->height > device->max_3d ||
                  depth > device->max_3d)) {
      say(why, why_size, "a %ux%ux%u MTLTextureType3D: %s makes 3D images of up to %u texels "
                         "a side (maxImageDimension3D)", shape->width, shape->height, depth,
          device->name, device->max_3d);
      return NULL;
   }
   const struct rwa_vk_format *fmt = vk_format_of(shape->pixel_format);
   if (!fmt || fmt->view_only) {
      format_refusal(why, why_size, "a texture", shape->pixel_format);
      return NULL;
   }
   /* [M5b: one format of the table is named by an extension (the comment at the table).] */
   if (fmt->vk == VK_FORMAT_A8_UNORM_KHR && !device->maintenance5) {
      say(why, why_size, "%s on %s, which does not list VK_KHR_maintenance5, the extension "
                         "that names it (M5b)", fmt->name, device->name);
      return NULL;
   }
   /* What the format can do here, asked of turnip rather than assumed, for each use the
    * descriptor asked for: a format that cannot be rendered into is a sentence now, not a
    * failure at the first draw.
    *
    * The same question serves a 3D texture: format features are not per image type in
    * Vulkan, and what turnip's image-format answer adds for VK_IMAGE_TYPE_3D
    * (tu_get_image_format_properties) is only the extent above, one layer, one sample and
    * no depth/stencil format -- none of which this table holds. [M3c-2: the table holds
    * four depth/stencil formats now, and a 3D texture of one is refused just below.]
    *
    * MTLTextureUsageRenderTarget on a 3D texture is honoured at creation, for the reason
    * ShaderWrite is below: an application may set it on a texture it never renders into.
    * Rendering into one is refused at the draw (M3c), where it is asked for.
    *
    * A DEPTH/STENCIL format (M3c-2) turns the same usage bits into different Vulkan
    * usages, for air_metal.c's reason (PIPE_BIND_DEPTH_STENCIL where colour has
    * PIPE_BIND_RENDER_TARGET): MTLTextureUsageRenderTarget is the DEPTH_STENCIL_ATTACHMENT
    * usage and the feature asked for is DEPTH_STENCIL_ATTACHMENT, never COLOR_ATTACHMENT,
    * which no depth format has. Two refusals come first, both air_metal.c's or Vulkan's:
    * MTLTextureUsageShaderWrite (no shader stores to a depth texture, in Metal or here)
    * and a 3D depth texture (Metal has none, and Vulkan makes no 3D image of a depth
    * format -- tu_get_image_format_properties declines it).
    */
   bool zs = format_is_depth_stencil(fmt);
   /* [M5b: MTLTextureUsage as air_metal.c reads it, which this file had not needed: a bit that
    * is none of Metal's is refused; zero is Metal's "assume anything", which is a shader's read,
    * a shader's write where the format has one and a depth format's attachment; and a usage with
    * none of the three (PixelFormatView alone) still has to be readable. The write is settled
    * below, once turnip has said what the format can do.] */
   unsigned use = shape->usage;
   const unsigned known_usage = RWA_USAGE_SHADER_READ | RWA_USAGE_SHADER_WRITE |
                                RWA_USAGE_RENDER_TARGET | RWA_USAGE_PIXEL_FORMAT_VIEW;
   if (use & ~known_usage) {
      say(why, why_size, "usage bits 0x%x are not MTLTextureUsage's, and this library will "
                         "not guess what they allow", use & ~known_usage);
      return NULL;
   }
   if (zs && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s of %s with MTLTextureUsageShaderWrite: no shader stores to "
                         "a depth or stencil texture, in Metal or on this GPU, so there is "
                         "no binding this could be given", texture_type_name(shape->type),
          fmt->name);
      return NULL;
   }
   if (zs && volume) {
      say(why, why_size, "a MTLTextureType3D of %s: Metal has no 3D depth or stencil "
                         "texture, and Vulkan makes no 3D image of a depth/stencil format",
          fmt->name);
      return NULL;
   }
   /* (M3e) No depth over an IOSurface: turnip renders depth and stencil only into tiled
    * images -- its linear tiling has no DEPTH_STENCIL_ATTACHMENT feature at all
    * (tu_formats.cc, "we can't support sysmem rendering with a linear depth texture") --
    * and a surface of this kernel is linear and holds no depth format. */
   if (zs && import) {
      say(why, why_size, "a %s of %s over IOSurface %d: turnip renders depth and stencil "
                         "only into tiled images, an IOSurface is linear rows, and no surface "
                         "of this kernel holds depth; it is not in the Vulkan back end (M3e)",
          texture_type_name(shape->type), fmt->name, import->id);
      return NULL;
   }
   VkFormatProperties fp;
   device->vk.GetPhysicalDeviceFormatProperties(device->physical, fmt->vk, &fp);
   /* An imported surface is linear rows, so what it can be used for is the format's
    * LINEAR features -- which on turnip are exactly what its LINEAR modifier offers
    * (tu_formats.cc: drmFormatModifierTilingFeatures = linearTilingFeatures), and for the
    * colour formats of the table the same as the optimal ones but depth. */
   VkFormatFeatureFlags have = import ? fp.linearTilingFeatures : fp.optimalTilingFeatures;
   if (!use)
      use = RWA_USAGE_SHADER_READ | (zs ? RWA_USAGE_RENDER_TARGET : 0) |
            (!zs && !multisample && (have & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                ? RWA_USAGE_SHADER_WRITE : 0);
   if (!(use & (RWA_USAGE_SHADER_READ | RWA_USAGE_SHADER_WRITE | RWA_USAGE_RENDER_TARGET)))
      use |= RWA_USAGE_SHADER_READ;
   VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   const char *missing = NULL;
   if ((use & RWA_USAGE_RENDER_TARGET) && zs) {
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      if (!(have & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
         missing = "tested and written as a depth/stencil attachment "
                   "(MTLTextureUsageRenderTarget)";
   } else if (use & RWA_USAGE_RENDER_TARGET) {
      /* M5d: and read, by a shader that takes it as [[color(n)]] (FRAMEBUFFER FETCH) */
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
      need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      if (!(have & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
         missing = "rendered into (MTLTextureUsageRenderTarget)";
   }
   if (use & RWA_USAGE_SHADER_READ) {
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
      need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      if (!(have & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
         missing = "sampled (MTLTextureUsageShaderRead)";
   }
   /* MTLTextureUsageShaderWrite is honoured at creation, as air_metal.c honours it: an
    * application sets it on textures no shader of its ever writes, and refusing the
    * texture would refuse those too. What is not here yet is a pipeline that WRITES one
    * (a storage image), and that is refused by name where it is asked for.
    * [M3d: a compute pipeline writes one now (COMPUTE, STORAGE IMAGES); a render
    * pipeline's stage that writes one is still the refusal.]
    */
   if (use & RWA_USAGE_SHADER_WRITE) {
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;
      need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
      if (!(have & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
         missing = "written by a shader (MTLTextureUsageShaderWrite)";
   }
   if (!missing && (have & need) != need)
      missing = "copied to and from memory";
   if (missing) {
      say(why, why_size, "%s cannot be %s on %s%s", fmt->name, missing, device->name,
          import ? " in linear rows, which is what an IOSurface is" : "");
      return NULL;
   }
   /*
    * (M3e) The pitch and the offset of an imported surface, by the rules both GPU drivers
    * share (import_pitch_align above), checked here so that a failure names the number
    * that failed rather than leaving a VK_ERROR to be guessed at. The first two are
    * air_metal.c's own checks in its words -- the pitch at least the image, and aligned --
    * except that the alignment is the one for this texel size: air_metal.c checks 64 and
    * leaves an 8-byte format's 128 to freedreno, whose failure then comes back as its
    * sentence about the surface itself. The kernel's surfaces are aligned to 64
    * (MINISURF_ROW_ALIGN), so an RGBA16Float surface of a width that is not a multiple of
    * 16 -- 1080, the panel's -- has rows neither driver takes. The offset is where the
    * pixels start in the buffer, and a linear image's base must be 64-byte aligned
    * (fdl6_layout's base_align for a linear layout); the kernel's own surfaces start at 0.
    *
    * Then turnip itself is asked, for exactly this import: a 2D image of this format and
    * usage, of the linear modifier, from a dma-buf (vkGetPhysicalDeviceImageFormatProperties2
    * with the modifier and the handle type). That is the answer vkCreateImage would be held
    * to, and its extent limit with it.
    */
   if (import) {
      unsigned texel = fmt->bytes;
      if ((uint64_t)import->row_bytes < (uint64_t)shape->width * texel) {
         say(why, why_size, "IOSurface %d has %u bytes a row, and %u texels of %u bytes "
                            "need at least %llu: this GPU's import refuses a pitch narrower "
                            "than the image", import->id, import->row_bytes, shape->width,
             texel, (unsigned long long)shape->width * texel);
         return NULL;
      }
      unsigned align = import_pitch_align(texel);
      if (import->row_bytes % align != 0) {
         say(why, why_size, "IOSurface %d has %u bytes a row, which is not a multiple of %u: "
                            "this GPU stores a render target 16 texels at a time, so a linear "
                            "image of %u-byte texels needs rows of a multiple of %u bytes "
                            "(turnip's fdl6_layout, and freedreno's fd_resource_from_handle "
                            "on the Gallium side, refuse it alike); the kernel aligns a row "
                            "to 64 (MINISURF_ROW_ALIGN)", import->id, import->row_bytes, align,
             texel, align);
         return NULL;
      }
      if (import->offset % 64 != 0) {
         say(why, why_size, "IOSurface %d's pixels start at offset %u, which is not a "
                            "multiple of 64: a linear image's base on this GPU is 64-byte "
                            "aligned (fdl6_layout's base_align), and an image that started "
                            "elsewhere would be read from the wrong place", import->id,
             import->offset);
         return NULL;
      }
      const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
         .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      const VkPhysicalDeviceExternalImageFormatInfo external_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
         .pNext = &modifier_info,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      const VkPhysicalDeviceImageFormatInfo2 info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
         .pNext = &external_info,
         .format = fmt->vk,
         .type = VK_IMAGE_TYPE_2D,
         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
         .usage = usage,
      };
      VkExternalImageFormatProperties external_props = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
      };
      VkImageFormatProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
         .pNext = &external_props,
      };
      VkResult q = device->vk.GetPhysicalDeviceImageFormatProperties2(device->physical, &info,
                                                                      &props);
      VkExternalMemoryFeatureFlags ef =
         external_props.externalMemoryProperties.externalMemoryFeatures;
      if (q != VK_SUCCESS || !(ef & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
         say(why, why_size, "turnip imports no linear dma-buf image of %s for what this "
                            "texture is made for (vkGetPhysicalDeviceImageFormatProperties2: "
                            "%s%s), so IOSurface %d cannot be one", fmt->name,
             vk_Result_to_str(q), q == VK_SUCCESS ? ", and not importable" : "", import->id);
         return NULL;
      }
      if (shape->width > props.imageFormatProperties.maxExtent.width ||
          shape->height > props.imageFormatProperties.maxExtent.height) {
         say(why, why_size, "a %ux%u image over IOSurface %d: turnip imports images of up to "
                            "%ux%u", shape->width, shape->height, import->id,
             props.imageFormatProperties.maxExtent.width,
             props.imageFormatProperties.maxExtent.height);
         return NULL;
      }
   }
   /* The sample count (MULTISAMPLE), asked of turnip for this format and exactly this
    * usage, because that is the question Vulkan checks VkImageCreateInfo.samples against;
    * the answer is named in the refusal so a count this GPU does not give says which ones
    * it does. 8 is the commonest such count: Metal has it, a7xx gives it, an a6xx does not
    * (and freedreno's Gallium driver declines it too: valid_sample_count). */
   if (multisample) {
      VkSampleCountFlags offered = image_sample_counts(device, fmt->vk, usage);
      if (!(offered & sample_flag)) {
         char words[48];
         say(why, why_size, "a %s of %s with %u samples per texel: %s makes this format, "
                            "for what this texture is made for, with %s samples (sampleCounts), "
                            "and %u is not among them (M3c)", texture_type_name(shape->type),
             fmt->name, samples, device->name,
             sample_count_words(offered, words, sizeof(words)), samples);
         return NULL;
      }
   }

   /* [M5b: what the image is made as, and turnip asked for exactly that, which is the answer
    * vkCreateImage is held to: a limit passed is undefined there and a sentence here.
    *   CUBE_COMPATIBLE on a cube, and on an array that a view could call one: Metal allows the
    *     view, Vulkan allows it only of an image made so, and the bit costs nothing in turnip.
    *   2D_ARRAY_COMPATIBLE on a 3D render target: a pass renders one depth plane through a 2D
    *     view (TEXTURES, LEVELS AND SLICES, ITS VIEWS).
    *   MUTABLE_FORMAT on what asked for MTLTextureUsagePixelFormatView, and on nothing else:
    *     turnip may give up UBWC for it.] */
   VkImageCreateFlags image_flags = 0;
   if (cube || (shape->type == RWA_TYPE_2D_ARRAY && layers >= 6 && shape->width == tall))
      image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   if (volume && (use & RWA_USAGE_RENDER_TARGET))
      image_flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
   if ((use & RWA_USAGE_PIXEL_FORMAT_VIEW) && !import)
      image_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   const VkImageType image_type = volume ? VK_IMAGE_TYPE_3D
                                : one_d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
   if (!import) {
      VkImageFormatProperties ifp;
      VkResult q = device->vk.GetPhysicalDeviceImageFormatProperties(
         device->physical, fmt->vk, image_type, VK_IMAGE_TILING_OPTIMAL, usage, image_flags, &ifp);
      if (q != VK_SUCCESS) {
         say(why, why_size, "turnip makes no %s of %s for what this texture is made for "
                            "(vkGetPhysicalDeviceImageFormatProperties: %s) (M5b)", type_word,
             fmt->name, vk_Result_to_str(q));
         return NULL;
      }
      if (levels > ifp.maxMipLevels || layers > ifp.maxArrayLayers ||
          shape->width > ifp.maxExtent.width || tall > ifp.maxExtent.height ||
          depth > ifp.maxExtent.depth) {
         say(why, why_size, "a %s of %s of %ux%ux%u with %u levels and %u layers: turnip makes "
                            "it up to %ux%ux%u, %u levels and %u layers (M5b)", type_word,
             fmt->name, shape->width, tall, depth, levels, layers, ifp.maxExtent.width,
             ifp.maxExtent.height, ifp.maxExtent.depth, ifp.maxMipLevels, ifp.maxArrayLayers);
         return NULL;
      }
   }

   struct rwa_texture *t = calloc(1, sizeof(*t));
   if (!t) {
      say(why, why_size, "out of memory making a texture");
      return NULL;
   }
   t->device = device;
   t->format = fmt;
   t->aspects = format_aspects(fmt);
   t->shape = *shape;
   /* [M5b: a field a caller left at zero is Metal's default, as above, from here on: `shape` is
    * what every later reader takes a size and a count from.] */
   t->shape.height = tall;
   t->shape.depth = depth;
   t->shape.array_length = slices;
   t->shape.mip_levels = levels;
   t->shape.sample_count = samples;
   t->root = t;
   t->refs = 1;
   t->levels = levels;
   t->layers = layers;
   t->cube_compatible = (image_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
   t->plane_compatible = (image_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0;
   t->mutable_format = (image_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0;
   t->depth = depth;
   t->samples = samples;
   t->sample_bit = sample_flag;
   /* An imported surface's memory already holds a picture somebody else drew, and every
    * barrier's old layout is this field: from UNDEFINED Vulkan may discard what is there,
    * which for a surface would be a frame thrown away by contract. So it starts as
    * GENERAL, the layout a linear image of external memory is in as its producer left it
    * (M3e). On turnip a linear image without UBWC has no layout of its own -- the bytes in
    * memory are the picture in every layout, and a barrier from UNDEFINED differs only by
    * a CCU flush (tu_cmd_buffer.cc) -- so this changes no command, only what the barriers
    * promise. Queue-family transfers to and from VK_QUEUE_FAMILY_FOREIGN_EXT are not
    * recorded: turnip ignores them, and what the processor side relies on is the kernel's
    * cache maintenance at submission (TEXTURES OVER AN IOSURFACE, COHERENCY). */
   t->layout = import ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
   t->surface_id = import ? import->id : 0;
   t->render_target = (use & RWA_USAGE_RENDER_TARGET) != 0;
   t->shader_read = (use & RWA_USAGE_SHADER_READ) != 0;
   t->shader_write = (use & RWA_USAGE_SHADER_WRITE) != 0;

   /* A 3D texture's depth is the image's third extent and NOT its layers (3D TEXTURES):
    * arrayLayers stays 1, as Vulkan requires of every VK_IMAGE_TYPE_3D.
    *
    * A multisample texture is a 2D image of its samples (MULTISAMPLE): the type stays
    * VK_IMAGE_TYPE_2D, and `samples` is the whole of the difference -- which is
    * freedreno's view of it as well (a PIPE_TEXTURE_2D of nr_samples, air_metal.c).
    *
    * An imported surface (M3e) is the same image in the surface's own layout, said the way
    * Linux says a dma-buf's: VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT with an EXPLICIT
    * layout of one plane -- DRM_FORMAT_MOD_LINEAR (MiniIOSurface has no tiling and no
    * compression), the kernel's offset and row pitch -- which turnip hands fdl6_layout as
    * it stands (tu_image_update_layout: plane_layouts[0].offset and .rowPitch). That is
    * air_metal.c's winsys_handle with WINSYS_HANDLE_TYPE_FD, stride, offset and
    * DRM_FORMAT_MOD_LINEAR, field for field. Vulkan requires an image of external memory
    * to say so (VkExternalMemoryImageCreateInfo) and to start UNDEFINED; what that means
    * for the contents is at t->layout above.
    */
   const VkSubresourceLayout plane = {
      .offset = import ? import->offset : 0,
      .rowPitch = import ? import->row_bytes : 0,
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_layout = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane,
   };
   const VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_layout,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = import ? &external : NULL,
      .flags = image_flags,
      .imageType = image_type,
      .format = fmt->vk,
      .extent = { shape->width, tall, depth },
      .mipLevels = levels,
      .arrayLayers = layers,
      .samples = sample_flag,
      .tiling = import ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult r = device->vk.CreateImage(device->dev, &ici, NULL, &t->image);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a %ux%ux%u %s image (%s)", shape->width,
          shape->height, depth, fmt->name, vk_Result_to_str(r));
      free(t);
      return NULL;
   }
   VkMemoryRequirements req;
   device->vk.GetImageMemoryRequirements(device->dev, t->image, &req);
   uint32_t type = memory_type(device, req.memoryTypeBits, 0,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   /*
    * An imported surface's memory (M3e) is a dma-buf import whose "file descriptor" is the
    * surface's ID -- the slot freedreno's import puts it in on the Gallium side, and the
    * one hk_drm.c's drmPrimeFDToHandle answers with kHKSurface. turnip-darwin.patch makes
    * the two places where turnip would treat it as a real descriptor take it as an ID:
    * msm_bo_init_dmabuf asks the kernel for the size (hk_drm_surface) instead of lseek()ing
    * it, and tu_AllocateMemory does not close() it. The size asked for is the image's
    * (offset + rows of the pitch, the last level's rows rounded up to 4 for the GPU's 16x4
    * stores, fdl6_layout); turnip refuses the import when the surface's buffer is smaller.
    * A dedicated allocation, because Vulkan requires one of an image of external memory
    * where turnip says so (tu_get_image_memory_requirements: requiresDedicatedAllocation).
    *
    * OWNERSHIP, as on Gallium: the buffer the kernel makes for the surface belongs to
    * turnip's table of buffers from here on (one per surface per process, counted, as the
    * kernel gives the same handle for the same surface again), and holds the surface for
    * as long as it is there; vkFreeMemory at rwa_texture_destroy gives it back
    * (tu_bo_finish: DRM_IOCTL_GEM_CLOSE, kDrmGemClose). Nothing here frees the surface's
    * memory, which is its owner's, and nothing here takes the IOSurfaceRef.
    */
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = t->image,
   };
   const VkImportMemoryFdInfoKHR import_fd = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = import ? import->id : -1,
   };
   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = import ? &import_fd : NULL,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (import && type != UINT32_MAX &&
       (r = device->vk.AllocateMemory(device->dev, &mai, NULL, &t->memory)) != VK_SUCCESS) {
      /* Everything this side could check has been checked, so what is left is named as
       * what is left, in air_metal.c's words for the same failure: the kernel declining to
       * hand the surface's memory over, and a surface ID its table does not hold -- and
       * one this side adds, a surface smaller than the image laid over it. */
      say(why, why_size, "%s would not take IOSurface %d as a %ux%u %s with %u bytes a row "
                         "from offset %u (%s), and the pitch and the shape are both "
                         "acceptable, so what is left is the surface itself: the kernel will "
                         "not give the GPU a surface whose pixels are memory of a process's "
                         "own or another program's panel buffer, it knows nothing of an ID "
                         "that was never made shareable, and a surface smaller than the "
                         "%llu bytes this image spans is refused", device->name, import->id,
          shape->width, shape->height, fmt->name, import->row_bytes, import->offset,
          vk_Result_to_str(r), (unsigned long long)req.size);
      t->memory = VK_NULL_HANDLE;
      goto fail;
   }
   if (type == UINT32_MAX ||
       (!import &&
        (r = device->vk.AllocateMemory(device->dev, &mai, NULL, &t->memory)) != VK_SUCCESS) ||
       (r = device->vk.BindImageMemory(device->dev, t->image, t->memory, 0)) != VK_SUCCESS) {
      say(why, why_size, "memory for a %ux%ux%u %s image could not be allocated and bound "
                         "(%s)", shape->width, shape->height, depth, fmt->name,
          type == UINT32_MAX ? "no memory type holds it" : vk_Result_to_str(r));
      goto fail;
   }
   /*
    * THE SURFACE MARKED "SEEN" (M3e, run 061800), which is what Gallium's path gets without
    * asking. The kernel cleans and invalidates the processor's cache over a surface's buffer
    * at a submission that names it when the surface's seed moved, when 4 ms have passed since
    * it last did, or -- always -- once the program has MAPPED that buffer
    * (iokit/Kernel/HKGPU.cpp: clientMemoryForType sets b->seen, submit() tests it). freedreno
    * maps an imported linear resource the first time the processor reads or writes it
    * through the driver (transfer_map -> fd_bo_map -> hk_drm_map, freedreno-darwin.patch),
    * so on Gallium the surface is seen from then on; turnip copies through staging and never
    * maps it, so on this back end it was cleaned only by the seed or the clock -- and a
    * processor line read within 4 ms of the last clean survived the GPU's next write: run
    * 060507/061800, whole 64-byte lines of the old picture after the fence, on vk alone.
    *
    * So the memory is mapped once here and unmapped at once: vkMapMemory -> tu_bo_map ->
    * msm_bo_map -> hk_drm_map -> IOConnectMapMemory64(connection, GEM handle) ->
    * HKGPUUserClient::clientMemoryForType, which sets b->seen; the flag stays for the
    * buffer's life, the unmap does not clear it. turnip's one memory type is host-visible, so
    * an imported dma-buf may be mapped.
    *
    * What this covers: lines of the surface in the processor's cache are dropped at every
    * submission naming the surface -- and turnip names every buffer of the device in every
    * submission -- so a read after a later submission sees the GPU's writes. What it does NOT
    * cover, and the kernel has to: a processor reading the surface WHILE the GPU writes it,
    * and a read after the GPU's writes with no submission in between (nothing invalidates the
    * processor's cache at a fence or at IOSurfaceLock). And its price: the kernel cleans the
    * whole surface at every submission of this process, which for a full-screen drawable is
    * ten megabytes each time. AIR_VK_NOSEEN=1 turns it off.
    */
   if (import && !device->no_seen) {
      void *map = NULL;
      if ((r = device->vk.MapMemory(device->dev, t->memory, 0, VK_WHOLE_SIZE, 0, &map)) !=
          VK_SUCCESS) {
         say(why, why_size, "turnip would not map IOSurface %d's memory (%s), and without that "
                            "the kernel does not clean the surface out of the processor's "
                            "cache at every submission, so the processor would read old "
                            "pixels after the GPU's", import->id, vk_Result_to_str(r));
         goto fail;
      }
      device->vk.UnmapMemory(device->dev, t->memory);
   }
   /* One view, for both of the things a view is for here: rendering into the texture and
    * sampling it. A view inherits the image's usage, so the one serves whichever of the
    * two the texture was made for.
    *
    * A 3D texture's view is VK_IMAGE_VIEW_TYPE_3D, the type its texture3d uniform is
    * (lower_stage), over the one layer that is all of its depth. That view samples and
    * cannot be a colour attachment -- which is the draw's refusal (M3c), not this one's.
    *
    * A depth texture has two views instead (DEPTH AND STENCIL, VIEWS), because Vulkan
    * wants one aspect in a sampled image's view and every aspect in an attachment's:
    * `view` samples the depth (or a Stencil8 texture's stencil) and is made when the
    * shader reads it; zs_view is every aspect and is made when it is rendered into.
    *
    * A multisample texture's views are these same views (MULTISAMPLE): VK_IMAGE_VIEW_TYPE_2D
    * of a multisample image, which is what a texture2DMS uniform is bound to, what a
    * multisample attachment renders into and what a resolve reads -- Vulkan has no view
    * type of its own for one.
    *
    * And the same view is what a kernel WRITES through (COMPUTE, STORAGE IMAGES, M3d): a
    * STORAGE_IMAGE descriptor takes an image view, and this one inherits STORAGE from the
    * image's usage, so a texture made with MTLTextureUsageShaderWrite alone has it too.
    */
   /* [M5b: of all its levels and layers, and of the type its shader's uniform is (ITS VIEWS). A
    * MTLTextureType2DMultisampleArray is a 2D_ARRAY of N samples, a cube's six layers a CUBE.] */
   t->view_type = view_type_of(shape->type);
   if (!zs && (t->render_target || t->shader_read || t->shader_write)) {
      const VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = t->image,
         .viewType = t->view_type,
         .format = fmt->vk,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers },
      };
      if ((r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->view)) != VK_SUCCESS) {
         say(why, why_size, "turnip would not make a view of a %s image (%s)", fmt->name,
             vk_Result_to_str(r));
         goto fail;
      }
   }
   /* [M5b: a kernel's storage image is one level (ITS VIEWS): Gallium's bound image is level 0
    * too (bind_stage, u.tex.level), and every layer. Where that is everything, the one view.] */
   if (!zs && t->shader_write) {
      t->store_view = t->view;
      if (levels > 1) {
         const VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->image,
            .viewType = t->view_type,
            .format = fmt->vk,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers },
         };
         if ((r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->store_view)) !=
             VK_SUCCESS) {
            t->store_view = VK_NULL_HANDLE;
            say(why, why_size, "turnip would not make a view of one level of a %s image for a "
                               "shader to write (%s)", fmt->name, vk_Result_to_str(r));
            goto fail;
         }
      }
   }
   if (zs && t->shader_read) {
      VkImageAspectFlags one = (t->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                                  ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
      const VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = t->image,
         .viewType = t->view_type,
         .format = fmt->vk,
         .subresourceRange = { one, 0, levels, 0, layers },
      };
      if ((r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->view)) != VK_SUCCESS) {
         say(why, why_size, "turnip would not make a view of the %s aspect of a %s image "
                            "for a shader to sample (%s)",
             one == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : "stencil", fmt->name,
             vk_Result_to_str(r));
         goto fail;
      }
   }
   /* [M5b: of the first level and layer, which is what it always was; another is att_view_for's.
    * Its presence still says what `render_target` says of a colour texture.] */
   if (zs && t->render_target) {
      const VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = t->image,
         .viewType = one_d ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D,
         .format = fmt->vk,
         .subresourceRange = { t->aspects, 0, 1, 0, 1 },
      };
      if ((r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->zs_view)) !=
          VK_SUCCESS) {
         say(why, why_size, "turnip would not make a depth/stencil attachment view of a %s "
                            "image (%s)", fmt->name, vk_Result_to_str(r));
         goto fail;
      }
   }
   p_atomic_inc(&device->children);
   p_atomic_inc(&rwa_live[RWA_LIVE_IMAGE]);
   return t;

fail:
   if (t->store_view && t->store_view != t->view)
      device->vk.DestroyImageView(device->dev, t->store_view, NULL);
   if (t->view)
      device->vk.DestroyImageView(device->dev, t->view, NULL);
   device->vk.DestroyImage(device->dev, t->image, NULL);
   if (t->memory)
      device->vk.FreeMemory(device->dev, t->memory, NULL);
   free(t);
   return NULL;
}

struct rwa_texture *
rwa_texture_create(struct rwa_device *device, const struct rwa_texture_shape *shape,
                   char *why, size_t why_size)
{
   return texture_of_shape(device, shape, NULL, why, why_size);
}

/*
 * TEXTURES OVER AN IOSURFACE (M3e): the memory of an IOSurface of somebody else's as a
 * texture, zero copy, which is what a CAMetalLayer drawable is (air_metal.h, and
 * userland/src/hkdrawable.m for the bridge that calls this). texture_of_shape above does
 * the work with an import; what is here is air_metal.c's one check of its own, the ID.
 */
struct rwa_texture *
rwa_texture_from_surface(struct rwa_device *device, const struct rwa_texture_shape *shape,
                         int surface_id, unsigned offset, unsigned row_bytes, char *why,
                         size_t why_size)
{
   /* A surface ID of zero is not a surface: IOSurfaceGetID answers 0 for one that was
    * never made shareable, and the kernel's table has nothing at 0 either. A negative one
    * would be handed to turnip as a descriptor of -1 or below, which it reads as none. */
   if (surface_id <= 0) {
      say(why, why_size, "IOSurface %d is not a surface: an ID of zero is what a surface "
                         "that was never made shareable answers, and there is nothing for "
                         "the kernel to look up", surface_id);
      return NULL;
   }
   const struct rwa_surface_import import = {
      .id = surface_id, .offset = offset, .row_bytes = row_bytes };
   return texture_of_shape(device, shape, &import, why, why_size);
}

/*
 * A TEXTURE VIEW (M5b, air_metal.h, and TEXTURES, LEVELS AND SLICES, rwa_texture_view_create). In
 * Metal's terms and with Metal's rules, each refused by name; what Vulkan cannot do of them says
 * "(M5b)". What the view reads is a VkImageView, with a VkComponentMapping; what it renders into is
 * att_view_for's, with none.
 */
static bool
view_types_compatible(unsigned from, unsigned to)
{
   static const unsigned char family[10] = {
      [RWA_TYPE_1D] = 1, [RWA_TYPE_1D_ARRAY] = 1, [RWA_TYPE_2D] = 2, [RWA_TYPE_2D_ARRAY] = 2,
      [RWA_TYPE_CUBE] = 2, [RWA_TYPE_CUBE_ARRAY] = 2, [RWA_TYPE_2D_MULTISAMPLE] = 3,
      [RWA_TYPE_2D_MULTISAMPLE_ARRAY] = 3, [RWA_TYPE_3D] = 4, [RWA_TYPE_TEXTURE_BUFFER] = 5,
   };
   return from < 10 && to < 10 && family[from] == family[to];
}

struct rwa_texture *
rwa_texture_view_create(struct rwa_texture *parent, const struct rwa_texture_view_shape *shape,
                        char *why, size_t why_size)
{
   if (!parent || !shape) {
      say(why, why_size, "a texture view needs a texture and a description of the view");
      return NULL;
   }
   struct rwa_device *device = parent->device;
   struct rwa_texture *root = parent->root;
   if (parent->plane_view) {
      say(why, why_size, "a view of a view of one depth plane: that view is for a pass's clear");
      return NULL;
   }
   const char *from_type = texture_type_name(parent->shape.type);
   const char *to_type = texture_type_name(shape->type);
   const bool plane = parent->shape.type == RWA_TYPE_3D && shape->type == RWA_TYPE_2D;
   if (shape->type > RWA_TYPE_TEXTURE_BUFFER ||
       (!plane && !view_types_compatible(parent->shape.type, shape->type))) {
      say(why, why_size, "a %s view of a %s: Metal views a type as another only within one "
                         "family (1D and its array; 2D, its array and the cubes; 2D multisample "
                         "and its array; 3D)", to_type, from_type);
      return NULL;
   }
   unsigned count_of_levels = shape->level_count ? shape->level_count
                            : parent->levels > shape->base_level
                                 ? parent->levels - shape->base_level : 0;
   if ((uint64_t)shape->base_level + count_of_levels > parent->levels || !count_of_levels) {
      say(why, why_size, "%u levels from level %u of a texture with %u", count_of_levels,
          shape->base_level, parent->levels);
      return NULL;
   }
   /* A slice of a 3D view is a depth plane, and the only such view is one of one plane. */
   const unsigned parent_depth = parent->shape.type == RWA_TYPE_3D
                                    ? MAX2(parent->depth >> shape->base_level, 1) : 1;
   const unsigned have_slices = plane ? parent_depth : parent->layers;
   unsigned count_of_slices = shape->slice_count ? shape->slice_count
                            : have_slices > shape->base_slice
                                 ? have_slices - shape->base_slice : 0;
   if ((uint64_t)shape->base_slice + count_of_slices > have_slices || !count_of_slices) {
      say(why, why_size, "%u slices from slice %u of a texture with %u", count_of_slices,
          shape->base_slice, have_slices);
      return NULL;
   }
   const bool to_cube = shape->type == RWA_TYPE_CUBE || shape->type == RWA_TYPE_CUBE_ARRAY;
   if ((shape->type == RWA_TYPE_CUBE && count_of_slices != 6) ||
       (shape->type == RWA_TYPE_CUBE_ARRAY && count_of_slices % 6) ||
       ((shape->type == RWA_TYPE_1D || shape->type == RWA_TYPE_2D ||
         shape->type == RWA_TYPE_2D_MULTISAMPLE) && count_of_slices != 1)) {
      say(why, why_size, "a %s view of %u slices: a cube is six, a cube array six each, and a "
                         "type with no array one", to_type, count_of_slices);
      return NULL;
   }
   if (to_cube && !root->cube_compatible) {
      say(why, why_size, "a %s view of a texture that was not made one that a cube could be "
                         "laid over (square, six slices or more): Vulkan views an image as a "
                         "cube only if it was made CUBE_COMPATIBLE (M5b)", to_type);
      return NULL;
   }
   if (shape->type == RWA_TYPE_CUBE_ARRAY && !device->cube_array) {
      say(why, why_size, "a %s view on %s, which has no imageCubeArray (M5b)", to_type,
          device->name);
      return NULL;
   }
   if (plane && (!root->plane_compatible || count_of_levels != 1 || count_of_slices != 1)) {
      say(why, why_size, "a view of %u levels and %u planes of a 3D texture %s: the one such "
                         "view there is, is of one plane of one level of a render target "
                         "(air_metal.h, and Vulkan's 2D_ARRAY_COMPATIBLE) (M5b)", count_of_levels,
          count_of_slices, root->plane_compatible ? "made a render target" : "not made one");
      return NULL;
   }
   const unsigned mtl = shape->pixel_format ? shape->pixel_format : parent->shape.pixel_format;
   const struct rwa_vk_format *fmt = vk_format_of(mtl);
   if (!fmt) {
      format_refusal(why, why_size, "a texture view", mtl);
      return NULL;
   }
   if (fmt->vk == VK_FORMAT_A8_UNORM_KHR && !device->maintenance5) {
      say(why, why_size, "a view as %s on %s, which does not list VK_KHR_maintenance5, the "
                         "extension that names it (M5b)", fmt->name, device->name);
      return NULL;
   }
   /* The format: Metal's rule and Vulkan's say the same, the same texel size on a texture that
    * was made to be reinterpreted. Depth has its one exception, the stencil of a combined format
    * as X32_Stencil8; and otherwise a depth format is viewed as itself, because Vulkan's rule
    * is the same format, and Metal's table has no other either. */
   const bool same_format = fmt->vk == root->format->vk;
   if (fmt->view_only && root->format->vk != VK_FORMAT_D32_SFLOAT_S8_UINT) {
      say(why, why_size, "a view as %s of a %s texture: that format is the stencil aspect of a "
                         "Depth32Float_Stencil8 one", fmt->name, parent->format->name);
      return NULL;
   }
   if (!same_format && (format_is_depth_stencil(fmt) || format_is_depth_stencil(root->format))) {
      say(why, why_size, "a view as %s of a %s texture: a depth or stencil format is viewed as "
                         "itself, in Metal and in Vulkan", fmt->name, parent->format->name);
      return NULL;
   }
   if (!same_format && (fmt->bytes != root->format->bytes ||
                        fmt_block_w(fmt) != fmt_block_w(root->format) ||
                        fmt_block_h(fmt) != fmt_block_h(root->format))) {
      say(why, why_size, "a view as %s of a %s texture: Metal views a format as another of the "
                         "same texel size, and these are %u and %u bytes", fmt->name,
          parent->format->name, fmt->bytes, root->format->bytes);
      return NULL;
   }
   if (!same_format && !root->mutable_format) {
      say(why, why_size, "a view as %s of a %s texture made without "
                         "MTLTextureUsagePixelFormatView: Vulkan reinterprets an image only if "
                         "it was made MUTABLE_FORMAT, and this back end makes one so only when "
                         "asked, because turnip may give up its compression then (M5b)",
          fmt->name, parent->format->name);
      return NULL;
   }
   /* MTLTextureSwizzle as VkComponentSwizzle, by table: Metal's starts at Zero 0 and Vulkan's
    * at IDENTITY 0, so a cast is off by one for all six. */
   static const VkComponentSwizzle swizzle_of[6] = {
      VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
      VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
   };
   unsigned sw[4];
   memcpy(sw, shape->swizzle, sizeof(sw));
   if (!sw[0] && !sw[1] && !sw[2] && !sw[3])
      sw[0] = 2, sw[1] = 3, sw[2] = 4, sw[3] = 5;   /* air_metal.h: four zeros are none at all */
   for (unsigned c = 0; c < 4; c++) {
      if (sw[c] > 5) {
         say(why, why_size, "a swizzle of %u for channel %u: MTLTextureSwizzle has six values",
             sw[c], c);
         return NULL;
      }
   }
   const bool swizzled = sw[0] != 2 || sw[1] != 3 || sw[2] != 4 || sw[3] != 5;
   /* A depth format may be swizzled: this refused it, saying a depth2d reads one value, and real
    * Metal takes the view and a depth2d read through abgr returns 1.0, the texel's alpha (M1 Max,
    * tools/air/mtlref). view-depth-swizzle in air_metal_test.c is the row. */

   struct rwa_texture *t = calloc(1, sizeof(*t));
   if (!t) {
      say(why, why_size, "out of memory making a texture view");
      return NULL;
   }
   t->device = device;
   t->image = root->image;
   t->root = root;
   t->format = fmt;
   t->aspects = format_aspects(fmt);
   t->base_level = parent->base_level + shape->base_level;
   t->levels = plane ? 1 : count_of_levels;
   t->base_layer = plane ? shape->base_slice : parent->base_layer + shape->base_slice;
   t->layers = plane ? 1 : count_of_slices;
   t->plane_view = plane;
   t->samples = parent->samples;
   t->sample_bit = parent->sample_bit;
   t->render_target = parent->render_target;
   t->shader_read = parent->shader_read;
   /* a swizzle is a sampling matter, and Vulkan's storage image has the identity */
   t->shader_write = parent->shader_write && !swizzled;
   t->surface_id = parent->surface_id;
   t->shape = parent->shape;
   t->shape.pixel_format = mtl;
   t->shape.type = shape->type;
   t->shape.width = MAX2(parent->shape.width >> shape->base_level, 1);
   t->shape.height = MAX2(parent->shape.height >> shape->base_level, 1);
   t->shape.depth = parent_depth;
   t->shape.mip_levels = t->levels;
   t->shape.array_length = to_cube ? count_of_slices / 6 : count_of_slices;
   t->depth = plane ? 1 : parent_depth;
   t->view_type = view_type_of(shape->type);
   t->swizzle = (VkComponentMapping){ swizzle_of[sw[0]], swizzle_of[sw[1]], swizzle_of[sw[2]],
                                      swizzle_of[sw[3]] };
   t->swizzled = swizzled;

   /* What the view reads, of one aspect where it is a depth format; and where the format is not
    * the image's, only what that format can do (VkImageViewUsageCreateInfo), since it may not be
    * able to do everything the image was made for. */
   VkFormatProperties fp;
   device->vk.GetPhysicalDeviceFormatProperties(device->physical, fmt->vk, &fp);
   VkImageUsageFlags view_usage = 0;
   if (t->shader_read && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      view_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
   else
      t->shader_read = false;
   if (t->shader_write && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
      view_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
   else
      t->shader_write = false;
   const bool is_zs = format_is_depth_stencil(fmt);
   VkImageAspectFlags one = !is_zs ? VK_IMAGE_ASPECT_COLOR_BIT
                          : (t->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                     : VK_IMAGE_ASPECT_STENCIL_BIT;
   VkResult r = VK_SUCCESS;
   if (view_usage) {
      const VkImageViewUsageCreateInfo vu = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
         .usage = view_usage,
      };
      const VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .pNext = &vu,
         .image = t->image,
         .viewType = t->view_type,
         .format = fmt->vk,
         .components = t->swizzle,
         .subresourceRange = { one, t->base_level, t->levels, t->base_layer, t->layers },
      };
      r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->view);
   }
   if (r == VK_SUCCESS && t->shader_write) {
      t->store_view = t->view;
      if (t->levels > 1) {
         const VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = t->image,
            .viewType = t->view_type,
            .format = fmt->vk,
            .subresourceRange = { one, t->base_level, 1, t->base_layer, t->layers },
         };
         r = device->vk.CreateImageView(device->dev, &vci, NULL, &t->store_view);
      }
   }
   /* (t->zs_view stays as the texture's own says: a pass takes att_view_for's.) */
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a %s view as %s of a %s image (%s)", to_type,
          fmt->name, parent->format->name, vk_Result_to_str(r));
      if (t->store_view && t->store_view != t->view)
         device->vk.DestroyImageView(device->dev, t->store_view, NULL);
      if (t->view)
         device->vk.DestroyImageView(device->dev, t->view, NULL);
      free(t);
      return NULL;
   }
   p_atomic_inc(&root->refs);
   p_atomic_inc(&device->children);
   p_atomic_inc(&rwa_live[RWA_LIVE_IMAGE]);
   return t;
}

/*
 * The one view of one level and one layer a pass renders into (ITS VIEWS), with the texture's
 * format and no swizzle. Kept until the texture goes, in a short list: a pass into a level is
 * the same pass again next frame, and a VkImageView made at every draw would be most of what it
 * costs. A texture of one level and one layer with no swizzle has that view already.
 *
 * `slice` is checked by the caller. On a 3D image it is a depth plane.
 */
static VkImageView
att_view_for(struct rwa_texture *t, unsigned level, unsigned slice, char *why, size_t why_size)
{
   struct rwa_device *device = t->device;
   const bool zs = format_is_depth_stencil(t->format);
   const bool volume = t->root->shape.type == RWA_TYPE_3D;
   if (level == 0 && slice == 0 && t->levels == 1 && t->layers == 1 && !t->swizzled && !volume &&
       t->base_level == 0 && t->base_layer == 0 && t->root == t)
      return zs ? t->zs_view : t->view;
   for (unsigned i = 0; i < t->att_count; i++)
      if (t->att[i].level == level && t->att[i].layer == slice)
         return t->att[i].view;
   const VkImageViewUsageCreateInfo vu = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = zs ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
   };
   const bool one_d = t->root->shape.type == RWA_TYPE_1D ||
                      t->root->shape.type == RWA_TYPE_1D_ARRAY;
   const VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &vu,
      .image = t->image,
      .viewType = one_d ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D,
      .format = t->format->vk,
      .subresourceRange = { format_aspects(t->root->format), t->base_level + level, 1,
                            t->base_layer + slice, 1 },
   };
   VkImageView view;
   VkResult r = device->vk.CreateImageView(device->dev, &vci, NULL, &view);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a view of level %u, slice %u of a %s image for a "
                         "pass to render into (%s)", level, slice, t->format->name,
          vk_Result_to_str(r));
      return VK_NULL_HANDLE;
   }
   struct rwa_att_view *grown = realloc(t->att, (t->att_count + 1) * sizeof(*t->att));
   if (!grown) {
      device->vk.DestroyImageView(device->dev, view, NULL);
      say(why, why_size, "out of memory keeping a view of a texture");
      return VK_NULL_HANDLE;
   }
   t->att = grown;
   t->att[t->att_count++] = (struct rwa_att_view){ level, slice, view };
   return view;
}

static void
texture_free(struct rwa_device *device, void *object)
{
   struct rwa_texture *texture = object;
   if (texture->store_view && texture->store_view != texture->view)
      device->vk.DestroyImageView(device->dev, texture->store_view, NULL);
   if (texture->base_view)
      device->vk.DestroyImageView(device->dev, texture->base_view, NULL);
   for (unsigned i = 0; i < texture->att_count; i++)
      device->vk.DestroyImageView(device->dev, texture->att[i].view, NULL);
   free(texture->att);
   texture->att = NULL;
   texture->att_count = 0;
   texture->store_view = texture->base_view = VK_NULL_HANDLE;
   if (texture->view)
      device->vk.DestroyImageView(device->dev, texture->view, NULL);
   if (texture->zs_view)
      device->vk.DestroyImageView(device->dev, texture->zs_view, NULL);
   texture->view = texture->zs_view = VK_NULL_HANDLE;
   /* [M5b: a view is itself and no image, and the image goes with the last texture over it:
    * either of the two may be destroyed first (air_metal.h).] */
   struct rwa_texture *root = texture->root;
   if (root != texture) {
      p_atomic_dec(&device->children);
      free(texture);
   }
   if (p_atomic_dec_zero(&root->refs)) {
      device->vk.DestroyImage(device->dev, root->image, NULL);
      device->vk.FreeMemory(device->dev, root->memory, NULL);
      p_atomic_dec(&device->children);
      free(root);
   }
}

void
rwa_texture_destroy(struct rwa_texture *texture)
{
   if (!texture)
      return;
   struct rwa_device *device = texture->device;
   p_atomic_dec(&rwa_live[RWA_LIVE_IMAGE]);
   argbuf_scrub(device, NULL, texture, NULL);
   /* [M5b: a view's work is named on the root (image_barrier), so that is what it waits for] */
   when_done(device, texture->root->use, texture_free, texture);
}

/* One barrier on one image, from the layout this library last left it in to `to`. The
 * source scope is every earlier write at every stage, because M3b does a handful of these
 * per frame and a narrow scope that is wrong is a picture that is sometimes right.
 *
 * The range is one level and one layer for a 3D texture too, and that is the whole of
 * it: a 3D image has one layer, and its depth is inside it (3D TEXTURES above).
 *
 * And it is every aspect the texture has (DEPTH AND STENCIL, ASPECTS): COLOR, or a depth
 * format's depth and stencil together, which is what Vulkan requires of a combined format
 * when separateDepthStencilLayouts is not enabled -- the two aspects share one layout.
 */
static void
image_barrier(struct rwa_texture *t, VkCommandBuffer cmd, VkImageLayout to,
              VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
   struct rwa_device *device = t->device;
   /* [M4-1, the cost part: the recording is the only one to call this, and it names the image
    * for what a destroy waits for, every use of a texture coming through here. And the layout is
    * read and written under the lock, because an upload on another thread reads it. The first
    * barrier of a recording on an image notes what it found (upload_image_begin).] */
   const struct rwa_fence *s = device->current;
   /* [M5b: all of it through the root: a view is another window on one image, one layout] */
   struct rwa_texture *root = t->root;
   pthread_mutex_lock(&device->lock);
   const VkImageLayout from = root->layout;
   if (s && root->use.recording != s->recording) {
      root->layout_at_start = from;
      named(&root->use, s);
   }
   root->layout = to;
   pthread_mutex_unlock(&device->lock);

   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t->image,
      /* [M5b: every level and every layer: TEXTURES, LEVELS AND SLICES, THE LAYOUT OF ONE IMAGE] */
      .subresourceRange = { root->aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
                            VK_REMAINING_ARRAY_LAYERS },
   };
   device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage, 0, 0,
                                 NULL, 0, NULL, 1, &b);
}

/*
 * The same barrier in the upload command buffer, under upload_lock, and the one thing it must get
 * right that image_barrier need not (UPLOADS, the second thing in the way). If the open recording
 * has named this image, its first barrier promised a layout it found the image in, and this upload
 * runs BEFORE that barrier: so the image is moved from that layout, and comes back to it
 * after the copy (upload_image_end), and `layout`, which is where the recording leaves it, stays
 * as it is. Otherwise this upload is simply what comes next, and `layout` follows it.
 * *back_to is where upload_image_end must take it, or equal to `to` for nowhere: which is also
 * the answer when that barrier found UNDEFINED, a layout nothing may be moved into and which
 * any image may be moved out of; the next upload then starts from where this one left it.
 */
static void
upload_image_begin(struct rwa_texture *t, VkCommandBuffer cmd, VkImageLayout to,
                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access,
                   VkImageLayout *back_to)
{
   struct rwa_device *device = t->device;
   struct rwa_texture *root = t->root;
   pthread_mutex_lock(&device->lock);
   const bool in_recording = device->current && root->use.recording == device->current->recording;
   const VkImageLayout from = in_recording ? root->layout_at_start : root->layout;
   const bool back = in_recording && from != VK_IMAGE_LAYOUT_UNDEFINED;
   if (!in_recording)
      root->layout = to;
   else if (!back)
      root->layout_at_start = to;
   root->use.upload = device->uploads;
   root->upload_id = 0;              /* (a write that wants to, says so after this) */
   pthread_mutex_unlock(&device->lock);
   *back_to = back ? from : to;

   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t->image,
      .subresourceRange = { root->aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
                            VK_REMAINING_ARRAY_LAYERS },
   };
   device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_stage, 0, 0,
                                 NULL, 0, NULL, 1, &b);
}

static void
upload_image_end(struct rwa_texture *t, VkCommandBuffer cmd, VkImageLayout to,
                 VkImageLayout back_to)
{
   if (back_to == to)
      return;
   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = to,
      .newLayout = back_to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = t->image,
      .subresourceRange = { t->root->aspects, 0, VK_REMAINING_MIP_LEVELS, 0,
                            VK_REMAINING_ARRAY_LAYERS },
   };
   t->device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/*
 * A read-back (rwa_texture_read, rwa_depth_read): `n` regions of `t` out into a piece of staging
 * of `total` bytes, every byte on the processor's side when this returns 0. The regions'
 * bufferOffset are from the start of that piece on the way in. UPLOADS says what the order is: this
 * thread's recording, then the writes already in the upload command buffer, then this copy, and
 * that upload to the queue now, with a fence of its own. Waited for without a lock. On 0 the caller
 * reads st->map and then calls staging_done; on -1 there is a sentence and nothing to give back.
 */
static int
read_back(struct rwa_texture *t, VkDeviceSize total, unsigned n, VkBufferImageCopy *region,
          const char *what, struct rwa_staging *st, char *why, size_t why_size)
{
   struct rwa_device *device = t->device;
   if (flush_mine(device, why, why_size) != 0 ||
       !staging_get(device, total, true, st, why, why_size))
      return -1;
   struct rwa_ucmd *u = upload_begin(device, why, why_size);
   if (!u) {
      pthread_mutex_unlock(&device->upload_lock);
      staging_done(device, st);
      return -1;
   }
   for (unsigned i = 0; i < n; i++)
      region[i].bufferOffset += st->offset;
   const VkImageLayout to = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkImageLayout back_to;
   upload_image_begin(t, u->cmd, to, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      &back_to);
   device->vk.CmdCopyImageToBuffer(u->cmd, t->image, to, st->buffer, n, region);
   upload_image_end(t, u->cmd, to, back_to);

   struct rwa_fence *s;
   int rc = upload_submit_locked(device, &s, why, why_size);
   pthread_mutex_unlock(&device->upload_lock);
   if (rc == 0) {
      rc = rwa_fence_wait(device, s, why, why_size);
      pair_release(device, s);
   }
   if (rc != 0) {
      if (why && why_size) {
         char inner[256];
         snprintf(inner, sizeof(inner), "%s", why);
         say(why, why_size, "the %s: %s", what, inner);
      }
      staging_done(device, st);
   }
   return rc;
}

/* air_metal.c's texture_box and caller_stride, for the one level and slice a texture of
 * this back end has: origin and size may both be left out, which is the whole texture,
 * and a stride of 0 is tightly packed. *row is the bytes of one row of the region.
 *
 * z is the region's depth in a 3D texture (o[2], s[2]) and is 0 and 1 in a 2D one, as
 * texture_box has it; the slice (`layer`) is Metal's arrayLength index and is 0 for both,
 * because Metal numbers a 3D texture's depth in the region and not in the slice.
 */
/* [M5b: for the levels and slices it has. origin[2] and size[2] are z on a 3D texture and nothing
 * else, and a 1D texture has one row, as texture_box has it; `layer` is Metal's slice, and on a
 * cube a face. *row is the bytes of one row of BLOCKS of the region and *rows how many such rows
 * there are, one block being one texel but for a compressed format, where the origin has to be on
 * a block and the size whole blocks or up to the edge of the level: Metal's rule for
 * -replaceRegion: and Vulkan's for a copy, and an offset into a block has no meaning in either. A
 * view's level and slice are its own, so a caller passes those to vk_subresource below.] */
static unsigned
level_width(const struct rwa_texture *t, unsigned level)
{
   return u_minify(t->shape.width, level);
}

static unsigned
level_height(const struct rwa_texture *t, unsigned level)
{
   return u_minify(t->shape.height, level);
}

static unsigned
level_depth(const struct rwa_texture *t, unsigned level)
{
   return t->plane_view ? 1 : u_minify(t->depth, level);
}

/* The image's subresource for a level and slice of this texture; a 3D image's one layer is 0. */
static VkImageSubresourceLayers
vk_subresource(const struct rwa_texture *t, VkImageAspectFlags aspect, unsigned level,
               unsigned layer)
{
   return (VkImageSubresourceLayers){ aspect, t->base_level + level, t->base_layer + layer, 1 };
}

static bool
texture_region(const struct rwa_texture *t, unsigned level, unsigned layer,
               const unsigned origin[3], const unsigned size[3], size_t stride,
               unsigned o[3], unsigned s[3], size_t *row, size_t *pitch, unsigned *rows,
               char *why, size_t why_size)
{
   if (level >= t->levels) {
      say(why, why_size, "level %u of a texture with %u", level, t->levels);
      return false;
   }
   if (layer >= t->layers) {
      say(why, why_size, "slice %u of a texture with %u", layer, t->layers);
      return false;
   }
   if (t->plane_view) {
      say(why, why_size, "a region of a view of one depth plane of a 3D texture: that view is "
                         "for a pass's clear, and a region of the plane is one of the texture");
      return false;
   }
   const unsigned w = level_width(t, level), h = level_height(t, level),
                  d = level_depth(t, level);
   o[0] = origin ? origin[0] : 0;
   o[1] = origin ? origin[1] : 0;
   o[2] = origin ? origin[2] : 0;
   s[0] = size ? size[0] : w;
   s[1] = size ? size[1] : h;
   s[2] = size ? size[2] : d;
   if (!s[0] || !s[1] || !s[2]) {
      say(why, why_size, "a region of %ux%ux%u texels has no texels in it", s[0], s[1], s[2]);
      return false;
   }
   /* each sum in 64 bits, so an origin near UINT_MAX cannot wrap round into range */
   if ((uint64_t)o[0] + s[0] > w || (uint64_t)o[1] + s[1] > h || (uint64_t)o[2] + s[2] > d) {
      say(why, why_size, "a region of %ux%ux%u texels at (%u, %u, %u) of a level that is "
                         "%ux%ux%u", s[0], s[1], s[2], o[0], o[1], o[2], w, h, d);
      return false;
   }
   const unsigned bw = fmt_block_w(t->format), bh = fmt_block_h(t->format);
   if (o[0] % bw || o[1] % bh || (s[0] % bw && o[0] + s[0] != w) ||
       (s[1] % bh && o[1] + s[1] != h)) {
      say(why, why_size, "a region of %ux%u texels at (%u, %u) of %s, whose blocks are %ux%u: it "
                         "starts on a block and is whole blocks or reaches the edge of the "
                         "level", s[0], s[1], o[0], o[1], t->format->name, bw, bh);
      return false;
   }
   *row = (size_t)DIV_ROUND_UP(s[0], bw) * t->format->bytes;
   *rows = DIV_ROUND_UP(s[1], bh);
   if (stride == 0) {
      *pitch = *row;
   } else if (stride < *row) {
      say(why, why_size, "%zu bytes per row for %u texels of %s, which need %zu", stride,
          s[0], t->format->name, *row);
      return false;
   } else {
      *pitch = stride;
   }
   return true;
}

int
rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
                 const unsigned origin[3], const unsigned size[3], void *bytes,
                 size_t stride, char *why, size_t why_size)
{
   if (!texture || !bytes) {
      say(why, why_size, "nowhere to read a texture into");
      return -1;
   }
   /* air_metal.c's refusal, sentence for sentence: a depth/stencil texel has an aspect
    * this call cannot name, and rwa_depth_read is the call that names one. */
   if (format_is_depth_stencil(texture->format)) {
      VkImageAspectFlags a = texture->aspects;
      say(why, why_size, "reading texels out of a %s texture: it has %s, and the one answer "
                         "this call can give would be whichever aspect the caller did not "
                         "mean. rwa_depth_read takes a float per texel, a byte per texel, "
                         "or both", texture->format->name,
          (a & VK_IMAGE_ASPECT_DEPTH_BIT) && (a & VK_IMAGE_ASPECT_STENCIL_BIT)
             ? "a depth and a stencil aspect"
             : (a & VK_IMAGE_ASPECT_DEPTH_BIT) ? "a depth aspect" : "a stencil aspect");
      return -1;
   }
   /* A multisample texture has no one texel to give back (MULTISAMPLE, THE PROCESSOR):
    * air_metal.c's refusal, sentence for sentence but for its last word -- this back end
    * does resolve, so the way to the processor is a resolve into a one-sample texture and
    * a read of that. Vulkan agrees: vkCmdCopyImageToBuffer takes only an image of one
    * sample (its valid usage: the source's samples must be VK_SAMPLE_COUNT_1_BIT). */
   if (texture->samples > 1) {
      say(why, why_size, "reading texels out of a %s of %u samples per texel: it holds %u "
                         "values per texel and the one answer this call can give would be "
                         "whichever sample the caller did not mean. Metal reads one with a "
                         "shader's read(coord, sample) or with a resolve into a one-sample "
                         "texture (rwa_resolve_texture), and neither is this call",
          texture->format->name, texture->samples, texture->samples);
      return -1;
   }
   struct rwa_device *device = texture->device;
   unsigned o[3], s[3], rows;
   size_t row, pitch;
   if (!texture_region(texture, level, layer, origin, size, stride, o, s, &row, &pitch, &rows, why,
                       why_size))
      return -1;

   /* A staging buffer for this read and no longer, tightly packed: the image's own
    * tiling is turnip's business, and vkCmdCopyImageToBuffer is what turns it into rows.
    * s[2] images of s[1] rows, which is one image of a 2D texture: bufferRowLength and
    * bufferImageHeight 0 below are exactly this packing.
    * [M4-1, the cost part: a piece of THE STAGING RING for this read and no longer, and the same
    * packing.]
    */
   struct rwa_staging staging;
   /* z in the offset and the extent, and layer 0 for a 3D image too (3D TEXTURES) */
   VkBufferImageCopy region = {
      .imageSubresource = vk_subresource(texture, VK_IMAGE_ASPECT_COLOR_BIT, level, layer),
      .imageOffset = { (int32_t)o[0], (int32_t)o[1], (int32_t)o[2] },
      .imageExtent = { s[0], s[1], s[2] },
   };

   /* Always submitted and waited for, deferring or not: the caller reads the bytes the
    * moment this returns, and they must include every draw recorded before it -- which
    * they do, because the copy went into the same command buffer after those draws, behind
    * a barrier on the image (and the host barrier record_submit ends every buffer with).
    * [M4-1, the cost part: because those draws reached the queue first, and every write before
    * this one is ahead of it in the upload command buffer it goes into (read_back).]
    */
   int rc = read_back(texture, row * rows * s[2], 1, &region, "read of a texture", &staging, why,
                      why_size);
   if (rc == 0) {
      /* row by row, because the caller's rows are as far apart as the caller said; and
       * image by image, the caller's images `pitch * s[1]` apart -- air_metal.c's
       * rwa_texture_read lays a 3D region out exactly so, (z * height + y) rows in */
      const uint8_t *src = staging.map;
      uint8_t *dst = bytes;
      for (unsigned z = 0; z < s[2]; z++)
         for (unsigned y = 0; y < rows; y++)
            memcpy(dst + ((size_t)z * rows + y) * pitch, src + ((size_t)z * rows + y) * row,
                   row);
      staging_done(device, &staging);
   }
   return rc;
}

int
rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
                  const unsigned origin[3], const unsigned size[3], const void *bytes,
                  size_t stride, char *why, size_t why_size)
{
   if (!texture || !bytes) {
      say(why, why_size, "nothing to write a texture from");
      return -1;
   }
   /* air_metal.c's refusal: nothing in this contract writes a depth texel from the
    * processor -- a depth attachment is filled by the pass's clear and by the draw. */
   if (format_is_depth_stencil(texture->format)) {
      say(why, why_size, "writing texels into a %s texture: a depth or stencil texel has "
                         "an aspect and this call has nowhere to name it. A depth "
                         "attachment is filled by the render pass's clearDepth and by what "
                         "the draw writes", texture->format->name);
      return -1;
   }
   /* And a multisample texture (MULTISAMPLE, THE PROCESSOR), for air_metal.c's harder
    * reason and in its words: Metal forbids it twice, and the GPU lays the samples of a
    * texel out in an order of its own -- Vulkan's vkCmdCopyBufferToImage takes only an
    * image of one sample, so here there is not even a wrong place for the bytes to go. */
   if (texture->samples > 1) {
      say(why, why_size, "writing texels into a %s of %u samples per texel: Metal writes "
                         "no multisample texture from the processor (one cannot be "
                         "MTLStorageModeShared on this device, and -replaceRegion: writes "
                         "only what the processor can see) and this GPU lays the samples of "
                         "a texel out in an order of its own, so these bytes would land in "
                         "no defined sample. A multisample texture is filled by rendering "
                         "into it", texture->format->name, texture->samples);
      return -1;
   }
   struct rwa_device *device = texture->device;
   unsigned o[3], s[3], rows;
   size_t row, pitch;
   if (!texture_region(texture, level, layer, origin, size, stride, o, s, &row, &pitch, &rows, why,
                       why_size))
      return -1;
   /* [M5b: the record of the image's use is the root's: see image_barrier] */
   struct rwa_texture *const root = texture->root;

   /* The caller's rows into a tightly packed staging buffer, and from there onto the GPU
    * in the order of everything recorded before: -replaceRegion: is the processor's write,
    * so it is waited for here and the texture holds the bytes when this returns -- the
    * same promise texture_subdata gives air_metal.c.
    *
    * A 3D region is s[2] images of the caller's, `pitch * s[1]` bytes apart: the slice
    * stride air_metal.c hands texture_subdata (row * box.height), because the contract
    * has one stride and that is the image a -replaceRegion: of bytesPerImage 0 or of the
    * obvious product hands over.
    *
    * [M4-1, the cost part: waited for here only when that is still the promise, which is
    * a device told to wait at every call, and a texture over an IOSurface, whose memory somebody
    * else reads. Otherwise the bytes are in THE STAGING RING when this returns, and the texture
    * holds them for anything that can look: UPLOADS, at upload_begin, has the order and the
    * threads.]
    */
   const bool at_once = !device->defer_waits || texture->surface_id != 0;

   /* What this thread already recorded with this texture in it comes first (UPLOADS) */
   pthread_mutex_lock(&device->lock);
   const bool mine_first = device->current && pthread_equal(device->recorder, pthread_self()) &&
                           root->use.recording == device->current->recording;
   pthread_mutex_unlock(&device->lock);
   if (mine_first && flush_mine(device, why, why_size) != 0)
      return -1;

   struct rwa_staging staging;
   if (!staging_get(device, row * rows * s[2], false, &staging, why, why_size))
      return -1;
   /* (upload_lock is held from here: the bytes go in in the order the copies do) */
   const uint8_t *src = bytes;
   uint8_t *dst = staging.map;
   for (unsigned z = 0; z < s[2]; z++)
      for (unsigned y = 0; y < rows; y++)
         memcpy(dst + ((size_t)z * rows + y) * row, src + ((size_t)z * rows + y) * pitch,
                row);
   /* [This memory is write-combined (HKGPU.cpp, and memory_type's HOST_COHERENT): what the bytes
    * went into is this processor's own write buffer. The kernel's barrier at a submission (submit,
    * hk_gpu_to_memory) used to be on the same thread right after this, and may now run on any
    * other core, so it is drained here; a mutex's release does not reach an observer that is not
    * a core. The same as Linux's wmb() before a doorbell.] */
#if defined(__aarch64__)
   __asm__ volatile("dsb st" : : : "memory");
#endif

   struct rwa_ucmd *u = upload_begin(device, why, why_size);
   if (!u) {
      pthread_mutex_unlock(&device->upload_lock);
      staging_done(device, &staging);
      return -1;
   }
   /* A buffer of its own goes when this upload's fence has signalled: the same list as a destroy */
   if (staging.own) {
      const struct rwa_use tag = { .upload = device->uploads };
      pthread_mutex_lock(&device->lock);
      struct rwa_after *a = after_add_locked(device, tag, raw_free, staging.own);
      pthread_mutex_unlock(&device->lock);
      if (!a) {
         say(why, why_size, "out of memory keeping a staging buffer for its upload");
         pthread_mutex_unlock(&device->upload_lock);
         staging_done(device, &staging);
         return -1;
      }
      staging.own = NULL;
   }
   /* From UNDEFINED the image's earlier contents may be discarded, which is right only for
    * an image nothing has written; every other layout keeps them, so a region written
    * row by row keeps the rows written before it.
    *
    * [M4-1, the cost part: and a second write into this upload, the row after the row before,
    * needs no barrier at all if it is still in TRANSFER_DST here and the two do not overlap: a
    * barrier is a wait on the GPU and a texture written row by row would pay one a row, which is
    * half of what this is for. Two that do overlap have an order to keep, so one memory barrier
    * between them. Only while the open recording has not named the image: otherwise each write is
    * the whole there and back (upload_image_begin).]
    */
   const VkImageLayout to = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   VkImageLayout back_to = to;
   pthread_mutex_lock(&device->lock);
   const bool in_recording = device->current &&
                             root->use.recording == device->current->recording;
   const bool again = !in_recording && root->upload_id == device->uploads &&
                      root->layout == to;
   pthread_mutex_unlock(&device->lock);
   /* [M5b: a box in one level's texels compared with another level's, which can only find an
    * overlap that is not one: a barrier more than was needed, and nothing wrong.] */
   unsigned *box = root->upload_box;
   bool overlap = false;
   if (again) {
      overlap = o[0] < box[3] && box[0] < o[0] + s[0] && o[1] < box[4] && box[1] < o[1] + s[1] &&
                o[2] < box[5] && box[2] < o[2] + s[2];
      if (overlap)
         barrier_all(device, u->cmd);
   } else {
      upload_image_begin(texture, u->cmd, to, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, &back_to);
   }
   if (again && !overlap) {
      box[0] = MIN2(box[0], o[0]);
      box[1] = MIN2(box[1], o[1]);
      box[2] = MIN2(box[2], o[2]);
      box[3] = MAX2(box[3], o[0] + s[0]);
      box[4] = MAX2(box[4], o[1] + s[1]);
      box[5] = MAX2(box[5], o[2] + s[2]);
   } else {
      box[0] = o[0];
      box[1] = o[1];
      box[2] = o[2];
      box[3] = o[0] + s[0];
      box[4] = o[1] + s[1];
      box[5] = o[2] + s[2];
   }
   if (!in_recording) {
      /* still there for the next write */
      pthread_mutex_lock(&device->lock);
      root->use.upload = device->uploads;
      root->upload_id = device->uploads;
      pthread_mutex_unlock(&device->lock);
   }
   const VkBufferImageCopy region = {
      .bufferOffset = staging.offset,
      .imageSubresource = vk_subresource(texture, VK_IMAGE_ASPECT_COLOR_BIT, level, layer),
      .imageOffset = { (int32_t)o[0], (int32_t)o[1], (int32_t)o[2] },
      .imageExtent = { s[0], s[1], s[2] },
   };
   device->vk.CmdCopyBufferToImage(u->cmd, staging.buffer, texture->image, to, 1, &region);
   upload_image_end(texture, u->cmd, to, back_to);

   if (!at_once) {
      pthread_mutex_unlock(&device->upload_lock);
      return 0;
   }
   struct rwa_fence *f;
   int rc = upload_submit_locked(device, &f, why, why_size);
   pthread_mutex_unlock(&device->upload_lock);
   if (rc == 0) {
      rc = rwa_fence_wait(device, f, why, why_size);
      pair_release(device, f);
   }
   if (rc != 0 && why && why_size) {
      char inner[256];
      snprintf(inner, sizeof(inner), "%s", why);
      say(why, why_size, "the write of a texture: %s", inner);
   }
   return rc;
}

/*
 * What a depth/stencil texture holds, one aspect at a time (air_metal.h), as air_metal.c's
 * rwa_depth_read gives it and with its refusals: neither aspect asked for, a colour
 * texture, an aspect the format has not got -- each by name, never a buffer of zeros.
 *
 * Each aspect asked for is copied out on the GPU as its own region (a buffer<->image copy
 * names one aspect, DEPTH AND STENCIL, ASPECTS) into one staging buffer, the stencil
 * after the depth at a multiple of four bytes (Vulkan's rule for a depth/stencil copy's
 * bufferOffset), and decoded on the processor by Mesa's own unpackers for the bytes
 * Vulkan wrote for that aspect (aspect_layout): Z16_UNORM, Z32_FLOAT or S8_UINT. So
 * Depth16Unorm's 65535 comes back as 1.0f here exactly as on the Gallium side, and a
 * Depth32Float_Stencil8 texture -- two planes in turnip, as in freedreno -- is read
 * without anything here knowing how either driver lays it out.
 *
 * Always submitted and waited for, as rwa_texture_read is: the caller reads the floats
 * the moment this returns.
 */
int
rwa_depth_read(struct rwa_texture *texture, unsigned level, unsigned layer,
               const unsigned origin[3], const unsigned size[3], float *depth,
               uint8_t *stencil, char *why, size_t why_size)
{
   if (!texture) {
      say(why, why_size, "a depth read needs a texture");
      return -1;
   }
   if (!depth && !stencil) {
      say(why, why_size, "a depth read that asks for neither aspect");
      return -1;
   }
   const struct rwa_vk_format *fmt = texture->format;
   if (!format_is_depth_stencil(fmt)) {
      say(why, why_size, "%s is a colour format and has no depth or stencil aspect; its "
                         "texels come back through rwa_texture_read", fmt->name);
      return -1;
   }
   if (depth && !(texture->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      say(why, why_size, "the depth of a %s texture, which has only a stencil aspect",
          fmt->name);
      return -1;
   }
   if (stencil && !(texture->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) {
      say(why, why_size, "the stencil of a %s texture, which has only a depth aspect",
          fmt->name);
      return -1;
   }
   /* A multisample depth texture (MULTISAMPLE, THE PROCESSOR): air_metal.c's refusal --
    * the aspect is named, the sample is not -- with this back end's way round it, a
    * SAMPLE_ZERO resolve into a one-sample depth texture, which this call then reads. */
   if (texture->samples > 1) {
      say(why, why_size, "the depth of a %s of %u samples per texel: this call gives one "
                         "value per texel and such a texture holds %u, with no argument "
                         "here to say which. A multisample depth attachment is read by a "
                         "shader, or resolved into a one-sample depth texture "
                         "(rwa_resolve_texture) and read there", fmt->name, texture->samples,
          texture->samples);
      return -1;
   }
   struct rwa_device *device = texture->device;
   unsigned o[3], s[3], rows;
   size_t row, pitch;
   if (!texture_region(texture, level, layer, origin, size, 0, o, s, &row, &pitch, &rows, why,
                       why_size))
      return -1;

   /* The regions, and where each lands in the staging buffer. */
   const VkImageAspectFlags asked[2] = {
      depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0, stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0,
   };
   size_t texels = (size_t)s[0] * s[1] * s[2];
   unsigned bytes[2] = { 0, 0 };
   enum pipe_format pf[2] = { PIPE_FORMAT_NONE, PIPE_FORMAT_NONE };
   VkDeviceSize at[2] = { 0, 0 }, total = 0;
   VkBufferImageCopy region[2];
   unsigned n = 0;
   for (unsigned i = 0; i < 2; i++) {
      if (!asked[i])
         continue;
      aspect_layout(fmt, asked[i], &bytes[i], &pf[i]);
      const struct util_format_unpack_description *u = util_format_unpack_description(pf[i]);
      if (!u || (i == 0 && !u->unpack_z_float) || (i == 1 && !u->unpack_s_8uint)) {
         /* [M5b: a view's own aspects may be fewer than the texture's: it reads what it says] */
         say(why, why_size, "Mesa has no unpacker for the %s aspect of %s, and this library "
                            "will not take the bits apart itself", i == 0 ? "depth" : "stencil",
             fmt->name);
         return -1;
      }
      at[i] = total;
      total = (total + texels * bytes[i] + 3) / 4 * 4;
      region[n++] = (VkBufferImageCopy){
         .bufferOffset = at[i],
         .imageSubresource = vk_subresource(texture, asked[i], level, layer),
         .imageOffset = { (int32_t)o[0], (int32_t)o[1], (int32_t)o[2] },
         .imageExtent = { s[0], s[1], s[2] },
      };
   }

   struct rwa_staging staging;
   int rc = read_back(texture, total, n, region, "read of a depth/stencil texture", &staging, why,
                      why_size);
   if (rc == 0) {
      /* row by row and slice by slice, as air_metal.c decodes them; the caller's memory is
       * tightly packed, one value per texel, which is what the contract promises */
      const uint8_t *map = staging.map;
      for (unsigned z = 0; z < s[2]; z++) {
         for (unsigned y = 0; y < s[1]; y++) {
            size_t t = ((size_t)z * s[1] + y) * s[0];
            if (depth)
               util_format_unpack_description(pf[0])->unpack_z_float(
                  depth + t, 0, map + at[0] + t * bytes[0], 0, s[0], 1);
            if (stencil)
               util_format_unpack_description(pf[1])->unpack_s_8uint(
                  stencil + t, 0, map + at[1] + t * bytes[1], 0, s[0], 1);
         }
      }
      staging_done(device, &staging);
   }
   return rc;
}

int
rwa_clear_color(struct rwa_texture *texture, const double rgba[4], char *why, size_t why_size)
{
   if (!texture || !rgba) {
      say(why, why_size, "a clear needs a texture and a colour");
      return -1;
   }
   /* vkCmdClearColorImage takes colour images only; a depth texture's clear is its
    * clearDepth and clearStencil, which reach this library as rwa_clear_depth_stencil. */
   if (format_is_depth_stencil(texture->format)) {
      say(why, why_size, "a colour clear of a %s texture: Vulkan clears a depth or stencil "
                         "image only through its aspects, and Metal's clearDepth and "
                         "clearStencil come here as rwa_clear_depth_stencil",
          texture->format->name);
      return -1;
   }
   struct rwa_device *device = texture->device;

   /* converted for an integer format as air_metal.c converts it, by a cast of each
    * channel, and taken as floats otherwise */
   VkClearColorValue color;
   for (int i = 0; i < 4; i++) {
      if (texture->format->kind == RWA_KIND_UINT)
         color.uint32[i] = (uint32_t)rgba[i];
      else if (texture->format->kind == RWA_KIND_SINT)
         color.int32[i] = (int32_t)rgba[i];
      else
         color.float32[i] = (float)rgba[i];
   }
   /* [M5b: of one depth plane of a 3D texture, which vkCmdClearColorImage cannot say (a 3D
    * image's range is its one layer, all of its depth): a pass of its own that clears and draws
    * nothing, through a view of the plane. That is what a pass's clear of a plane is in Metal.] */
   VkImageView plane_view = VK_NULL_HANDLE;
   if (texture->plane_view &&
       !(plane_view = att_view_for(texture, 0, 0, why, why_size)))
      return -1;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   if (plane_view) {
      image_barrier(texture, cmd->cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      VkRenderingAttachmentInfo att = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = plane_view,
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      att.clearValue.color = color;
      const VkRenderingInfo ri = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = { { 0, 0 }, { texture->shape.width, texture->shape.height } },
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &att,
      };
      device->CmdBeginRendering(cmd->cmd, &ri);
      device->CmdEndRendering(cmd->cmd);
      return recorded(device, "clear of a depth plane", why, why_size);
   }
   image_barrier(texture, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
   /* the one layer, which for a 3D texture is every depth of it: the whole texture, as
    * air_metal.c's whole_surface clears every slice of one (last_layer = depth0 - 1) */
   /* [M5b: of its base level, every slice it has: that is whole_surface exactly, and a view of
    * one level and one slice is how a pass's clear of another one is said (air_metal.h)] */
   const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, texture->base_level, 1,
                                           texture->base_layer, texture->layers };
   device->vk.CmdClearColorImage(cmd->cmd, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &color, 1, &range);
   return recorded(device, "clear", why, why_size);
}

/* Metal's clearDepth as Vulkan takes it, or a sentence: Vulkan's depth clear value lies in
 * [0, 1] unless VK_EXT_depth_range_unrestricted is enabled, which this device is not
 * asked for, and a value outside would be undefined rather than clamped. Metal's own
 * default is 1.0 and its depths lie in the same range. NaN is outside it too.
 */
static bool
clear_depth_fits(float d, char *why, size_t why_size)
{
   if (d >= 0.0f && d <= 1.0f)
      return true;
   say(why, why_size, "a clearDepth of %g: Vulkan clears a depth to a value in [0, 1] (no "
                      "VK_EXT_depth_range_unrestricted here), and so does every depth Metal "
                      "tests", (double)d);
   return false;
}

/*
 * A render pass's clearDepth and clearStencil when the pass draws nothing (air_metal.h):
 * either aspect, NULL for one that keeps its contents -- and, as in air_metal.c, only the
 * aspects the texture HAS are cleared, so asking for the stencil of a Depth32Float texture
 * clears its depth (or nothing) and is not an error, and a texture that has neither of the
 * asked aspects is left alone and answered 0. The stencil value is its low eight bits, the
 * stencil buffer's width, as at the draw.
 *
 * vkCmdClearDepthStencilImage over the one level and layer, in TRANSFER_DST_OPTIMAL; the
 * barrier moves every aspect (image_barrier) and the clear names only the ones asked for.
 */
int
rwa_clear_depth_stencil(struct rwa_texture *texture, const float *depth,
                        const unsigned *stencil, char *why, size_t why_size)
{
   if (!texture) {
      say(why, why_size, "a depth or stencil clear needs a texture");
      return -1;
   }
   VkImageAspectFlags clear =
      (depth && (texture->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
      (stencil && (texture->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                                   : 0);
   if (!clear)
      return 0;
   if ((clear & VK_IMAGE_ASPECT_DEPTH_BIT) && !clear_depth_fits(*depth, why, why_size))
      return -1;
   struct rwa_device *device = texture->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   image_barrier(texture, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
   const VkClearDepthStencilValue value = {
      .depth = depth ? *depth : 0.0f,
      .stencil = stencil ? (*stencil & 0xff) : 0,
   };
   /* [M5b: the base level and every slice of it, as a colour clear] */
   const VkImageSubresourceRange range = { clear, texture->base_level, 1, texture->base_layer,
                                           texture->layers };
   device->vk.CmdClearDepthStencilImage(cmd->cmd, texture->image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                        &range);
   return recorded(device, "depth/stencil clear", why, why_size);
}

/*
 * The multisample resolve at the end of a pass (MULTISAMPLE, THE RESOLVE): the samples of
 * `src` into the one-sample `dst`, which is what hkmetal.m records at -endEncoding for an
 * attachment whose store action is MTLStoreActionMultisampleResolve or
 * StoreAndMultisampleResolve and which has a resolveTexture. Level 0, slice 0, the whole of
 * `src`, at the origin of `dst`.
 *
 * Checked first, each refusal naming what was asked for: the sample counts (air_metal.c's
 * sentence for a destination of more than one; and a source of one, which Metal never
 * resolves -- a resolve is a store action OF a multisample attachment -- and Vulkan
 * cannot: a resolve attachment's image view has to be multisampled), the format (Metal
 * requires the resolveTexture's to be the attachment's, and Vulkan's resolveImageView must
 * have the imageView's format), a destination that is not a MTLTextureType2D (Metal's
 * resolveSlice and resolveDepthPlane would say where in an array or a 3D texture, and this
 * contract carries neither), and the size (air_metal.c's: the source no larger than the
 * destination, which is Vulkan's rule for the render area too).
 *
 * Then one of three recordings, each in order with everything recorded before it:
 *
 *   colour, both render targets   a pass of its own that draws nothing: src loaded and
 *                                 stored, with dst as its resolveImageView and the mode
 *                                 Vulkan requires of the format -- AVERAGE for a floating-
 *                                 point or normalised one, SAMPLE_ZERO for an integer one.
 *                                 The resolve is turnip's at the end of that pass, in GMEM or
 *                                 in sysmem, exactly as it is at the end of a pass that drew
 *   colour, otherwise             vkCmdResolveImage, the same resolve as a transfer: Vulkan
 *                                 takes only a COLOR_ATTACHMENT image as a resolve attachment
 *                                 and a texture made without MTLTextureUsageRenderTarget is
 *                                 not one, while a transfer asks only that the destination's
 *                                 FORMAT can be rendered into (COLOR_ATTACHMENT). air_metal.c
 *                                 takes such a destination (Maps resolves into one), so this
 *                                 back end does too
 *   depth and stencil             the same empty pass with pDepthAttachment (and
 *                                 pStencilAttachment for Depth32Float_Stencil8) resolved
 *                                 SAMPLE_ZERO, the only mode turnip has for either aspect; both
 *                                 textures must be render targets (they have the every-aspect
 *                                 view only then), and vkCmdResolveImage resolves no depth
 *
 * hkmetal.m carries a colour attachment's resolveTexture today and not the depth
 * attachment's (MTLRenderPassDepthAttachmentDescriptor.resolveTexture is not read there),
 * so the depth form is reached through this contract alone for now.
 */
int
rwa_resolve_texture(struct rwa_texture *src, struct rwa_texture *dst, char *why,
                    size_t why_size)
{
   if (!src || !dst) {
      say(why, why_size, "a resolve needs both textures");
      return -1;
   }
   if (src->device != dst->device) {
      say(why, why_size, "a resolve between textures of two devices");
      return -1;
   }
   if (dst->samples > 1) {
      say(why, why_size, "a resolve into a %s texture of %u samples: Metal resolves into one "
                         "sample", dst->format->name, dst->samples);
      return -1;
   }
   if (src->samples < 2) {
      say(why, why_size, "a resolve from a %s texture of one sample: Metal resolves a "
                         "multisample attachment, a resolve being one of its store actions, "
                         "and Vulkan resolves only from a multisample image",
          src->format->name);
      return -1;
   }
   if (src->format != dst->format) {
      say(why, why_size, "a resolve from %s into %s: Metal's resolveTexture has its "
                         "attachment's pixel format, and Vulkan resolves only into an image "
                         "of the same format", src->format->name, dst->format->name);
      return -1;
   }
   /* [M5b: "a resolve into a %s: resolving into a slice of an array or a plane of a 3D texture
    * (resolveSlice, resolveDepthPlane) is not in the Vulkan back end yet (M3c)" stood here. Those
    * are a view now, of the base level and one slice or plane, which is how hkmetal.m says
    * resolveLevel and resolveSlice; so what is refused is more than one slice to resolve into, or
    * out of, where nothing says which.] */
   if (dst->layers != 1 || src->layers != 1 || dst->shape.type == RWA_TYPE_3D) {
      say(why, why_size, "a resolve from %u slices of a %s into %u of a %s: a resolve is of one "
                         "slice into one (a view of it says which, and of a 3D texture a view "
                         "of one plane)", src->layers, texture_type_name(src->shape.type),
          dst->layers, texture_type_name(dst->shape.type));
      return -1;
   }
   if (src->shape.width > dst->shape.width || src->shape.height > dst->shape.height) {
      say(why, why_size, "a resolve of %ux%u into %ux%u", src->shape.width, src->shape.height,
          dst->shape.width, dst->shape.height);
      return -1;
   }
   struct rwa_device *device = src->device;
   const bool zs = format_is_depth_stencil(src->format);
   const bool pass = src->render_target && dst->render_target;
   if (zs) {
      if (!src->render_target || !dst->render_target) {
         say(why, why_size, "a resolve of a %s texture %s one made without "
                            "MTLTextureUsageRenderTarget: a depth or stencil resolve is the "
                            "end of a pass in Vulkan (vkCmdResolveImage resolves colour "
                            "alone), and a texture that is no attachment cannot be in one, so "
                            "this is not in the Vulkan back end yet (M3c)", src->format->name,
             !src->render_target ? "from" : "into");
         return -1;
      }
      if (((src->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
           !(device->depth_resolve_modes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) ||
          ((src->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
           !(device->stencil_resolve_modes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))) {
         say(why, why_size, "a resolve of %s on %s, which resolves depth with modes %#x and "
                            "stencil with %#x: this back end resolves both SAMPLE_ZERO, and a "
                            "device without it is not in the Vulkan back end yet (M3c)",
             src->format->name, device->name, device->depth_resolve_modes,
             device->stencil_resolve_modes);
         return -1;
      }
   } else if (!pass) {
      VkFormatProperties fp;
      device->vk.GetPhysicalDeviceFormatProperties(device->physical, dst->format->vk, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         say(why, why_size, "%s cannot render into %s, which a resolve writes", device->name,
             dst->format->name);
         return -1;
      }
   }

   /* [M5b: each side's one level and one slice, in the views a pass takes (ITS VIEWS)] */
   VkImageView src_view = VK_NULL_HANDLE, dst_view = VK_NULL_HANDLE;
   if ((zs || pass) && (!(src_view = att_view_for(src, 0, 0, why, why_size)) ||
                        !(dst_view = att_view_for(dst, 0, 0, why, why_size))))
      return -1;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   const VkExtent2D extent = { src->shape.width, src->shape.height };

   if (!zs && !pass) {
      image_barrier(src, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      image_barrier(dst, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      const VkImageResolve region = {
         .srcSubresource = vk_subresource(src, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0),
         .dstSubresource = vk_subresource(dst, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0),
         .extent = { extent.width, extent.height, 1 },
      };
      device->vk.CmdResolveImage(cmd->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      return recorded(device, "multisample resolve", why, why_size);
   }

   /* The empty pass. The resolve's writes are colour-attachment writes at the colour-
    * attachment-output stage for a depth resolve too (Vulkan, "End-of-subpass multisample
    * resolves are treated as color attachment writes"), so the destination's barrier
    * names that stage and that access whichever its format is. */
   VkRenderingAttachmentInfo catt = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
   VkRenderingAttachmentInfo zatt = catt, satt = catt;
   if (!zs) {
      image_barrier(src, cmd->cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      image_barrier(dst, cmd->cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      catt.imageView = src_view;
      catt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      catt.resolveMode = src->format->kind == RWA_KIND_FLOAT ? VK_RESOLVE_MODE_AVERAGE_BIT
                                                             : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      catt.resolveImageView = dst_view;
      catt.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      catt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   } else {
      image_barrier(src, cmd->cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      image_barrier(dst, cmd->cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      zatt.imageView = satt.imageView = src_view;
      zatt.imageLayout = satt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      zatt.resolveMode = satt.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      zatt.resolveImageView = satt.resolveImageView = dst_view;
      zatt.resolveImageLayout = satt.resolveImageLayout =
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      zatt.loadOp = satt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      zatt.storeOp = satt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   }
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { { 0, 0 }, extent },
      .layerCount = 1,
      .colorAttachmentCount = zs ? 0 : 1,
      .pColorAttachments = zs ? NULL : &catt,
      .pDepthAttachment = zs && (src->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &zatt : NULL,
      .pStencilAttachment = zs && (src->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &satt : NULL,
   };
   device->CmdBeginRendering(cmd->cmd, &ri);
   device->CmdEndRendering(cmd->cmd);
   return recorded(device, "multisample resolve", why, why_size);
}

/* ---------------------------------------------------------------------------------- */
/* The blit encoder                                                                   */
/* ---------------------------------------------------------------------------------- */

/* Every blit is recorded behind barrier_all, in the order the command buffer gives them,
 * and moves the images it touches into TRANSFER layouts with image_barrier: a texture a
 * blit writes and a pass then samples is moved again by that pass, so nothing here relies
 * on the queue's order for what one command sees of another.
 */

int
rwa_copy_buffer(struct rwa_buffer *src, size_t src_offset, struct rwa_buffer *dst,
                size_t dst_offset, size_t size, char *why, size_t why_size)
{
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "a copy between buffers needs two buffers of one device");
      return -1;
   }
   if (!buffer_range(src, src_offset, size, "the copy's source", why, why_size) ||
       !buffer_range(dst, dst_offset, size, "the copy's destination", why, why_size))
      return -1;
   if (size == 0)
      return 0;
   if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
      say(why, why_size, "a copy of %zu bytes from offset %zu to offset %zu of one buffer: "
                         "the two ranges overlap, which Metal leaves undefined and Vulkan "
                         "forbids", size, src_offset, dst_offset);
      return -1;
   }
   struct rwa_device *device = src->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   named(&src->use, cmd);
   named(&dst->use, cmd);
   barrier_all(device, cmd->cmd);
   const VkBufferCopy region = { src_offset, dst_offset, size };
   device->vk.CmdCopyBuffer(cmd->cmd, src->raw.buffer, dst->raw.buffer, 1, &region);
   return recorded(device, "copy between buffers", why, why_size);
}

int
rwa_fill_buffer(struct rwa_buffer *buffer, size_t offset, size_t size, uint8_t value,
                char *why, size_t why_size)
{
   if (!buffer) {
      say(why, why_size, "a fill needs a buffer");
      return -1;
   }
   if (!buffer_range(buffer, offset, size, "the fill", why, why_size))
      return -1;
   if (size == 0)
      return 0;
   /* vkCmdFillBuffer writes 32-bit words at offsets that are multiples of 4, and a byte
    * fill is one when the byte is repeated four times; a range that does not start and
    * end on a word is refused rather than filled a word too far.
    */
   if (offset % 4 || size % 4) {
      say(why, why_size, "a fill of %zu bytes at offset %zu: this GPU fills whole 32-bit "
                         "words, and a range not on word boundaries is not in the Vulkan "
                         "back end yet (M3b)", size, offset);
      return -1;
   }
   struct rwa_device *device = buffer->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   named(&buffer->use, cmd);
   barrier_all(device, cmd->cmd);
   device->vk.CmdFillBuffer(cmd->cmd, buffer->raw.buffer, offset, size,
                            0x01010101u * (uint32_t)value);
   return recorded(device, "fill", why, why_size);
}

int
rwa_copy_texture(struct rwa_texture *src, unsigned src_layer, unsigned src_level,
                 const unsigned src_origin[3], const unsigned size[3],
                 struct rwa_texture *dst, unsigned dst_layer, unsigned dst_level,
                 const unsigned dst_origin[3], char *why, size_t why_size)
{
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "a copy between textures needs two textures of one device");
      return -1;
   }
   unsigned so[3], ss[3], dso[3], dss[3], rows;
   size_t row, pitch;
   if (!texture_region(src, src_level, src_layer, src_origin, size, 0, so, ss, &row, &pitch,
                       &rows, why, why_size) ||
       !texture_region(dst, dst_level, dst_layer, dst_origin, ss, 0, dso, dss, &row, &pitch,
                       &rows, why, why_size))
      return -1;
   /* Metal's own rule for this copy, and Vulkan's "size-compatible" for these formats */
   if (src->format->bytes != dst->format->bytes) {
      say(why, why_size, "a copy from %s to %s: Metal copies between formats of one texel "
                         "size, and these are %u and %u bytes", src->format->name,
          dst->format->name, src->format->bytes, dst->format->bytes);
      return -1;
   }
   /* [M5b: and a compressed format to one of another block (Vulkan's rule is size-compatible, and
    * the extent would then be counted in one side's texels and not the other's). Two of one
    * block, or one to itself, copy as they are.] */
   if (fmt_block_w(src->format) != fmt_block_w(dst->format) ||
       fmt_block_h(src->format) != fmt_block_h(dst->format)) {
      say(why, why_size, "a copy from %s to %s: blocks of %ux%u and %ux%u texels, and one region "
                         "cannot name both (M5b)", src->format->name, dst->format->name,
          fmt_block_w(src->format), fmt_block_h(src->format), fmt_block_w(dst->format),
          fmt_block_h(dst->format));
      return -1;
   }
   /* Two sample counts (MULTISAMPLE): air_metal.c's refusal and its sentence, and Vulkan's
    * rule as well (vkCmdCopyImage: the same sample count on both sides). Two
    * multisample textures of one count copy sample for sample, as on Gallium. */
   if (src->samples != dst->samples) {
      say(why, why_size, "a copy between textures of %u and %u samples: that is a resolve, "
                         "not a copy", src->samples, dst->samples);
      return -1;
   }
   /* And a depth or stencil format (M3c-2) is size-compatible with itself alone in
    * Vulkan: a copy between Depth32Float and R32Float, which Metal's rule and Gallium's
    * resource_copy_region both allow by texel size, is a copy Vulkan makes only with
    * VK_KHR_maintenance8, which this device is not asked for. Between two textures of
    * one depth format the copy is every aspect at once (DEPTH AND STENCIL, ASPECTS).
    */
   if ((format_is_depth_stencil(src->format) || format_is_depth_stencil(dst->format)) &&
       src->format->vk != dst->format->vk) {
      say(why, why_size, "a copy from %s to %s: Vulkan copies a depth or stencil texture "
                         "only into one of the same format (a copy between a depth format "
                         "and another needs VK_KHR_maintenance8), so this copy, which the "
                         "Gallium back end makes by texel size, is not in the Vulkan back "
                         "end yet (M3c)", src->format->name, dst->format->name);
      return -1;
   }
   struct rwa_device *device = src->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   /* one texture to itself (two regions of it) is copied in GENERAL, the one layout an
    * image may be in as source and destination at once */
   /* [M5b: "one texture" is one image: a level into another, a view into its parent] */
   const bool one_image = src->root == dst->root;
   VkImageLayout sl = one_image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkImageLayout dl = one_image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   image_barrier(src, cmd->cmd, sl, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT | (one_image ? VK_ACCESS_TRANSFER_WRITE_BIT : 0));
   if (!one_image)
      image_barrier(dst, cmd->cmd, dl, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
   /* z and depth on both sides (3D TEXTURES). texture_region gave a 2D side depth 1 and
    * z 0, so between two 2D textures this is the copy it always was, and between a 2D
    * texture and a 3D one it is one slice of the 3D one: Vulkan 1.1 (maintenance1)
    * allows that copy when extent.depth is the 2D side's layer count, which is 1 here.
    * turnip takes a 3D side's z as its start layer (tu_copy_image_to_image).
    *
    * The aspect is the textures' own (one format on both sides when it is a depth one,
    * above): COLOR, or DEPTH and/or STENCIL, both together for Depth32Float_Stencil8 --
    * which vkCmdCopyImage allows in one region, and turnip copies plane by plane.
    */
   const VkImageCopy region = {
      .srcSubresource = vk_subresource(src, src->aspects, src_level, src_layer),
      .srcOffset = { (int32_t)so[0], (int32_t)so[1], (int32_t)so[2] },
      .dstSubresource = vk_subresource(dst, dst->aspects, dst_level, dst_layer),
      .dstOffset = { (int32_t)dso[0], (int32_t)dso[1], (int32_t)dso[2] },
      .extent = { ss[0], ss[1], ss[2] },
   };
   device->vk.CmdCopyImage(cmd->cmd, src->image, sl, dst->image, dl, 1, &region);
   return recorded(device, "copy between textures", why, why_size);
}

/* The buffer side of a buffer<->texture copy, as Metal gives it (bytes per row, an
 * offset) in Vulkan's terms (texels per row): Metal requires the row to be a whole number
 * of texels, and Vulkan's copies want the offset on a texel and on a word as well -- an
 * offset that is not is refused by name, because the copy would start somewhere else.
 *
 * And, for a region more than one image deep (a 3D texture's, M3c), Metal's bytes per
 * image in Vulkan's terms: bufferImageHeight, which counts ROWS, so an image stride that
 * is not a whole number of rows cannot be said and is refused by name rather than
 * rounded. 0 is packed (Metal's 0, and Gallium's buffer_layout reading of it: rows
 * times height). A region one image deep keeps *image_rows 0 and does not look at
 * image_bytes at all, which is what the 2D copies always did: Metal ignores the field
 * for them.
 */
/* air_metal.c's plain_texture, sentence for sentence: a depth or stencil texture is
 * refused by both buffer<->texture copies (M3c-2). Vulkan could copy one aspect of one,
 * but the contract's copy names no aspect -- Metal's MTLBlitOptionDepthFromDepthStencil
 * has nowhere to arrive -- and Gallium declines the same copy, so the two back ends agree.
 */
/* [M3c-3: and a multisample texture, as air_metal.c's plain_texture refuses one, in its
 * words: Vulkan copies between a buffer and an image of one sample only
 * (vkCmdCopyBufferToImage and vkCmdCopyImageToBuffer), and Metal's -copyFromBuffer:toTexture:
 * takes none either.] */
static bool
plain_texture(const struct rwa_texture *t, const char *what, char *why, size_t why_size)
{
   if (format_is_depth_stencil(t->format) || t->samples > 1) {
      say(why, why_size, "a copy between a buffer and the %s %s texture%s: its texels are "
                         "not bytes in one order a buffer can hold", what, t->format->name,
          t->samples > 1 ? " (multisample)" : "");
      return false;
   }
   return true;
}

static bool
buffer_side(const struct rwa_buffer *b, size_t offset, size_t row_bytes, size_t image_bytes,
            const struct rwa_texture *t, const unsigned s[3], uint32_t *row_texels,
            uint32_t *image_rows, char *why, size_t why_size)
{
   /* [M5b: of blocks, where the format has them: a row here is a row of blocks, and Vulkan's two
    * lengths are in texels and rows of texels whatever the block (bufferRowLength,
    * bufferImageHeight), so they come out multiplied by it. Otherwise a block is a texel.] */
   unsigned texel = t->format->bytes;
   const unsigned bw = fmt_block_w(t->format), bh = fmt_block_h(t->format);
   size_t row = (size_t)DIV_ROUND_UP(s[0], bw) * texel;
   const unsigned block_rows = DIV_ROUND_UP(s[1], bh);
   if (row_bytes == 0)
      row_bytes = row;
   if (row_bytes % texel || row_bytes < row) {
      say(why, why_size, "%zu bytes per row for %u texels of %s: Metal's bytesPerRow is a "
                         "whole number of texels, at least the region's width", row_bytes,
          s[0], t->format->name);
      return false;
   }
   if (offset % texel || offset % 4) {
      say(why, why_size, "a buffer offset of %zu for %s texels: this GPU copies from an "
                         "offset on a texel and on a 32-bit word, and staging an unaligned "
                         "copy is not in the Vulkan back end yet (M3b)", offset,
          t->format->name);
      return false;
   }
   *image_rows = 0;
   size_t image = row_bytes * block_rows;
   if (s[2] > 1) {
      if (image_bytes == 0)
         image_bytes = image;
      if (image_bytes < image) {
         say(why, why_size, "%zu bytes per image for %u rows of %zu bytes", image_bytes,
             s[1], row_bytes);
         return false;
      }
      if (image_bytes % row_bytes) {
         say(why, why_size, "%zu bytes per image with %zu bytes per row: Vulkan steps from "
                            "one image of a copy to the next by whole rows "
                            "(bufferImageHeight), and an image stride that is not a whole "
                            "number of rows is not in the Vulkan back end yet (M3c)",
             image_bytes, row_bytes);
         return false;
      }
      *image_rows = (uint32_t)(image_bytes / row_bytes) * bh;
   }
   size_t need = (s[2] > 1 ? image_bytes * (s[2] - 1) : 0) + row_bytes * (block_rows - 1) + row;
   if (!buffer_range(b, offset, need, "the copy's buffer side", why, why_size))
      return false;
   *row_texels = (uint32_t)(row_bytes / texel) * bw;
   return true;
}

int
rwa_copy_buffer_to_texture(struct rwa_buffer *src, size_t src_offset, size_t row_bytes,
                           size_t image_bytes, const unsigned size[3],
                           struct rwa_texture *dst, unsigned dst_layer, unsigned dst_level,
                           const unsigned dst_origin[3], char *why, size_t why_size)
{
   /* image_bytes steps from one image of a 3D region to the next (buffer_side); a 2D
    * region is one image and does not use it */
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "a copy into a texture needs a buffer and a texture of one device");
      return -1;
   }
   if (!plain_texture(dst, "destination", why, why_size))
      return -1;
   unsigned o[3], s[3], rows;
   size_t row, pitch;
   uint32_t row_texels, image_rows;
   if (!texture_region(dst, dst_level, dst_layer, dst_origin, size, 0, o, s, &row, &pitch,
                       &rows, why, why_size) ||
       !buffer_side(src, src_offset, row_bytes, image_bytes, dst, s, &row_texels,
                    &image_rows, why, why_size))
      return -1;
   struct rwa_device *device = src->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   named(&src->use, cmd);
   barrier_all(device, cmd->cmd);
   image_barrier(dst, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
   const VkBufferImageCopy region = {
      .bufferOffset = src_offset,
      .bufferRowLength = row_texels,
      .bufferImageHeight = image_rows,
      .imageSubresource = vk_subresource(dst, VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer),
      .imageOffset = { (int32_t)o[0], (int32_t)o[1], (int32_t)o[2] },
      .imageExtent = { s[0], s[1], s[2] },
   };
   device->vk.CmdCopyBufferToImage(cmd->cmd, src->raw.buffer, dst->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   return recorded(device, "copy into a texture", why, why_size);
}

int
rwa_copy_texture_to_buffer(struct rwa_texture *src, unsigned src_layer, unsigned src_level,
                           const unsigned src_origin[3], const unsigned size[3],
                           struct rwa_buffer *dst, size_t dst_offset, size_t row_bytes,
                           size_t image_bytes, char *why, size_t why_size)
{
   if (!src || !dst || src->device != dst->device) {
      say(why, why_size, "a copy out of a texture needs a texture and a buffer of one "
                         "device");
      return -1;
   }
   if (!plain_texture(src, "source", why, why_size))
      return -1;
   unsigned o[3], s[3], rows;
   size_t row, pitch;
   uint32_t row_texels, image_rows;
   if (!texture_region(src, src_level, src_layer, src_origin, size, 0, o, s, &row, &pitch,
                       &rows, why, why_size) ||
       !buffer_side(dst, dst_offset, row_bytes, image_bytes, src, s, &row_texels,
                    &image_rows, why, why_size))
      return -1;
   struct rwa_device *device = src->device;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   named(&dst->use, cmd);
   barrier_all(device, cmd->cmd);
   image_barrier(src, cmd->cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
   const VkBufferImageCopy region = {
      .bufferOffset = dst_offset,
      .bufferRowLength = row_texels,
      .bufferImageHeight = image_rows,
      .imageSubresource = vk_subresource(src, VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer),
      .imageOffset = { (int32_t)o[0], (int32_t)o[1], (int32_t)o[2] },
      .imageExtent = { s[0], s[1], s[2] },
   };
   device->vk.CmdCopyImageToBuffer(cmd->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   dst->raw.buffer, 1, &region);
   return recorded(device, "copy out of a texture", why, why_size);
}

int
rwa_generate_mipmaps(struct rwa_texture *texture, char *why, size_t why_size)
{
   if (!texture) {
      say(why, why_size, "mipmaps need a texture");
      return -1;
   }
   /* Every texture of this back end has one level (rwa_texture_create refuses a chain),
    * and generating the levels below level 0 of a one-level texture generates nothing:
    * that is the whole of the call here, and it is what util_gen_mipmap does with
    * last_level 0 on the Gallium side.
    */
   /* [M5b: a chain is a texture of this back end now (TEXTURES, LEVELS AND SLICES, THE MIPMAPS).
    * That paragraph is still the case of one level. Otherwise each level from the one above it,
    * in order, every slice, and a 3D texture's depth halving with it: util_gen_mipmap's result,
    * by Vulkan's blit, which is a linear filter over each level's own texels. A view makes the
    * levels of its window, from the base of it, as Metal's does.] */
   if (texture->levels == 1)
      return 0;
   struct rwa_device *device = texture->device;
   const struct rwa_vk_format *f = texture->format;
   VkFormatProperties fp;
   device->vk.GetPhysicalDeviceFormatProperties(device->physical, f->vk, &fp);
   const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                     VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
   if ((fp.optimalTilingFeatures & need) != need) {
      say(why, why_size, "generating the levels of a %s texture: Vulkan makes them with a linear "
                         "blit, which needs a format that is filtered and rendered into, and "
                         "Metal's rule for -generateMipmapsForTexture: is the same two words; "
                         "turnip gives this one features %#x (M5b)", f->name,
          (unsigned)fp.optimalTilingFeatures);
      return -1;
   }
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      return -1;
   /* One layout for the two sides, as for a copy within one image (THE LAYOUT OF ONE IMAGE), and
    * a barrier between levels: each one is read by the next. */
   image_barrier(texture, cmd->cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
   const bool volume = texture->shape.type == RWA_TYPE_3D;
   for (unsigned l = 1; l < texture->levels; l++) {
      const VkImageBlit blit = {
         .srcSubresource = { texture->aspects, texture->base_level + l - 1, texture->base_layer,
                             texture->layers },
         .srcOffsets = { { 0, 0, 0 },
                         { (int32_t)level_width(texture, l - 1),
                           (int32_t)level_height(texture, l - 1),
                           volume ? (int32_t)level_depth(texture, l - 1) : 1 } },
         .dstSubresource = { texture->aspects, texture->base_level + l, texture->base_layer,
                             texture->layers },
         .dstOffsets = { { 0, 0, 0 },
                         { (int32_t)level_width(texture, l), (int32_t)level_height(texture, l),
                           volume ? (int32_t)level_depth(texture, l) : 1 } },
      };
      device->vk.CmdBlitImage(cmd->cmd, texture->image, VK_IMAGE_LAYOUT_GENERAL, texture->image,
                              VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
      barrier_all(device, cmd->cmd);
   }
   return recorded(device, "generating levels", why, why_size);
}

/* ---------------------------------------------------------------------------------- */
/* Samplers                                                                           */
/* ---------------------------------------------------------------------------------- */

enum {   /* MTLSamplerAddressMode, MTLSamplerMinMagFilter, MTLSamplerMipFilter */
   RWA_ADDRESS_CLAMP_TO_EDGE = 0, RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE = 1,
   RWA_ADDRESS_REPEAT = 2, RWA_ADDRESS_MIRROR_REPEAT = 3,
   RWA_ADDRESS_CLAMP_TO_ZERO = 4, RWA_ADDRESS_CLAMP_TO_BORDER = 5,
   RWA_FILTER_NEAREST = 0, RWA_FILTER_LINEAR = 1,
   RWA_MIP_NOT_MIPMAPPED = 0, RWA_MIP_NEAREST = 1, RWA_MIP_LINEAR = 2,
};

/*
 * MTLSamplerDescriptor in Vulkan's terms, by the same tables air_metal.c maps it to
 * Gallium with, and with the same refusals. Three places where Vulkan needs saying
 * differently and nothing is approximated:
 *
 *   - "not mipmapped". Gallium has PIPE_TEX_MIPFILTER_NONE and Vulkan has no such mode.
 *     The standard spelling, and the one Mesa's own zink uses for that Gallium state
 *     (zink_create_sampler_state), is mipmapMode NEAREST with the LOD clamped to
 *     [0, 0.25]: level 0 is always the one chosen, and the LOD can still exceed 0, so the
 *     choice between the minifying and the magnifying filter is still made. A clamp of
 *     [0, 0] would make every sample a magnification.
 *   - MTLSamplerAddressModeClampToZero and ClampToBorderColor are CLAMP_TO_BORDER with
 *     transparent black, which is air_metal.c's reading too: rwa_sampler_state carries no
 *     border colour, and transparent black is Metal's default.
 *   - normalizedCoordinates NO (pixel coordinates) is Vulkan's unnormalizedCoordinates,
 *     whose rules are narrower than Metal's descriptor allows: one filter for both, no mip
 *     levels (minLod = maxLod = 0), clamping address modes, no anisotropy and no
 *     comparison. What Metal allows and Vulkan does not is refused by name; the LOD
 *     clamp, which no level but 0 can be reached through anyway, is set to 0.
 *   - [M3d-2] `normalize`: the same pixel-coordinate sampler, judged by every one of the
 *     rules above, and then made with unnormalizedCoordinates OFF, because it samples a
 *     texture3d and Vulkan has unnormalised coordinates on 1D and 2D views only
 *     (VkSamplerCreateInfo's valid usage, and the image view's at the sample). Nothing
 *     else changes: the one filter, mip mode NEAREST with the LOD clamped to [0, 0], no
 *     anisotropy, no comparison, and the same address modes -- lower_stage divides each
 *     coordinate sampled through it by the texture's size, so the sampler sees the same
 *     texel-space position it would have been handed in pixels (3D TEXTURES IN PIXEL
 *     COORDINATES). One rule more than the plain pixel-coordinate case, and it is about
 *     the third axis, which a 2D view never reads: r has to clamp too (CLAMP_TO_EDGE, or
 *     CLAMP_TO_BORDER for clamp_to_zero), because Metal defines a sampler in pixel
 *     coordinates for clamping alone (its header: "ClampToEdge wrap mode, otherwise the
 *     results of sampling are undefined") and a repeat on r in normalised space would be a
 *     meaning this file invented for it.
 *
 * MTLCompareFunction and VkCompareOp number their eight values identically (Never 0,
 * Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7), so the
 * compare op is the value itself once it is checked to be one of them.
 */
/* [M5b: and the border colour, which the comment's second point says the contract has none of:
 * it has now (rwa_sampler_state.border_color), so ClampToBorderColor reads it, and ClampToZero
 * stays transparent black. Vulkan's one colour is the sampler's, whichever axes read it, so an
 * opaque one beside a clamp_to_zero on another axis is refused by name, as is a colour Metal does
 * not have. `integer` is the same state for an integer texture: VK_BORDER_COLOR_INT_* (turnip's
 * two builtin kinds are packed for their own type, tu6_pack_border_color).] */
static bool
sampler_info(const struct rwa_device *device, const struct rwa_sampler_state *st,
             bool compare, bool normalize, bool integer, VkSamplerCreateInfo *ci, char *why,
             size_t why_size)
{
   static const VkSamplerAddressMode address[] = {
      [RWA_ADDRESS_CLAMP_TO_EDGE] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      [RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      [RWA_ADDRESS_REPEAT] = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      [RWA_ADDRESS_MIRROR_REPEAT] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
      [RWA_ADDRESS_CLAMP_TO_ZERO] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      [RWA_ADDRESS_CLAMP_TO_BORDER] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
   };
   static const char *const axis[3] = { "s", "t", "r" };

   for (unsigned i = 0; i < 3; i++) {
      if (st->address[i] >= ARRAY_SIZE(address)) {
         say(why, why_size, "address mode %u on %s is not one of MTLSamplerAddressMode's six",
             st->address[i], axis[i]);
         return false;
      }
   }
   if (st->min_filter > RWA_FILTER_LINEAR || st->mag_filter > RWA_FILTER_LINEAR) {
      say(why, why_size, "filters %u and %u: MTLSamplerMinMagFilter is nearest or linear",
          st->min_filter, st->mag_filter);
      return false;
   }
   if (st->mip_filter > RWA_MIP_LINEAR) {
      say(why, why_size, "mip filter %u is not one of MTLSamplerMipFilter's three",
          st->mip_filter);
      return false;
   }
   if (st->compare_function > 7) {
      say(why, why_size, "the sampler's compare function is %u, and MTLCompareFunction has "
                         "eight values (0 Never to 7 Always)", st->compare_function);
      return false;
   }
   if (st->max_anisotropy > 16) {
      say(why, why_size, "a maxAnisotropy of %u, where Metal's range is 1 to 16",
          st->max_anisotropy);
      return false;
   }
   if (!(st->lod_min <= st->lod_max)) {
      say(why, why_size, "a LOD clamp of [%g, %g]", (double)st->lod_min,
          (double)st->lod_max);
      return false;
   }
   if (st->border_color > 2) {
      say(why, why_size, "a border colour of %u: MTLSamplerBorderColor is TransparentBlack, "
                         "OpaqueBlack or OpaqueWhite", st->border_color);
      return false;
   }
   bool reads_border = false, reads_zero = false;
   for (unsigned i = 0; i < 3; i++) {
      reads_border |= st->address[i] == RWA_ADDRESS_CLAMP_TO_BORDER;
      reads_zero |= st->address[i] == RWA_ADDRESS_CLAMP_TO_ZERO;
   }
   if (reads_border && reads_zero && st->border_color != 0) {
      say(why, why_size, "a sampler with clamp_to_zero on one axis and clamp_to_border of colour "
                         "%u on another: a Vulkan sampler has one border colour, and these want "
                         "two (M5b)", st->border_color);
      return false;
   }
   VkBorderColor border = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK
                                  : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
   if (reads_border && st->border_color == 1)
      border = integer ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
   else if (reads_border && st->border_color == 2)
      border = integer ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

   *ci = (VkSamplerCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = st->mag_filter == RWA_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .minFilter = st->min_filter == RWA_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
      .mipmapMode = st->mip_filter == RWA_MIP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = address[st->address[0]],
      .addressModeV = address[st->address[1]],
      .addressModeW = address[st->address[2]],
      .anisotropyEnable = st->max_anisotropy > 1 && device->anisotropy,
      .maxAnisotropy = MIN2((float)MAX2(st->max_anisotropy, 1), device->max_anisotropy),
      .compareEnable = compare,
      .compareOp = (VkCompareOp)st->compare_function,
      .minLod = st->mip_filter == RWA_MIP_NOT_MIPMAPPED ? 0.0f : st->lod_min,
      .maxLod = st->mip_filter == RWA_MIP_NOT_MIPMAPPED ? 0.25f : st->lod_max,
      .borderColor = border,
      .unnormalizedCoordinates = !st->normalized_coordinates,
   };
   if (ci->anisotropyEnable)
      ci->maxAnisotropy = MAX2(ci->maxAnisotropy, 1.0f);

   if (!st->normalized_coordinates) {
      if (st->min_filter != st->mag_filter) {
         say(why, why_size, "a sampler in pixel coordinates with filters %u and %u: Vulkan "
                            "gives such a sampler one filter for both", st->min_filter,
             st->mag_filter);
         return false;
      }
      if (st->mip_filter != RWA_MIP_NOT_MIPMAPPED) {
         say(why, why_size, "a sampler in pixel coordinates with mip filter %u: such a "
                            "sampler reaches level 0 alone", st->mip_filter);
         return false;
      }
      const VkSamplerAddressMode uv[2] = { ci->addressModeU, ci->addressModeV };
      for (unsigned i = 0; i < 2; i++) {
         if (uv[i] != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
             uv[i] != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
            say(why, why_size, "a sampler in pixel coordinates with address mode %u on %s: "
                               "Vulkan allows such a sampler only to clamp", st->address[i],
                axis[i]);
            return false;
         }
      }
      ci->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      ci->minLod = ci->maxLod = 0.0f;
      ci->anisotropyEnable = VK_FALSE;
      if (compare) {
         say(why, why_size, "a comparing sampler in pixel coordinates, which Vulkan does "
                            "not have");
         return false;
      }
      /* [M3d-2: the same sampler made normalised, for a texture3d (see above).] */
      if (normalize) {
         if (ci->addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
             ci->addressModeW != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
            say(why, why_size, "a sampler in pixel coordinates with address mode %u on r, "
                               "sampling a texture3d: Metal defines a sampler in pixel "
                               "coordinates for clamping alone, and this back end samples a "
                               "texture3d in pixel coordinates through a normalised sampler "
                               "that has to clamp on all three axes for the two to read the "
                               "same texels", st->address[2]);
            return false;
         }
         ci->unnormalizedCoordinates = VK_FALSE;
      }
   }
   return true;
}

/* The plain sampler and, where Vulkan allows one, the comparing one. [M3d-2: `normalize`
 * is sampler_info's: a pixel-coordinate sampler made normalised for a texture3d, and then
 * pixel_coords is false, because it says what the Vulkan sampler is.] */
static bool
sampler_make(struct rwa_device *device, const struct rwa_sampler_state *st, bool normalize,
             struct rwa_sampler *out, char *why, size_t why_size)
{
   VkSamplerCreateInfo ci;
   VkResult r;
   memset(out, 0, sizeof(*out));
   out->device = device;
   out->pixel_coords = !st->normalized_coordinates && !normalize;
   if (!sampler_info(device, st, false, normalize, false, &ci, why, why_size))
      return false;
   if ((r = device->vk.CreateSampler(device->dev, &ci, NULL, &out->plain)) != VK_SUCCESS) {
      say(why, why_size, "turnip would not make a sampler (%s)", vk_Result_to_str(r));
      return false;
   }
   char ignored[64];
   if (st->normalized_coordinates &&
       sampler_info(device, st, true, false, false, &ci, ignored, sizeof(ignored)) &&
       device->vk.CreateSampler(device->dev, &ci, NULL, &out->compare) != VK_SUCCESS)
      out->compare = VK_NULL_HANDLE;
   /* [M5b: the integer twin, only where the colour differs between the two kinds: an opaque
    * border some address mode reads. Zero is zero in both.] */
   if (st->border_color != 0 &&
       (st->address[0] == RWA_ADDRESS_CLAMP_TO_BORDER ||
        st->address[1] == RWA_ADDRESS_CLAMP_TO_BORDER ||
        st->address[2] == RWA_ADDRESS_CLAMP_TO_BORDER) &&
       sampler_info(device, st, false, normalize, true, &ci, ignored, sizeof(ignored)) &&
       device->vk.CreateSampler(device->dev, &ci, NULL, &out->plain_int) != VK_SUCCESS)
      out->plain_int = VK_NULL_HANDLE;
   return true;
}

static void
sampler_unmake(struct rwa_sampler *s)
{
   if (s->plain)
      s->device->vk.DestroySampler(s->device->dev, s->plain, NULL);
   if (s->compare)
      s->device->vk.DestroySampler(s->device->dev, s->compare, NULL);
   if (s->plain_int)
      s->device->vk.DestroySampler(s->device->dev, s->plain_int, NULL);
   s->plain = s->compare = s->plain_int = VK_NULL_HANDLE;
}

/*
 * A shader's own `constexpr sampler`, decoded by the frontend (air_sampler_state), in
 * MTLSamplerDescriptor's terms: air_metal.c's sampler_state_from_air, table for table and
 * refusal for refusal, so the two back ends make the same sampler of the same word.
 */
static bool
sampler_state_from_air(const struct air_sampler_state *a, unsigned index,
                       struct rwa_sampler_state *out, char *why, size_t why_size)
{
   static const unsigned address[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = RWA_ADDRESS_CLAMP_TO_ZERO,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = RWA_ADDRESS_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = RWA_ADDRESS_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = RWA_ADDRESS_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = RWA_ADDRESS_CLAMP_TO_BORDER,
   };
   for (unsigned i = 0; i < 3; i++) {
      if (a->address[i] >= ARRAY_SIZE(address)) {
         say(why, why_size, "the shader's own sampler at index %u carries address mode %u, "
                            "which is none of the five its word can hold", index,
             a->address[i]);
         return false;
      }
      /* [M5b: the border colour is bound now, so "the shader's own sampler at index %u clamps to
       * a border colour %u (opaque black or white), and nothing in this contract can bind one"
       * is air_metal.c's refusal alone. Its word has two bits, and the fourth value is none of
       * Metal's three: sampler_info refuses it by name.] */
   }
   if (a->mag_filter == AIR_SAMPLER_BICUBIC || a->min_filter == AIR_SAMPLER_BICUBIC) {
      say(why, why_size, "the shader's own sampler at index %u asks for filter::bicubic, "
                         "which neither MTLSamplerDescriptor nor this GPU has", index);
      return false;
   }
   if (a->mag_filter > AIR_SAMPLER_BICUBIC || a->min_filter > AIR_SAMPLER_BICUBIC ||
       a->mip_filter > AIR_SAMPLER_MIP_LINEAR) {
      say(why, why_size, "the shader's own sampler at index %u carries filters %u/%u/%u, "
                         "which are not values its word can hold", index, a->mag_filter,
          a->min_filter, a->mip_filter);
      return false;
   }
   static const unsigned compare[] = {
      [0] = 0,   /* none          -> MTLCompareFunctionNever, as air_metal.c reads it */
      [1] = 1,   /* less          -> MTLCompareFunctionLess */
      [2] = 3,   /* less_equal    -> MTLCompareFunctionLessEqual */
      [3] = 4,   /* greater       -> MTLCompareFunctionGreater */
      [4] = 6,   /* greater_equal -> MTLCompareFunctionGreaterEqual */
      [5] = 2,   /* equal         -> MTLCompareFunctionEqual */
      [6] = 5,   /* not_equal     -> MTLCompareFunctionNotEqual */
      [7] = 7,   /* always        -> MTLCompareFunctionAlways */
      [8] = 0,   /* never         -> MTLCompareFunctionNever */
   };
   if (a->compare_func >= ARRAY_SIZE(compare)) {
      say(why, why_size, "the shader's own sampler at index %u carries compare function %u, "
                         "and Metal's compare_func has nine values: 0 none to 8 never",
          index, a->compare_func);
      return false;
   }
   *out = (struct rwa_sampler_state){
      .address = { address[a->address[0]], address[a->address[1]], address[a->address[2]] },
      .min_filter = a->min_filter == AIR_SAMPLER_LINEAR ? RWA_FILTER_LINEAR : RWA_FILTER_NEAREST,
      .mag_filter = a->mag_filter == AIR_SAMPLER_LINEAR ? RWA_FILTER_LINEAR : RWA_FILTER_NEAREST,
      .mip_filter = a->mip_filter == AIR_SAMPLER_MIP_LINEAR ? RWA_MIP_LINEAR
                  : a->mip_filter == AIR_SAMPLER_MIP_NEAREST ? RWA_MIP_NEAREST
                                                             : RWA_MIP_NOT_MIPMAPPED,
      .compare_function = compare[a->compare_func],
      .max_anisotropy = a->max_anisotropy,
      .normalized_coordinates = !a->pixel_coord,
      .lod_min = _mesa_half_to_float(a->lod_min),
      .lod_max = _mesa_half_to_float(a->lod_max),
      .border_color = a->border_color,
   };
   return true;
}

struct rwa_sampler *
rwa_sampler_create(struct rwa_device *device, const struct rwa_sampler_state *state,
                   char *why, size_t why_size)
{
   if (!state) {
      say(why, why_size, "a sampler with no descriptor");
      return NULL;
   }
   struct rwa_sampler *s = calloc(1, sizeof(*s));
   if (!s) {
      say(why, why_size, "out of memory making a sampler");
      return NULL;
   }
   if (!sampler_make(device, state, false, s, why, why_size)) {
      free(s);
      return NULL;
   }
   p_atomic_inc(&device->children);
   return s;
}

static void
sampler_free(struct rwa_device *device, void *object)
{
   struct rwa_sampler *sampler = object;
   sampler_unmake(sampler);
   p_atomic_dec(&device->children);
   free(sampler);
}

void
rwa_sampler_destroy(struct rwa_sampler *sampler)
{
   if (!sampler)
      return;
   argbuf_scrub(sampler->device, NULL, NULL, sampler);
   when_done(sampler->device, sampler->use, sampler_free, sampler);
}

/* ---------------------------------------------------------------------------------- */
/* Depth and stencil state: MTLDepthStencilDescriptor in Vulkan's terms (M3c-2)        */
/* ---------------------------------------------------------------------------------- */

/*
 * A created object, as in Metal and in air_metal.c, holding what the draw sets as DYNAMIC
 * state (DEPTH AND STENCIL, THE STATE) -- already translated, so every refusal about a
 * state happens here, where the caller's struct is still in hand. There is no Vulkan
 * object behind it: the pipeline does not change with it, and this struct is all of it.
 * The stencil reference is not here, because Metal keeps it on the encoder
 * (-setStencilReferenceValue:) and rwa_draw carries it; the draw puts it into each face.
 *
 * Not counted among the device's children (rwa_device_close): its destroy is a free() and
 * reaches for nothing of the device, so it cannot outlive anything it needs.
 */
struct rwa_depth_stencil {
   struct rwa_device *device;
   VkCompareOp depth_compare;
   VkBool32 depth_write;
   VkBool32 stencil_test;
   VkStencilOpState front, back;   /* reference 0: the draw's */
};

/*
 * MTLCompareFunction as VkCompareOp. The two enumerations run in the same order -- Never,
 * Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always -- so this table is the
 * identity, and it is written out for air_metal.c's reason: a cast is a bet that two
 * enumerations nobody here controls stay aligned, and the table is a statement that they
 * are, checkable in eight lines. (The sampler path's cast predates this table.)
 */
static bool
compare_op_from_metal(unsigned f, VkCompareOp *out, const char *what, char *why,
                      size_t why_size)
{
   static const VkCompareOp op[] = {
      VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
      VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
      VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS,
   };
   if (f >= ARRAY_SIZE(op)) {
      say(why, why_size, "%s is %u, which is not one of MTLCompareFunction's eight", what, f);
      return false;
   }
   *out = op[f];
   return true;
}

/*
 * MTLStencilOperation as VkStencilOp -- and here, unlike Gallium's PIPE_STENCIL_OP_x, it IS
 * the identity: Metal and Vulkan both count Keep, Zero, Replace, IncrementClamp,
 * DecrementClamp, Invert, IncrementWrap, DecrementWrap, with Invert at 5 in both, where
 * Gallium keeps it at 7 (air_metal.c's stencil_op_from_metal says what a cast costs
 * there). Written out all the same, so the agreement is read and not assumed.
 */
static bool
stencil_op_from_metal(unsigned op, VkStencilOp *out, const char *what, char *why,
                      size_t why_size)
{
   static const VkStencilOp ops[] = {
      VK_STENCIL_OP_KEEP,                 /* MTLStencilOperationKeep */
      VK_STENCIL_OP_ZERO,                 /* ...Zero */
      VK_STENCIL_OP_REPLACE,              /* ...Replace */
      VK_STENCIL_OP_INCREMENT_AND_CLAMP,  /* ...IncrementClamp */
      VK_STENCIL_OP_DECREMENT_AND_CLAMP,  /* ...DecrementClamp */
      VK_STENCIL_OP_INVERT,               /* ...Invert -- 5 here and in Metal */
      VK_STENCIL_OP_INCREMENT_AND_WRAP,   /* ...IncrementWrap */
      VK_STENCIL_OP_DECREMENT_AND_WRAP,   /* ...DecrementWrap */
   };
   if (op >= ARRAY_SIZE(ops)) {
      say(why, why_size, "%s is %u, which is not one of MTLStencilOperation's eight", what,
          op);
      return false;
   }
   *out = ops[op];
   return true;
}

/* air_metal.c's stencil_face_does_anything: Metal has no "stencil enabled" flag, and its
 * defaults -- compare Always, every operation Keep -- ARE the off state, so Vulkan's
 * stencilTestEnable is derived from them. */
static bool
stencil_face_does_anything(const struct rwa_stencil_face *f)
{
   return f->compare_function != 7 /* MTLCompareFunctionAlways */ ||
          f->stencil_fail_operation != 0 || f->depth_fail_operation != 0 ||
          f->depth_stencil_pass_operation != 0;
}

static bool
stencil_face_from_metal(const struct rwa_stencil_face *in, VkStencilOpState *out,
                        const char *face, char *why, size_t why_size)
{
   char what[64];
   snprintf(what, sizeof(what), "the %s face's stencilCompareFunction", face);
   if (!compare_op_from_metal(in->compare_function, &out->compareOp, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's stencilFailureOperation", face);
   if (!stencil_op_from_metal(in->stencil_fail_operation, &out->failOp, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthFailureOperation", face);
   if (!stencil_op_from_metal(in->depth_fail_operation, &out->depthFailOp, what, why,
                              why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthStencilPassOperation", face);
   if (!stencil_op_from_metal(in->depth_stencil_pass_operation, &out->passOp, what, why,
                              why_size))
      return false;
   /* Metal's masks are uint32 over an eight-bit buffer and its default is 0xFFFFFFFF;
    * the eight bits that can select anything are kept and the rest dropped, not refused,
    * as air_metal.c does -- refusing Metal's own default would refuse every descriptor. */
   out->compareMask = in->read_mask & 0xff;
   out->writeMask = in->write_mask & 0xff;
   out->reference = 0;
   return true;
}

struct rwa_depth_stencil *
rwa_depth_stencil_create(struct rwa_device *device,
                         const struct rwa_depth_stencil_state *state,
                         char *why, size_t why_size)
{
   if (!device) {
      say(why, why_size, "no device to make a depth/stencil state on");
      return NULL;
   }
   if (!state) {
      say(why, why_size, "a depth/stencil state with no descriptor");
      return NULL;
   }
   struct rwa_depth_stencil out = { .device = device };
   if (!compare_op_from_metal(state->depth_compare_function, &out.depth_compare,
                              "depthCompareFunction", why, why_size) ||
       !stencil_face_from_metal(&state->front, &out.front, "front", why, why_size) ||
       !stencil_face_from_metal(&state->back, &out.back, "back", why, why_size))
      return NULL;
   /* The depth test is ON whenever a state exists, as air_metal.c's depth_enabled = 1:
    * Metal has no enable bit, Always with no write is its off state, and in Vulkan that
    * is also the only way a depth WRITE happens at all -- with depthTestEnable off nothing
    * is written, so a state of Always and writes on (a shader's [[depth]] stored as it is)
    * needs the test on. */
   out.depth_write = state->depth_write_enabled ? VK_TRUE : VK_FALSE;
   out.stencil_test = stencil_face_does_anything(&state->front) ||
                      stencil_face_does_anything(&state->back);

   struct rwa_depth_stencil *ds = calloc(1, sizeof(*ds));
   if (!ds) {
      say(why, why_size, "out of memory making a depth/stencil state");
      return NULL;
   }
   *ds = out;
   return ds;
}

void
rwa_depth_stencil_destroy(struct rwa_depth_stencil *state)
{
   /* Nothing of Vulkan's to give back and nothing bound to unbind: a draw takes the
    * values out of this struct as it records, and keeps no pointer to it. */
   free(state);
}

/* ---------------------------------------------------------------------------------- */
/* What a stage binds, and AIR's resource form rewritten into Vulkan's                */
/* ---------------------------------------------------------------------------------- */

static const char *
shader_texture_name(enum glsl_sampler_dim dim, bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   return is_array ? "texture1d_array" : "texture1d";
   case GLSL_SAMPLER_DIM_2D:   return is_array ? "texture2d_array" : "texture2d";
   case GLSL_SAMPLER_DIM_3D:   return "texture3d";
   case GLSL_SAMPLER_DIM_CUBE: return is_array ? "texturecube_array" : "texturecube";
   case GLSL_SAMPLER_DIM_MS:   return is_array ? "texture2d_ms_array" : "texture2d_ms";
   case GLSL_SAMPLER_DIM_BUF:  return "texture_buffer";
   default:                    return "a texture of a kind with no Metal name";
   }
}

static const char *
base_type_name(nir_alu_type type)
{
   switch (type) {
   case nir_type_int:   return "int";
   case nir_type_uint:  return "uint";
   case nir_type_float: return "float";
   default:             return "a type with no Metal name";
   }
}

/* The MTLTextureType a shader's texture is, as air_metal.c's metal_texture_type. */
static bool
metal_texture_type(enum glsl_sampler_dim dim, bool is_array, unsigned *out)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   *out = is_array ? RWA_TYPE_1D_ARRAY : RWA_TYPE_1D; return true;
   case GLSL_SAMPLER_DIM_2D:   *out = is_array ? RWA_TYPE_2D_ARRAY : RWA_TYPE_2D; return true;
   case GLSL_SAMPLER_DIM_3D:   *out = RWA_TYPE_3D; return true;
   case GLSL_SAMPLER_DIM_CUBE: *out = is_array ? RWA_TYPE_CUBE_ARRAY : RWA_TYPE_CUBE; return true;
   case GLSL_SAMPLER_DIM_MS:
      *out = is_array ? RWA_TYPE_2D_MULTISAMPLE_ARRAY : RWA_TYPE_2D_MULTISAMPLE;
      return true;
   case GLSL_SAMPLER_DIM_BUF:  *out = RWA_TYPE_TEXTURE_BUFFER; return true;
   default:                    return false;
   }
}

/* The element type Apple's metadata declares for a texture, out of air.arg_type_name
 * ("texture2d<half, sample>"), as air_metal.c's declare_slot_element reads it.
 */
static void
declare_element(const char *type_name, struct rwa_binding *b)
{
   /* [M5c: an array's name is around its element's: array<texture2d<float, sample>, 3>] */
   if (type_name && !strncmp(type_name, "array<", 6))
      type_name += 6;
   const char *lt = type_name ? strchr(type_name, '<') : NULL;
   if (!lt)
      return;
   for (lt++; *lt == ' '; lt++)
      ;
   static const struct { const char *word; nir_alu_type type; unsigned bits; } words[] = {
      { "float",  nir_type_float, 32 }, { "half",   nir_type_float, 16 },
      { "int",    nir_type_int,   32 }, { "short",  nir_type_int,   16 },
      { "uint",   nir_type_uint,  32 }, { "ushort", nir_type_uint,  16 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
      size_t n = strlen(words[i].word);
      if (strncmp(lt, words[i].word, n) || (lt[n] != ',' && lt[n] != '>' && lt[n] != ' '))
         continue;
      b->declared_type = words[i].type;
      b->declared_element = words[i].word;
      b->declared_bits = words[i].bits;
      return;
   }
}

/* What a texture binding's element is, from the metadata first and the instructions
 * second: air_metal.c's slot_element. False when neither says.
 */
static bool
binding_element(const struct rwa_binding *b, nir_alu_type *type, const char **word,
                unsigned *bits)
{
   if (b->declared_element) {
      *type = b->declared_type;
      *word = b->declared_element;
      *bits = b->declared_bits;
      return true;
   }
   if (b->type_known) {
      *type = b->el_type;
      *word = base_type_name(b->el_type);
      *bits = 0;
      return true;
   }
   return false;
}

static struct rwa_binding *
find_binding(struct rwa_stage *st, enum rwa_bind_kind kind, unsigned index)
{
   for (unsigned i = 0; i < st->count; i++)
      if (st->b[i].kind == kind && index >= st->b[i].index &&
          index < st->b[i].index + st->b[i].span)
         return &st->b[i];
   return NULL;
}

/* M5c: how many descriptors a stage has, which is what a set holds: an array is its length. */
static unsigned
stage_descriptors(const struct rwa_stage *st)
{
   unsigned n = 0;
   for (unsigned i = 0; i < st->count; i++)
      n += st->b[i].span;
   return n;
}

/* M5c: which index a buffer's or an image's index source names, and the element of it chosen at
 * run time, NULL for none: the two forms of air_ptr_index, which nothing has moved yet. False for a
 * source that is neither. */
static bool
desc_index(nir_src src, unsigned *index, nir_def **element)
{
   *element = NULL;
   if (nir_src_is_const(src)) {
      *index = nir_src_as_uint(src);
      return true;
   }
   if (src.ssa->parent_instr->type == nir_instr_type_alu) {
      nir_alu_instr *a = nir_instr_as_alu(src.ssa->parent_instr);
      if (a->op == nir_op_iadd && nir_src_is_const(a->src[0].src)) {
         *index = nir_src_as_uint(a->src[0].src) >> 16;
         *element = a->src[1].src.ssa;
         return true;
      }
   }
   return false;
}

/* The element of an array binding as a value: `index` is what the instruction names, counted from
 * the binding's first, and `element` the part chosen at run time. */
static nir_def *
desc_element(nir_builder *b, const struct rwa_binding *bb, unsigned index, nir_def *element)
{
   nir_def *first = nir_imm_int(b, index - bb->index);
   return element ? nir_iadd(b, first, element) : first;
}

static const char *
resource_kind_name(enum air_resource_kind kind)
{
   switch (kind) {
   case AIR_RESOURCE_CONSTANT_BUFFER:    return "a constant buffer";
   case AIR_RESOURCE_DEVICE_BUFFER:      return "a device buffer";
   case AIR_RESOURCE_THREADGROUP_BUFFER: return "a threadgroup buffer";
   case AIR_RESOURCE_TEXTURE:            return "a sampled texture";
   case AIR_RESOURCE_IMAGE:              return "a written texture";
   case AIR_RESOURCE_SAMPLER:            return "a sampler";
   }
   return "a resource";
}

/* The buffer index source of a buffer intrinsic, or -1 when it is not one. */
static int
buffer_src(const nir_intrinsic_instr *in)
{
   switch (in->intrinsic) {
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_get_ssbo_size:
      return 0;
   case nir_intrinsic_store_ssbo:
      return 1;
   default:
      return -1;
   }
}

static bool
is_image_intrinsic(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_image_size:
   case nir_intrinsic_image_levels:
   case nir_intrinsic_image_samples:
      return true;
   default:
      return false;
   }
}

/*
 * What one stage binds: one rwa_binding per buffer, texture and sampler the module
 * declares, with what its instructions say about each -- air_metal.c's count_resources
 * and slot_shapes in one walk -- and, for a pipeline (`query` false), each refusal M3b
 * has to make, by name. `query` is rwa_module_textures, which only reports and refuses
 * nothing a draw would refuse later.
 */
static bool
stage_bindings(struct rwa_device *device, void *mem_ctx, nir_shader *nir,
               const struct air_shader_info *info, const char *word, unsigned base,
               bool query, struct rwa_stage *st, char *why, size_t why_size)
{
   memset(st, 0, sizeof(*st));
   st->name = info->entry_point ? info->entry_point : word;
   st->word = word;
   st->base = base;
   /* [M3d: and a kernel's stage, COMPUTE, which binds what a draw's stage binds and, of
    * the three, alone writes textures (STORAGE IMAGES).] */
   const bool kernel = nir->info.stage == MESA_SHADER_COMPUTE;
   st->flags = kernel ? VK_SHADER_STAGE_COMPUTE_BIT
             : nir->info.stage == MESA_SHADER_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT
                                                     : VK_SHADER_STAGE_FRAGMENT_BIT;
   st->b = rzalloc_array(mem_ctx, struct rwa_binding, MAX2(info->num_resources, 1));
   uint32_t ubo_touched = 0, ssbo_touched = 0;
   uint64_t sampled_touched = 0, image_touched = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               if (nir_instr_as_tex(instr)->texture_index < 64)
                  sampled_touched |= 1ull << nir_instr_as_tex(instr)->texture_index;
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_image_load || in->intrinsic == nir_intrinsic_image_store ||
                in->intrinsic == nir_intrinsic_image_atomic ||
                in->intrinsic == nir_intrinsic_image_atomic_swap ||
                in->intrinsic == nir_intrinsic_image_size || in->intrinsic == nir_intrinsic_image_levels) {
               if (nir_src_is_const(in->src[0]) && nir_src_as_uint(in->src[0]) < 64)
                  image_touched |= 1ull << nir_src_as_uint(in->src[0]);
               else
                  image_touched = ~0ull;
               continue;
            }
            const int src = in->intrinsic == nir_intrinsic_load_ubo || in->intrinsic == nir_intrinsic_load_ssbo ||
                                  in->intrinsic == nir_intrinsic_ssbo_atomic ||
                                  in->intrinsic == nir_intrinsic_ssbo_atomic_swap ||
                                  in->intrinsic == nir_intrinsic_get_ssbo_size ? 0
                          : in->intrinsic == nir_intrinsic_store_ssbo ? 1 : -1;
            if (src < 0 || !nir_src_is_const(in->src[src]) || nir_src_as_uint(in->src[src]) >= 32)
               continue;
            if (in->intrinsic == nir_intrinsic_load_ubo)
               ubo_touched |= 1u << nir_src_as_uint(in->src[src]);
            else
               ssbo_touched |= 1u << nir_src_as_uint(in->src[src]);
         }
      }
   }
   if (!st->b) {
      say(why, why_size, "out of memory reading what %s binds", st->name);
      return false;
   }

   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      enum rwa_bind_kind kind;
      unsigned limit;
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         kind = RWA_BIND_BUFFER;
         limit = RWA_MAX_BUFFERS;
         break;
      case AIR_RESOURCE_TEXTURE:
      case AIR_RESOURCE_IMAGE:
         kind = RWA_BIND_TEXTURE;
         limit = RWA_MAX_TEXTURES;
         break;
      case AIR_RESOURCE_SAMPLER:
         kind = RWA_BIND_SAMPLER;
         limit = RWA_MAX_SAMPLERS;
         break;
      case AIR_RESOURCE_THREADGROUP_BUFFER:
         /* A kernel's threadgroup buffer is memory and not a binding: nothing goes in the
          * push set for it, and its size is the dispatch's, which the contract does not
          * carry -- rwa_dispatch_buffers refuses it there, as air_metal.c does
          * (COMPUTE, THREADGROUP MEMORY). No other stage has one. */
         if (kernel)
            continue;
         FALLTHROUGH;
      default:
         if (query)
            continue;
         say(why, why_size, "%s binds %s at index %u, which a %s stage has no use for and "
                            "the Vulkan back end does not bind (M3b)", st->name,
             resource_kind_name(r->kind), r->index, word);
         return false;
      }
      if (query && kind != RWA_BIND_TEXTURE)
         continue;
      /* [M5c: an argument buffer's member is bound now (ARGUMENT BUFFERS), and a fragment or
       * vertex shader's storage image too (A TEXTURE A FRAGMENT OR VERTEX SHADER WRITES): the
       * two refusals here said "(M3b)". A member's index is the frontend's flat one and not
       * Metal's, so Metal's limits are not its limits.] */
      const bool member = r->argbuf_set != ~0u;
      /* M5d: `[[texture(n)]]` whose n is a constant's, and that constant says -1: not in this
       * specialization (air_to_nir.c, A RESOURCE'S INDEX THAT IS A FUNCTION CONSTANT'S) */
      if (r->gated && !member && r->index >= limit)
         continue;
      if (!member && r->index + r->array_size > limit) {
         say(why, why_size, "%s binds %s at index %u, past the %u Metal has", st->name,
             resource_kind_name(r->kind), r->index, limit);
         return false;
      }
      /* M5d: RenderBox's primitive_gradient_fragment has two [[function_constant]] arguments at one
       * buffer index, `constant` for one value and `device` for the other. The constants have been
       * folded by now, so the instructions say which this pipeline is, and the other is not there. */
      if (kind == RWA_BIND_BUFFER && r->gated && !member && r->index < 32 &&
          !((r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? ubo_touched : ssbo_touched) & (1u << r->index)))
         continue;
      /* and VideoToolbox's scaler has a texture it samples and a texture it writes at one index, one of
       * them for each value of a constant */
      if (kind == RWA_BIND_TEXTURE && r->gated && !member && r->index < 64 && !query &&
          !((r->kind == AIR_RESOURCE_IMAGE ? image_touched : sampled_touched) & (1ull << r->index)))
         continue;
      struct rwa_binding *b = find_binding(st, kind, r->index);
      if (b && kind == RWA_BIND_BUFFER && b->constant != (r->kind == AIR_RESOURCE_CONSTANT_BUFFER)) {
         say(why, why_size, "%s declares buffer %u both `constant` and `device`, and one "
                            "binding can be only one of them", st->name, r->index);
         return false;
      }
      if (!b) {
         b = &st->b[st->count++];
         b->kind = kind;
         b->index = r->index;
         b->r = r;
         b->constant = r->kind == AIR_RESOURCE_CONSTANT_BUFFER;
         b->ubo_ok = true;
         b->member = member;
         b->span = r->array_size;
         b->gated = r->gated;
      }
      b->written |= r->write && kind == RWA_BIND_BUFFER;
      if (kind == RWA_BIND_TEXTURE) {
         b->sampled |= r->kind == AIR_RESOURCE_TEXTURE;
         b->stored |= r->kind == AIR_RESOURCE_IMAGE;
         b->read |= r->read;
         b->write |= r->write;
         if (!b->declared_element)
            declare_element(r->type_name, b);
      }
      if (kind == RWA_BIND_SAMPLER && r->inline_sampler) {
         b->inline_sampler = true;
         b->pixel_coords = r->sampler.pixel_coord;
      }
   }

   /* The instructions: what shape each texture is read in, which samplers compare, which
    * buffers are written, and whether every load of a constant buffer is at a constant
    * offset inside its declared size (RESOURCES: a UBO).
    */
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *t = nir_instr_as_tex(instr);
               struct rwa_binding *b = find_binding(st, RWA_BIND_TEXTURE, t->texture_index);
               if (!b) {
                  if (query)
                     continue;
                  say(why, why_size, "%s samples texture %u, which it does not declare",
                      st->name, t->texture_index);
                  return false;
               }
               b->used = true;
               /* [M3d: a texture a kernel writes is a storage image, and one binding is
                * one descriptor of one type: a sample or fetch through the same index
                * would need a sampled one there too. Metal declares a slot one way or the
                * other, so no module of the corpus meets this; it is named, not bound.] */
               if (!query && b->stored) {
                  say(why, why_size, "%s samples texture %u through the texture path and "
                                     "declares it a texture it writes: Vulkan binds one "
                                     "descriptor of one kind at a binding, and this would "
                                     "need a sampled image and a storage image at one index "
                                     "(M3d)", st->name, t->texture_index);
                  return false;
               }
               if (!b->shape_known) {
                  b->shape_known = true;
                  b->dim = t->sampler_dim;
                  b->is_array = t->is_array;
               } else if (!query && (b->dim != t->sampler_dim || b->is_array != t->is_array)) {
                  say(why, why_size, "%s reads texture %u as a %s and as a %s", st->name,
                      t->texture_index, shader_texture_name(b->dim, b->is_array),
                      shader_texture_name(t->sampler_dim, t->is_array));
                  return false;
               }
               /* a query's result type is the size's, not the texel's */
               if (!b->type_known && t->op != nir_texop_txs &&
                   t->op != nir_texop_query_levels && t->op != nir_texop_texture_samples) {
                  b->type_known = true;
                  b->el_type = nir_alu_type_get_base_type(t->dest_type);
               }
               if (t->is_shadow)
                  b->shadow = true;
               if (!query && nir_tex_instr_need_sampler(t)) {
                  struct rwa_binding *s = find_binding(st, RWA_BIND_SAMPLER, t->sampler_index);
                  if (!s) {
                     say(why, why_size, "%s samples texture %u with sampler %u, which it does "
                                        "not declare", st->name, t->texture_index,
                         t->sampler_index);
                     return false;
                  }
                  s->used = true;
                  if (t->is_shadow)
                     s->compare = true;
                  /* [M5b: which kinds of texture it reads, and which textures read through it
                   * (the new fields of struct rwa_binding say what for)] */
                  if (nir_alu_type_get_base_type(t->dest_type) == nir_type_float)
                     s->samples_float = true;
                  else
                     s->samples_int = true;
                  if (t->sampler_index < 64)
                     b->sampler_mask |= 1ull << t->sampler_index;
                  /* Vulkan samples a 3D view with normalised coordinates only: which
                   * samplers do, so a pixel-coordinate one can be refused by name (M3c)
                   * [M3d-2: or, when it is the shader's own, made normalised with the
                   * coordinates divided (`normalize`, below); an API sampler is still
                   * judged at the draw, where it is first known] */
                  if (t->sampler_dim == GLSL_SAMPLER_DIM_3D)
                     s->samples_3d = true;
               }
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (is_image_intrinsic(in->intrinsic)) {
               /* [M3d: for a kernel these are bound now (STORAGE IMAGES), so a pipeline
                * refuses what lower_stage could not rewrite: an index computed at run
                * time, which only an argument buffer makes, and an index nothing
                * declares as a texture the kernel writes. M5c: and for a draw's stage.] */
               unsigned image_index;
               nir_def *image_element;
               if (!desc_index(in->src[0], &image_index, &image_element)) {
                  if (query)
                     continue;
                  say(why, why_size, "%s indexes the textures it writes at run time in a form "
                                     "this back end cannot read (M5c)", st->name);
                  return false;
               }
               struct rwa_binding *b = find_binding(st, RWA_BIND_TEXTURE, image_index);
               if (!query && (!b || !b->stored)) {
                  say(why, why_size, "%s writes or reads texture %u as an image, and it "
                                     "declares no texture it writes there", st->name, image_index);
                  return false;
               }
               if (b)
                  b->used = true;
               if (b && !b->shape_known) {
                  b->shape_known = true;
                  b->dim = nir_intrinsic_image_dim(in);
                  b->is_array = nir_intrinsic_image_array(in);
               }
               if (b && in->intrinsic == nir_intrinsic_image_levels)
                  b->asks_levels = true;   /* M5b: image_deref answers 1 */
               continue;
            }
            int s = buffer_src(in);
            if (s < 0 || query)
               continue;
            unsigned buffer_index;
            nir_def *buffer_element;
            if (!desc_index(in->src[s], &buffer_index, &buffer_element)) {
               say(why, why_size, "%s reads a buffer whose index is computed at run time in a "
                                  "form this back end cannot read (M5c)", st->name);
               return false;
            }
            struct rwa_binding *b = find_binding(st, RWA_BIND_BUFFER, buffer_index);
            if (!b) {
               say(why, why_size, "%s reads buffer %u, which it does not declare", st->name,
                   buffer_index);
               return false;
            }
            b->used = true;
            if (in->intrinsic == nir_intrinsic_store_ssbo ||
                in->intrinsic == nir_intrinsic_ssbo_atomic ||
                in->intrinsic == nir_intrinsic_ssbo_atomic_swap)
               b->written = true;
            if (in->intrinsic == nir_intrinsic_load_ubo) {
               unsigned bytes = in->def.num_components * in->def.bit_size / 8;
               if (!nir_src_is_const(in->src[1]) || !b->r->size ||
                   nir_src_as_uint(in->src[1]) + bytes > b->r->size)
                  b->ubo_ok = false;
            }
         }
      }
   }
   if (query)
      return true;

   /* [M5c: a member nothing reads goes, ARGUMENT BUFFERS says why] */
   unsigned kept = 0, members = 0;
   for (unsigned i = 0; i < st->count; i++)
      if (!st->b[i].member || st->b[i].used)
         st->b[kept++] = st->b[i];
   st->count = kept;

   /* The descriptor type and the binding number of each (RESOURCES). */
   for (unsigned i = 0; i < st->count; i++) {
      struct rwa_binding *b = &st->b[i];
      switch (b->kind) {
      case RWA_BIND_BUFFER:
         if (b->constant && b->ubo_ok && b->r->size && b->r->size <= device->max_ubo_range) {
            b->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b->ubo_size = b->r->size;
         } else {
            b->type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         }
         if (b->constant && b->written) {
            say(why, why_size, "%s stores to buffer %u, which it declares `constant`",
                st->name, b->index);
            return false;
         }
         b->binding = base + b->index;
         break;
      case RWA_BIND_TEXTURE:
         /* [M3d: a texture a kernel writes is a storage image, at the same range: Metal
          * numbers both kinds in one [[texture(n)]] space (COMPUTE, STORAGE IMAGES).] */
         if (b->stored && b->sampled) {
            say(why, why_size, "%s declares texture %u both as a texture it samples and as "
                               "one it writes: Vulkan binds one descriptor of one kind at a "
                               "binding (M3d)", st->name, b->index);
            return false;
         }
         b->type = b->stored ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
         b->binding = base + RWA_BIND_TEXTURE_BASE + b->index;
         break;
      case RWA_BIND_SAMPLER:
         b->type = VK_DESCRIPTOR_TYPE_SAMPLER;
         b->binding = base + RWA_BIND_SAMPLER_BASE + b->index;
         /* [M3d-2: the shader's own sampler in pixel coordinates that samples a texture3d
          * anywhere in this stage is one Vulkan sampler, and Vulkan has no unnormalised
          * one for a 3D view, so it is made normalised and every coordinate sampled
          * through it is divided (3D TEXTURES IN PIXEL COORDINATES). One that samples
          * only 1D and 2D textures keeps unnormalizedCoordinates, as since M3b.] */
         b->normalize = b->inline_sampler && b->pixel_coords && b->samples_3d;
         break;
      }
      /* [M5c: a member's flat index is not in Metal's ranges (ARGUMENT BUFFERS), so members
       * have one range of their own, in the order they come] */
      if (b->member) {
         if (members >= RWA_MAX_STAGE_MEMBERS) {
            say(why, why_size, "%s reads more than %u members of its argument buffers, and "
                               "this back end numbers that many bindings for them in one "
                               "stage (M5c)", st->name, RWA_MAX_STAGE_MEMBERS);
            return false;
         }
         b->binding = base + RWA_BIND_MEMBER_BASE + members++;
      }
   }
   return true;
}

/* The shader's own samplers, made once with the pipeline that declares them. */
static bool
stage_own_samplers(struct rwa_device *device, struct rwa_stage *st, char *why,
                   size_t why_size)
{
   for (unsigned i = 0; i < st->count; i++) {
      struct rwa_binding *b = &st->b[i];
      if (b->kind != RWA_BIND_SAMPLER || !b->inline_sampler)
         continue;
      struct rwa_sampler_state state;
      if (!sampler_state_from_air(&b->r->sampler, b->index, &state, why, why_size) ||
          !sampler_make(device, &state, b->normalize, &b->own, why, why_size))
         return false;
      if (b->compare && !b->own.compare) {
         say(why, why_size, "%s compares with its own sampler %u, which is in pixel "
                            "coordinates, and Vulkan has no comparing sampler of that kind",
             st->name, b->index);
         return false;
      }
      /* [M3d-2: a texture3d sampled with this sampler in pixel coordinates was refused
       * here until M3d-2 -- "samples a texture3d with its own sampler n, which is in pixel
       * coordinates: Vulkan allows unnormalizedCoordinates on 1D and 2D views only, and a
       * 3D texture in pixel coordinates is not in the Vulkan back end yet (M3c)", the
       * refusal of the six MetalFaceMask_DilateCube / ErodeCube kernels in runs 043116 and
       * 043536. Such a sampler is now `normalize` (stage_bindings) and was made normalised
       * just above, and lower_stage divides its coordinates (3D TEXTURES IN PIXEL
       * COORDINATES); what sampler_info refuses for it, it has said by now.] */
   }
   return true;
}

static void
stage_free_samplers(struct rwa_stage *st)
{
   for (unsigned i = 0; i < st->count; i++)
      if (st->b[i].own.device)
         sampler_unmake(&st->b[i].own);
}

/* A buffer's descriptor in the form nir_lower_explicit_io(vec2_index_32bit_offset) leaves
 * a SPIR-V buffer access in: vec3 (index, index, offset) out of load_vulkan_descriptor.
 */
static nir_def *
buffer_descriptor(nir_builder *b, const struct rwa_binding *bb, nir_def *element)
{
   nir_def *index = nir_vulkan_resource_index(b, 3, 32, element, .desc_set = 0,
                                              .binding = bb->binding,
                                              .desc_type = bb->type);
   return nir_load_vulkan_descriptor(b, 3, 32, index, .desc_type = bb->type);
}

static enum glsl_base_type
glsl_base_of(nir_alu_type t)
{
   switch (nir_alu_type_get_base_type(t)) {
   case nir_type_int:  return GLSL_TYPE_INT;
   case nir_type_uint: return GLSL_TYPE_UINT;
   default:            return GLSL_TYPE_FLOAT;
   }
}

/*
 * The rewrite itself (RESOURCES): every buffer access gets its descriptor, every texture
 * access a texture deref and, where the operation samples, a sampler deref, of uniforms
 * this makes on first use. A `constant` buffer bound as a STORAGE_BUFFER has its
 * load_ubo replaced by a non-writeable load_ssbo of the same bytes.
 *
 * One more thing, for the shader's own samplers in pixel coordinates: an implicit-LOD
 * sample (nir_texop_tex) through one becomes an explicit LOD 0 (txl), because Vulkan
 * forbids implicit LOD with unnormalizedCoordinates and such a sampler reaches level 0
 * alone anyway -- the same texel either way, said in the form Vulkan accepts.
 *
 * [M3d: and a kernel's image accesses (COMPUTE, STORAGE IMAGES): each image_* intrinsic
 * with its immediate index becomes the image_deref_* of a nir_var_image uniform of its
 * binding (image_deref, below), and image_levels the constant 1.]
 *
 * [M3d-2: and, for the shader's own pixel-coordinate sampler that samples a texture3d
 * (`normalize`), each coordinate sampled through it multiplied by 1 / its texture's size
 * at level 0 (pixel_to_normalized, below), after the txl rewrite above, which stays.]
 */

/* The deref form of an image intrinsic the frontend emits; nir_num_intrinsics for one
 * this file does not rewrite. */
static nir_intrinsic_op
image_deref_op(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_image_load:        return nir_intrinsic_image_deref_load;
   case nir_intrinsic_image_store:       return nir_intrinsic_image_deref_store;
   case nir_intrinsic_image_atomic:      return nir_intrinsic_image_deref_atomic;
   case nir_intrinsic_image_atomic_swap: return nir_intrinsic_image_deref_atomic_swap;
   case nir_intrinsic_image_size:        return nir_intrinsic_image_deref_size;
   case nir_intrinsic_image_samples:     return nir_intrinsic_image_deref_samples;
   default:                              return nir_num_intrinsics;
   }
}

/*
 * One image access in the form spirv_to_nir makes of a SPIR-V one (COMPUTE, STORAGE
 * IMAGES): the same sources after the first, which becomes a deref of the binding's
 * nir_var_image uniform, and the same indices -- dim, array, format, access and the
 * data or atomic type -- set one by one, because the two forms keep them at different
 * positions (the immediate form has RANGE_BASE, which the deref form has not), so a copy
 * of const_index[] would put each in the wrong place. turnip's lower_image_deref then
 * makes it bindless and nir_rewrite_image_intrinsic keeps every index set here.
 *
 * image_levels is the one that does not survive: turnip lowers no image_deref_levels, and
 * Vulkan has no level query on a storage image at all. Every texture this back end makes
 * has one level (rwa_texture_create: mipLevels 1), so the answer is 1 for any texture a
 * dispatch can bind, and it is given as that constant.
 * [M5b: a chain is a texture of this back end now, so that constant holds of a texture of one
 * level alone, and resolve_binding refuses another by name for a shader that asks.]
 */
static bool
image_deref(nir_builder *b, nir_intrinsic_instr *in, struct rwa_binding *tb, unsigned index,
            nir_def *element, const struct rwa_stage *st, char *why, size_t why_size)
{
   if (in->intrinsic == nir_intrinsic_image_levels) {
      nir_def_replace(&in->def, nir_imm_intN_t(b, 1, in->def.bit_size));
      return true;
   }
   nir_intrinsic_op op = image_deref_op(in->intrinsic);
   if (op == nir_num_intrinsics) {
      say(why, why_size, "%s uses texture %u with the image operation %s, which this back "
                         "end has no Vulkan form for (M3d)", st->name, tb->index,
          nir_intrinsic_infos[in->intrinsic].name);
      return false;
   }
   if (!tb->var) {
      nir_alu_type el;
      const char *w;
      unsigned bits;
      enum glsl_base_type base = binding_element(tb, &el, &w, &bits)
                                    ? glsl_base_of(el) : GLSL_TYPE_UINT;
      const struct glsl_type *one = glsl_image_type(tb->dim, tb->is_array, base);
      tb->var = nir_variable_create(b->shader, nir_var_image,
                                    tb->span > 1 ? glsl_array_type(one, tb->span, 0) : one,
                                    "air_image");
      tb->var->data.descriptor_set = 0;
      tb->var->data.binding = tb->binding;
      tb->var->data.image.format = PIPE_FORMAT_NONE;
   }
   nir_intrinsic_instr *d = nir_intrinsic_instr_create(b->shader, op);
   d->num_components = in->num_components;
   nir_deref_instr *deref = nir_build_deref_var(b, tb->var);
   if (tb->span > 1)
      deref = nir_build_deref_array(b, deref, desc_element(b, tb, index, element));
   d->src[0] = nir_src_for_ssa(&deref->def);
   for (unsigned i = 1; i < nir_intrinsic_infos[in->intrinsic].num_srcs; i++)
      d->src[i] = nir_src_for_ssa(in->src[i].ssa);
   nir_intrinsic_set_image_dim(d, nir_intrinsic_image_dim(in));
   nir_intrinsic_set_image_array(d, nir_intrinsic_image_array(in));
   nir_intrinsic_set_format(d, nir_intrinsic_format(in));
   nir_intrinsic_set_access(d, nir_intrinsic_access(in) | (element ? ACCESS_NON_UNIFORM : 0));
   if (nir_intrinsic_has_dest_type(in))
      nir_intrinsic_set_dest_type(d, nir_intrinsic_dest_type(in));
   if (nir_intrinsic_has_src_type(in))
      nir_intrinsic_set_src_type(d, nir_intrinsic_src_type(in));
   if (nir_intrinsic_has_atomic_op(in))
      nir_intrinsic_set_atomic_op(d, nir_intrinsic_atomic_op(in));
   if (nir_intrinsic_infos[op].has_dest) {
      nir_def_init(&d->instr, &d->def, in->def.num_components, in->def.bit_size);
      nir_builder_instr_insert(b, &d->instr);
      nir_def_replace(&in->def, &d->def);
   } else {
      nir_builder_instr_insert(b, &d->instr);
      nir_instr_remove(&in->instr);
   }
   return true;
}

/*
 * 3D TEXTURES IN PIXEL COORDINATES (M3d-2): one sample through a shader's own
 * pixel-coordinate sampler that has been made normalised (`normalize`, stage_bindings),
 * said in normalised coordinates. Six of PortraitFilters' kernels need it --
 * MetalFaceMask_DilateCube{Luma,Chroma1,Chroma2} and ErodeCube{...}, each a 3D texture
 * sampled with sampler(coord::pixel) at (gid + 0.5) and a texel offset -- and Vulkan's
 * unnormalizedCoordinates, which the 2D case uses, is allowed on 1D and 2D views alone. So
 * the coordinate is divided here and the sampler does the rest exactly as it would have:
 *
 *   coord' = coord * (1 / float(textureSize(texture, 0)))
 *
 * a txs of the same texture at level 0 (the deref form spirv_to_nir makes of
 * OpImageQuerySizeLod, which turnip lowers as it lowers that), converted to the
 * coordinate's float width, and its reciprocal -- a multiply, which is what ir3 makes of a
 * division anyway (lower_fdiv). For a power-of-two size the reciprocal is exact.
 *
 * NO HALF-TEXEL OFFSET is added or taken away, and that is the point of the formula:
 * Metal's pixel coordinates are already texel-space coordinates, with texel i centred at
 * i + 0.5 (Apple's rule, as tools/air/sampcheck.py states and checks it), and a normalised
 * coordinate times the size is that same texel space -- the space in which a Vulkan
 * sampler picks floor(u) for nearest and weights frac(u - 0.5) for linear. Dividing by
 * the size and nothing else therefore lands nearest and linear on the same centres and
 * the same weights; a +-0.5 here would move every tap by half a texel.
 *
 * What does not change, and why each is right:
 *   - THE OFFSETS (nir_tex_src_offset, constant or computed -- ErodeCubeChroma2 computes
 *     one): they are in texels in either space, applied by the texture unit after the
 *     coordinate is scaled, so they stay as they are.
 *   - THE LOD: the txl LOD 0 the M3b rewrite gave an implicit-LOD sample stays, and a
 *     shader's explicit LOD or bias reaches level 0 alone through a sampler clamped to
 *     [0, 0] with one filter, as it did in pixel coordinates.
 *   - THE ADDRESS MODES: clamp to edge in pixel space is clamp to edge in normalised
 *     space, and clamp_to_zero (CLAMP_TO_BORDER, transparent black) has its border at the
 *     same place, outside [0, size) in texels -- so a linear tap across the edge blends
 *     the same texel with the same zero. sampler_info has required all three axes to
 *     clamp for this case, which is all Metal defines in pixel coordinates.
 *   - AN ARRAY LAYER, which only a 1D or 2D array sampled through the same sampler has
 *     (a texture3d is never an array): it is a whole number in both spaces, so its scale
 *     is 1.
 * The gradients of a txd are scaled with the coordinate, because they are its
 * derivatives; through a sampler clamped to level 0 with one filter that changes no
 * texel, and it keeps the instruction saying one coordinate space throughout.
 *
 * Every sample through such a sampler is lowered, a 1D or 2D one included: the sampler is
 * one descriptor and it is now normalised, so a 2D sample through it is divided by the
 * same arithmetic. A sampler that samples no texture3d is not `normalize` and keeps the
 * unnormalizedCoordinates path M3b made, untouched. A cube through it is refused by name:
 * its coordinate is a direction, and Metal has no pixel coordinates for one.
 */
static bool
pixel_to_normalized(nir_builder *b, nir_tex_instr *tex, struct rwa_binding *tb,
                    const struct rwa_binding *sb, const struct rwa_stage *st, char *why,
                    size_t why_size)
{
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_1D && tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
       tex->sampler_dim != GLSL_SAMPLER_DIM_3D) {
      say(why, why_size, "%s samples a %s with its own sampler %u, which is in pixel "
                         "coordinates and also samples a texture3d, so it is made a "
                         "normalised sampler here: the coordinate of a %s is not a position "
                         "in texels, and there is no size to divide it by", st->name,
          shader_texture_name(tex->sampler_dim, tex->is_array), sb->index,
          shader_texture_name(tex->sampler_dim, tex->is_array));
      return false;
   }
   int c = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (c < 0 || nir_alu_type_get_base_type(nir_tex_instr_src_type(tex, c)) != nir_type_float) {
      say(why, why_size, "%s samples texture %u with its own sampler %u, which is in pixel "
                         "coordinates, and the instruction has no floating-point coordinate "
                         "to divide", st->name, tb->index, sb->index);
      return false;
   }
   nir_def *coord = tex->src[c].src.ssa;
   nir_def *size = nir_txs_deref(b, nir_build_deref_var(b, tb->var), nir_imm_int(b, 0));
   if (size->num_components != coord->num_components) {
      say(why, why_size, "%s samples texture %u with a %u-component coordinate and its size "
                         "has %u components", st->name, tb->index, coord->num_components,
          size->num_components);
      return false;
   }
   nir_def *scale = nir_frcp(b, nir_i2fN(b, size, coord->bit_size));
   if (tex->is_array)
      scale = nir_vector_insert_imm(b, scale, nir_imm_floatN_t(b, 1.0, coord->bit_size),
                                    coord->num_components - 1);
   nir_src_rewrite(&tex->src[c].src, nir_fmul(b, coord, scale));

   const nir_tex_src_type grads[2] = { nir_tex_src_ddx, nir_tex_src_ddy };
   for (unsigned i = 0; i < 2; i++) {
      int g = nir_tex_instr_src_index(tex, grads[i]);
      if (g < 0)
         continue;
      nir_def *d = tex->src[g].src.ssa;
      nir_def *s = nir_trim_vector(b, scale, d->num_components);
      if (d->bit_size != s->bit_size)
         s = nir_f2fN(b, s, d->bit_size);
      nir_src_rewrite(&tex->src[g].src, nir_fmul(b, d, s));
   }
   return true;
}

static bool
lower_stage(struct rwa_stage *st, nir_shader *nir, char *why, size_t why_size)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         b.cursor = nir_before_instr(instr);
         if (instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            /* [M5c: an array of descriptors chosen at run time (ARRAYS OF DESCRIPTORS). This
             * refused: "indexes its textures at run time, which only an argument buffer makes".] */
            nir_def *texture_element = NULL, *sampler_element = NULL;
            int off_src = nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
            if (off_src >= 0) {
               texture_element = tex->src[off_src].src.ssa;
               nir_tex_instr_remove_src(tex, off_src);
            }
            off_src = nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset);
            if (off_src >= 0) {
               sampler_element = tex->src[off_src].src.ssa;
               nir_tex_instr_remove_src(tex, off_src);
            }
            struct rwa_binding *tb = find_binding(st, RWA_BIND_TEXTURE, tex->texture_index);
            if (!tb)
               return false;   /* stage_bindings has said it */
            if (!tb->var) {
               nir_alu_type el;
               const char *w;
               unsigned bits;
               enum glsl_base_type base = binding_element(tb, &el, &w, &bits)
                                             ? glsl_base_of(el) : GLSL_TYPE_FLOAT;
               const struct glsl_type *one = glsl_texture_type(tb->dim, tb->is_array, base);
               tb->var = nir_variable_create(nir, nir_var_uniform,
                                             tb->span > 1 ? glsl_array_type(one, tb->span, 0) : one,
                                             "air_texture");
               tb->var->data.descriptor_set = 0;
               tb->var->data.binding = tb->binding;
            }
            /* an element chosen at run time may differ per pixel: ir3 has a bit for that */
            tex->texture_non_uniform = texture_element != NULL;
            tex->sampler_non_uniform = sampler_element != NULL;
            nir_deref_instr *tderef = nir_build_deref_var(&b, tb->var);
            if (tb->span > 1)
               tderef = nir_build_deref_array(&b, tderef, desc_element(&b, tb, tex->texture_index,
                                                                       texture_element));
            nir_tex_instr_add_src(tex, nir_tex_src_texture_deref, &tderef->def);
            if (nir_tex_instr_need_sampler(tex)) {
               struct rwa_binding *sb = find_binding(st, RWA_BIND_SAMPLER, tex->sampler_index);
               if (!sb)
                  return false;
               if (!sb->var) {
                  sb->var = nir_variable_create(nir, nir_var_uniform,
                                                sb->span > 1
                                                   ? glsl_array_type(glsl_bare_sampler_type(),
                                                                     sb->span, 0)
                                                   : glsl_bare_sampler_type(),
                                                "air_sampler");
                  sb->var->data.descriptor_set = 0;
                  sb->var->data.binding = sb->binding;
               }
               nir_deref_instr *sderef = nir_build_deref_var(&b, sb->var);
               if (sb->span > 1)
                  sderef = nir_build_deref_array(&b, sderef,
                                                 desc_element(&b, sb, tex->sampler_index,
                                                              sampler_element));
               nir_tex_instr_add_src(tex, nir_tex_src_sampler_deref, &sderef->def);
               /* [M5e: and any sample in a kernel or a vertex function, through an API sampler as
                * well. There is no quad there to take differences from, and Metal's level is 0;
                * the Adreno's implicit sample in a compute shader answered zeros, which is why
                * VideoToolbox's scale wrote an empty picture (vt_test --pixels, run 160351).] */
               if (((sb->inline_sampler && sb->pixel_coords) || nir->info.stage != MESA_SHADER_FRAGMENT) &&
                   tex->op == nir_texop_tex) {
                  tex->op = nir_texop_txl;
                  nir_tex_instr_add_src(tex, nir_tex_src_lod, nir_imm_float(&b, 0.0f));
               }
               /* [M3d-2: and the coordinate divided, for a sampler made normalised because
                * it samples a texture3d (3D TEXTURES IN PIXEL COORDINATES).] */
               if (sb->normalize && !pixel_to_normalized(&b, tex, tb, sb, st, why, why_size))
                  return false;
            }
            tex->texture_index = 0;
            tex->sampler_index = 0;
            continue;
         }
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         /* [M3d: a kernel's image access, whose index stage_bindings has checked is a
          * constant naming a texture the kernel writes (COMPUTE, STORAGE IMAGES); a draw's
          * stage that has one was refused there.] */
         if (is_image_intrinsic(in->intrinsic)) {
            unsigned image_index;
            nir_def *image_element;
            struct rwa_binding *ib = desc_index(in->src[0], &image_index, &image_element)
               ? find_binding(st, RWA_BIND_TEXTURE, image_index) : NULL;
            if (!ib || !ib->stored) {
               say(why, why_size, "%s uses an image no binding of it declares", st->name);
               return false;
            }
            if (!image_deref(&b, in, ib, image_index, image_element, st, why, why_size))
               return false;
            continue;
         }
         int s = buffer_src(in);
         if (s < 0)
            continue;
         unsigned buffer_index;
         nir_def *buffer_element;
         if (!desc_index(in->src[s], &buffer_index, &buffer_element))
            return false;
         struct rwa_binding *bb = find_binding(st, RWA_BIND_BUFFER, buffer_index);
         if (!bb)
            return false;
         nir_def *desc = buffer_descriptor(&b, bb, desc_element(&b, bb, buffer_index,
                                                                buffer_element));
         if (buffer_element && nir_intrinsic_has_access(in))
            nir_intrinsic_set_access(in, nir_intrinsic_access(in) | ACCESS_NON_UNIFORM);
         nir_def *index = nir_trim_vector(&b, desc, 2);
         nir_def *base = nir_channel(&b, desc, 2);
         if (in->intrinsic == nir_intrinsic_load_ubo &&
             bb->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            nir_def *value = nir_load_ssbo(&b, in->def.num_components, in->def.bit_size, index,
                                           nir_iadd(&b, in->src[1].ssa, base),
                                           .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER |
                                                     (buffer_element ? ACCESS_NON_UNIFORM : 0),
                                           .align_mul = nir_intrinsic_align_mul(in),
                                           .align_offset = nir_intrinsic_align_offset(in));
            nir_def_replace(&in->def, value);
            continue;
         }
         nir_src_rewrite(&in->src[s], index);
         if (in->intrinsic != nir_intrinsic_get_ssbo_size) {
            unsigned off = s + 1;
            nir_src_rewrite(&in->src[off], nir_iadd(&b, in->src[off].ssa, base));
         }
      }
   }
   nir_metadata_preserve(impl, nir_metadata_none);
   return true;
}

int
rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
                    const char *entry, struct rwa_texture_decl *out, unsigned max,
                    unsigned *count, char *why, size_t why_size)
{
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u texture declarations", max);
      return -1;
   }
   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }
   /* the translation a pipeline would make, with the options a pipeline would use, so the
    * answer is the one the draw will judge a binding by (air_metal.c says why at length) */
   struct air_constant_value *constants;
   unsigned num_constants = air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants, .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options, device->nir_options,
                                &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   struct rwa_stage st;
   if (!stage_bindings(device, mem_ctx, nir, &info, "shader", 0, true, &st, why, why_size)) {
      ralloc_free(mem_ctx);
      return -1;
   }
   unsigned n = 0;
   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      struct rwa_binding *b = find_binding(&st, RWA_BIND_TEXTURE, i);
      if (!b)
         continue;
      if (n >= max) {
         say(why, why_size, "%s declares a texture at slot %u and there is room for %u "
                            "declaration%s", st.name, i, max, max == 1 ? "" : "s");
         ralloc_free(mem_ctx);
         return -1;
      }
      struct rwa_texture_decl *d = &out[n++];
      memset(d, 0, sizeof(*d));
      d->index = i;
      if (b->sampled || b->read)
         d->usage |= RWA_USAGE_SHADER_READ;
      if (b->stored || b->write)
         d->usage |= RWA_USAGE_SHADER_WRITE;
      if (b->shape_known)
         d->type_known = metal_texture_type(b->dim, b->is_array, &d->type);
      nir_alu_type have;
      const char *word;
      unsigned bits;
      if (binding_element(b, &have, &word, &bits)) {
         d->element = have == nir_type_float ? RWA_ELEMENT_FLOAT
                    : have == nir_type_int   ? RWA_ELEMENT_INT
                    : have == nir_type_uint  ? RWA_ELEMENT_UINT
                                             : RWA_ELEMENT_UNKNOWN;
         if (d->element != RWA_ELEMENT_UNKNOWN)
            d->element_bits = bits;
      }
      if (b->r && b->r->type_name)
         snprintf(d->metal_type, sizeof(d->metal_type), "%s", b->r->type_name);
   }
   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}


/* ---------------------------------------------------------------------------------- */
/* Render pipelines                                                                   */
/* ---------------------------------------------------------------------------------- */

/* The constant coordinate of the generated vertex stage: the centre of texel (1, 1) of a
 * 4x4 texture, air_metal.c's RWA_TEX_* exactly, so a pixel drawn here and one drawn there
 * are the same measurement.
 */
#define RWA_TEX_SIZE 4
#define RWA_TEX_X 1
#define RWA_TEX_Y 1

/*
 * The generated vertex stage: air_metal.c's quad_vertex_shader, with the corners taken
 * from the vertex index instead of from a vertex buffer. Vertex v of the strip is
 * x = (v & 1) ? 1 : -1, y = (v & 2) ? 1 : -1, which is rwa_quad_verts' order --
 * (-1,-1) (1,-1) (-1,1) (1,1) -- and z = 0, w = 1. The index is nir_load_vertex_id, which
 * is Vulkan's VertexIndex (it counts from the draw's firstVertex), so vertex_start picks
 * corners exactly as it picks them out of air_metal.c's buffer, and render_draw holds
 * vertex_start + vertex_count to four as air_metal.c does. ir3 takes the intrinsic as it is
 * on a6xx (its nir_options.vertex_id_zero_based is false there, so turnip's
 * nir_lower_system_values leaves it alone), which is the form SPIR-V's VertexIndex reaches
 * it in.
 *
 * Then one vec4 out per varying the fragment shader reads, at VARYING_SLOT_VAR0 + i, the
 * slot the frontend gives the i'th fragment input, holding the one coordinate above. A
 * fragment input narrower than a vec4 reads the first components of it, which Vulkan's
 * interface rules allow (an input may have fewer components than the output it matches).
 */
static nir_shader *
quad_vertex_shader(void *mem_ctx, const struct nir_shader_compiler_options *options,
                   unsigned num_varyings)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "quad");
   ralloc_steal(mem_ctx, b.shader);

   nir_def *v = nir_load_vertex_id(&b);
   nir_def *x = nir_fadd_imm(&b, nir_fmul_imm(&b, nir_u2f32(&b, nir_iand_imm(&b, v, 1)), 2.0),
                             -1.0);
   nir_def *y = nir_fadd_imm(&b, nir_fmul_imm(&b, nir_u2f32(&b,
                                   nir_iand_imm(&b, nir_ushr_imm(&b, v, 1), 1)), 2.0), -1.0);
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(),
                                           "pos_out");
   out->data.location = VARYING_SLOT_POS;
   nir_store_var(&b, out, nir_vec4(&b, x, y, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)),
                 0xf);

   float c = (RWA_TEX_X + 0.5f) / RWA_TEX_SIZE;
   float d = (RWA_TEX_Y + 0.5f) / RWA_TEX_SIZE;
   for (unsigned i = 0; i < num_varyings; i++) {
      nir_variable *var = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(),
                                              "varying");
      var->data.location = VARYING_SLOT_VAR0 + i;
      var->data.interpolation = INTERP_MODE_SMOOTH;
      nir_store_var(&b, var, nir_imm_vec4(&b, c, d, c, 1.0f), 0xf);
   }
   return b.shader;
}

/*
 * What vk_spirv_to_nir does to every SPIR-V shader after spirv_to_nir and before turnip
 * sees it (src/vulkan/runtime/vk_nir.c) -- the part of it that applies to a shader that
 * arrives as NIR, because vk_pipeline_shader_stage_to_nir hands such a shader to turnip
 * untouched. The frontend has already inlined every function and kept only "main"
 * (air_to_nir.c), so what is left is the variable housekeeping turnip's own passes
 * assume: initialisers lowered, copies split, structs split per member, and the
 * interface variables nothing reads removed.
 */
/*
 * A half varying (M5d). Metal's `half4 color` between the stages is f16vec4 out of the frontend, in both
 * stages, and ir3 will compile that: `cov.f32f16 hr10.z` in the vertex stage and `bary.f hr2.x` in the
 * fragment one. What arrives is not what was sent (the row half-varying; CoreAnimation's solid fills came
 * out a different colour every frame in run 134123, `metal_drv_test --ca-fill`). Turnip does not offer
 * this: storageInputOutput16 is off in tu_device.cc, so nothing that reaches it from SPIR-V has a 16-bit
 * varying, and what it does with one is untested by anybody. So here it does not get one: the varying is
 * 32 bits wide between the stages, with a conversion at each end, and a half is a half again in the shader.
 */
static void
widen_varyings(nir_shader *nir)
{
   const nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX ? nir_var_shader_out
                                : nir->info.stage == MESA_SHADER_FRAGMENT ? nir_var_shader_in
                                                                          : (nir_variable_mode)0;
   if (!mode)
      return;
   bool any = false;
   nir_foreach_variable_with_modes(var, nir, mode) {
      if (var->data.location < VARYING_SLOT_VAR0 || !glsl_type_is_vector_or_scalar(var->type))
         continue;
      const enum glsl_base_type base = glsl_get_base_type(var->type);
      const enum glsl_base_type wide = base == GLSL_TYPE_FLOAT16 ? GLSL_TYPE_FLOAT
                                     : base == GLSL_TYPE_INT16 ? GLSL_TYPE_INT
                                     : base == GLSL_TYPE_UINT16 ? GLSL_TYPE_UINT : base;
      if (wide != base) {
         var->type = glsl_vector_type(wide, glsl_get_components(var->type));
         any = true;
      }
   }
   if (!any)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref) {
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->deref_type == nir_deref_type_var && nir_deref_mode_is(deref, mode))
               deref->type = deref->var->type;
            continue;
         }
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_load_deref && in->intrinsic != nir_intrinsic_store_deref)
            continue;
         nir_deref_instr *deref = nir_src_as_deref(in->src[0]);
         if (!nir_deref_mode_is(deref, mode))
            continue;
         const nir_variable *var = nir_deref_instr_get_variable(deref);
         const enum glsl_base_type base = glsl_get_base_type(var->type);
         if (in->intrinsic == nir_intrinsic_load_deref) {
            if (in->def.bit_size != 16 || glsl_get_bit_size(var->type) != 32)
               continue;
            in->def.bit_size = 32;
            b.cursor = nir_after_instr(instr);
            nir_def *narrow = base == GLSL_TYPE_FLOAT ? nir_f2f16(&b, &in->def) : nir_i2i16(&b, &in->def);
            nir_def_rewrite_uses_after(&in->def, narrow, narrow->parent_instr);
         } else {
            nir_def *value = in->src[1].ssa;
            if (value->bit_size != 16 || glsl_get_bit_size(var->type) != 32)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&in->src[1], base == GLSL_TYPE_FLOAT ? nir_f2f32(&b, value)
                                       : base == GLSL_TYPE_INT ? nir_i2i32(&b, value)
                                                               : nir_u2u32(&b, value));
         }
      }
   }
   nir_metadata_preserve(impl, nir_metadata_control_flow);
}

static void
shape_like_spirv(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   NIR_PASS(_, nir, nir_propagate_invariant, false);
   widen_varyings(nir);
}

static bool
writes_location(nir_shader *nir, int location)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(in->src[0]);
            nir_variable *var = d ? nir_deref_instr_get_variable(d) : NULL;
            if (var && var->data.mode == nir_var_shader_out &&
                var->data.location == location)
               return true;
         }
      }
   }
   return false;
}

/* MTLPrimitiveType in Vulkan's terms. */
static bool
topology_from_metal(unsigned p, VkPrimitiveTopology *out, char *why, size_t why_size)
{
   switch (p) {
   case 0: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;     return true;
   case 1: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;      return true;
   case 2: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;     return true;
   case 3: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  return true;
   case 4: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
   }
   say(why, why_size, "primitive type %u is not a value of MTLPrimitiveType", p);
   return false;
}

/* The pipeline for one topology, made the first time a draw asks for it. */
/* The pipeline for one topology, made the first time a draw asks for it. */
static VkPipeline
pipeline_for(struct rwa_render *render, unsigned primitive, char *why, size_t why_size)
{
   if (primitive < RWA_PRIMITIVE_TYPES && render->pipeline[primitive])
      return render->pipeline[primitive];
   struct rwa_device *device = render->device;
   VkPrimitiveTopology topology;
   if (!topology_from_metal(primitive, &topology, why, why_size))
      return VK_NULL_HANDLE;
   /* A point's size is whatever the vertex stage writes to [[point_size]], and Vulkan
    * leaves it undefined when nothing does; the generated stage writes none.
    */
   if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && !render->vs_writes_point_size) {
      say(why, why_size, "a draw of points with a vertex stage that writes no "
                         "[[point_size]], which leaves the size of every point undefined");
      return VK_NULL_HANDLE;
   }

   const VkPipelineShaderStageNirCreateInfoMESA vnir = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = render->vs,
   };
   const VkPipelineShaderStageNirCreateInfoMESA fnir = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = render->fs,
   };
   /* pName must be the NIR entry point's own name (vk_pipeline.c, get_builtin_nir), which
    * is "main" for both: nir_builder_init_simple_shader and air_to_nir both call it that.
    */
   const VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = &vnir,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = VK_NULL_HANDLE,
         .pName = nir_shader_get_entrypoint(render->vs)->function->name,
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = &fnir,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = VK_NULL_HANDLE,
         .pName = nir_shader_get_entrypoint(render->fs)->function->name,
      },
   };
   /* THE DRAW PATH (M5a), VERTEX INPUT: what build_vertex_state made, all of it empty for a
    * stage that fetches nothing, which is what every pipeline was before. */
   const struct rwa_vertex_state *vst = &render->vstate;
   const VkPipelineVertexInputDivisorStateCreateInfoKHR vdiv = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR,
      .vertexBindingDivisorCount = vst->divisors,
      .pVertexBindingDivisors = vst->divisor,
   };
   const VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .pNext = vst->divisors ? &vdiv : NULL,
      .vertexBindingDescriptionCount = vst->bindings,
      .pVertexBindingDescriptions = vst->binding,
      .vertexAttributeDescriptionCount = vst->attributes,
      .pVertexAttributeDescriptions = vst->attribute_vk,
   };
   /* THE DRAW PATH (M5a), INDEXED DRAWS: Metal's primitive restart is not a switch. On the two
    * strip types the largest value of the index type always restarts, on the three list types
    * it is an ordinary index, and Vulkan's own rule is that a list cannot enable it without
    * primitiveTopologyListRestart: this is one line, and the pipeline being per topology is
    * what makes it one. It reads only for an indexed draw, in Vulkan as in Metal. */
   const bool strip = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                      topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   const VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = topology,
      .primitiveRestartEnable = strip ? VK_TRUE : VK_FALSE,
   };
   const VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
   };
   /* Metal's defaults: nothing culled, so the winding the flipped viewport reverses
    * decides nothing. depthClampEnable and depthBiasEnable stay off (M3c-2): the contract
    * carries no depth clip mode and no depth bias, and air_metal.c's rasterizer is the
    * same -- depth_clip_near/far on, no offset (DEPTH AND STENCIL above). */
   /* [M5a: nothing culled and this winding, Metal's default, are only what a device without
    * Vulkan 1.3's dynamic cull and winding is left with; on any other both are set at every
    * draw, from the encoder, and the flipped viewport does NOT reverse them, whatever the
    * sentence above says (THE DRAW PATH, ORIENTATION). A LINE gets the rule Gallium's rasterizer programs on this
    * same GPU, fd6_rasterizer.cc's `multisample ? RECTANGULAR : BRESENHAM`, where Vulkan's
    * default on turnip is RECTANGULAR always: a picture of lines cannot be compared otherwise.
    * A device without VK_KHR_line_rasterization draws its default. Metal does not say which
    * of the two a line is; what --draw-vertex --pixels shows is written beside its result.] */
   const bool is_line = topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                        topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
   const VkPipelineRasterizationLineStateCreateInfoKHR line_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR,
      .lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR,
   };
   const VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .pNext = is_line && device->bresenham && render->samples == 1 ? &line_state : NULL,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   /* The pipeline's sample count (MULTISAMPLE, THE PASS): Metal's sampleCount, 1 unless a
    * pipeline asked for more. No sample shading (a shader that reads [[sample_id]] turns it
    * on by itself, as SampleId does in SPIR-V), no alpha-to-coverage and every sample in
    * the mask -- Metal's alphaToCoverageEnabled and the sample mask a shader may store are
    * not in rwa_render_shape, and their defaults are these. */
   const VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = render->sample_bit ? render->sample_bit : VK_SAMPLE_COUNT_1_BIT,
   };
   /* [M5a: the attachments' blend state is what blend_state_from_metal made of the shape, and
    * the constant is dynamic, set at every draw, because Metal keeps it on the encoder.] */
   const VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      /* M5d: Metal's [[color(n)]] sees what the primitives before this one left (FRAMEBUFFER FETCH) */
      .flags = render->fetch_targets
                  ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT
                  : 0,
      .attachmentCount = render->shape.color_count,
      .pAttachments = render->blend,
   };
   /* The depth/stencil state is DYNAMIC (DEPTH AND STENCIL, THE STATE), and only in a
    * pipeline with a depth attachment: a pipeline without one declares none of it, so a
    * draw without one sets none of it, exactly as before M3c-2. The eight states are set
    * at every draw of such a pipeline (rwa_render_draw), which Vulkan requires of every
    * dynamic state a pipeline declares. */
   const bool zs = render->zs_aspects != 0;
   /* [M5a: and, in every pipeline, what Metal keeps on the encoder and a draw sets every time:
    * the blend constant, and the cull mode and the winding where the device has them
    * dynamic. They are dynamic rather than one more key per pipeline because that is what
    * they are in Metal: one pipeline, set anew per draw.] */
   VkDynamicState dyn[16] = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
   };
   unsigned dyn_count = 3;
   if (device->CmdSetCullMode && device->CmdSetFrontFace) {
      dyn[dyn_count++] = VK_DYNAMIC_STATE_CULL_MODE;
      dyn[dyn_count++] = VK_DYNAMIC_STATE_FRONT_FACE;
   }
   if (zs) {
      static const VkDynamicState zs_states[] = {
         VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
         VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
         VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
         VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
      };
      for (unsigned i = 0; i < ARRAY_SIZE(zs_states); i++)
         dyn[dyn_count++] = zs_states[i];
   }
   const VkPipelineDynamicStateCreateInfo ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = dyn_count,
      .pDynamicStates = dyn,
   };
   /* What the static half says, which is only what nothing dynamic covers: no depth
    * bounds test (Metal has none). The rest is overwritten at every draw. */
   const VkPipelineDepthStencilStateCreateInfo dss = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
      .depthBoundsTestEnable = VK_FALSE,
      .front = { .compareOp = VK_COMPARE_OP_ALWAYS },
      .back = { .compareOp = VK_COMPARE_OP_ALWAYS },
      .minDepthBounds = 0.0f,
      .maxDepthBounds = 1.0f,
   };
   /* The attachment's format by aspect: a depth attachment format when it has a depth
    * aspect, a stencil one when it has a stencil aspect, both the same format for
    * Depth32Float_Stencil8 -- whichever of Metal's two properties named it, because the
    * draw renders into every aspect of the one texture (air_metal.h: one attachment of
    * two aspects), and Vulkan requires the pipeline's formats to be the views'. */
   const VkPipelineRenderingCreateInfo ri = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = render->shape.color_count,
      .pColorAttachmentFormats = render->color_format,
      .depthAttachmentFormat = (render->zs_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                                  ? render->zs_format : VK_FORMAT_UNDEFINED,
      .stencilAttachmentFormat = (render->zs_aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
                                    ? render->zs_format : VK_FORMAT_UNDEFINED,
   };
   const VkGraphicsPipelineCreateInfo gci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &ri,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pDepthStencilState = zs ? &dss : NULL,
      .pColorBlendState = &cb,
      .pDynamicState = &ds,
      .layout = render->layout,
   };
   VkPipeline p = VK_NULL_HANDLE;
   VkResult r = device->vk.CreateGraphicsPipelines(device->dev, VK_NULL_HANDLE, 1, &gci, NULL,
                                                   &p);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make the pipeline of %s over %s (%s)",
          render->fstage.name,
          render->generated_vertex ? "the generated vertex stage" : render->vstage.name,
          vk_Result_to_str(r));
      return VK_NULL_HANDLE;
   }
   render->pipeline[primitive] = p;
   return p;
}

/* MTLColorWriteMask -- Red 1 << 3 down to Alpha 1 << 0 -- in Vulkan's bits, which are
 * Gallium's order (R 0x1 .. A 0x8): the nibble reversed, as air_metal.c reverses it.
 */
static VkColorComponentFlags
write_mask_from_metal(unsigned m)
{
   return ((m & 8) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((m & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
          ((m & 2) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((m & 1) ? VK_COLOR_COMPONENT_A_BIT : 0);
}

/*
 * THE DRAW PATH (M5a): BLENDING. air_metal.c's blend_state_from_metal over Vulkan's words,
 * check for check and in its order, so that what is refused and why is the same sentence
 * on both back ends -- with the one difference the contract now makes possible, the four
 * BLEND COLOUR factors, which are real here: the colour is rwa_draw.blend_color and reaches
 * vkCmdSetBlendConstants at every draw. (Gallium still refuses them: its library reads no
 * such field.)
 *
 * What is still refused, with "(M5a)" where it is Vulkan's or this back end's own answer and
 * in Gallium's words where it is Gallium's decision too:
 *
 *   - MTLBlendFactorSource1Color and its three companions (15-18), dual-source blending.
 *     Turnip has the feature (dualSrcBlend, tu_device.cc) and the frontend now keeps the
 *     second number of air.render_target as the variable's data.index, but this contract
 *     has never drawn with a second output on either back end and Gallium refuses it, so
 *     there is no reference to compare a picture with. Refused by name.
 *   - MTLBlendFactorSourceAlphaSaturated as an ALPHA factor: Vulkan's alpha factor is 1,
 *     Metal's one equation says min(As, 1 - Ad), and the a6xx blender is the same hardware
 *     under both back ends. Same reading and same refusal as air_metal.c's.
 *   - Min or Max with a factor other than One: Vulkan ignores the factors for these two
 *     (VK_BLEND_OP_MIN and MAX), Metal's equation multiplies by them first, and the two
 *     agree only at One and One, which is served.
 *   - blending on a pure integer format, and on a format turnip does not call blendable
 *     (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, which is every non-integer colour
 *     format on a6xx: tu_formats.cc), the second one with "(M5a)".
 *   - attachments that blend differently on a device without independentBlend, "(M5a)".
 *
 * A FORMAT WITH NO ALPHA CHANNEL gets air_metal.c's fix-up (util_blend_dst_alpha_to_one):
 * DestinationAlpha is One there and OneMinusDestinationAlpha is Zero. Vulkan's blend
 * equations already say a format with no alpha reads as 1, so this is only saying the same
 * thing in the state, and not trusting what the hardware keeps in those bits.
 *
 * sRGB is the attachment's Vulkan format and nothing else: an _SRGB colour attachment blends
 * in linear space by Vulkan's own rule (the destination decoded, the result encoded), as
 * Metal's does, and the table of formats above maps the three Metal has to it.
 */
static const char *
blend_factor_name(unsigned f)
{
   static const char *const names[] = {
      "MTLBlendFactorZero", "MTLBlendFactorOne",
      "MTLBlendFactorSourceColor", "MTLBlendFactorOneMinusSourceColor",
      "MTLBlendFactorSourceAlpha", "MTLBlendFactorOneMinusSourceAlpha",
      "MTLBlendFactorDestinationColor", "MTLBlendFactorOneMinusDestinationColor",
      "MTLBlendFactorDestinationAlpha", "MTLBlendFactorOneMinusDestinationAlpha",
      "MTLBlendFactorSourceAlphaSaturated",
      "MTLBlendFactorBlendColor", "MTLBlendFactorOneMinusBlendColor",
      "MTLBlendFactorBlendAlpha", "MTLBlendFactorOneMinusBlendAlpha",
      "MTLBlendFactorSource1Color", "MTLBlendFactorOneMinusSource1Color",
      "MTLBlendFactorSource1Alpha", "MTLBlendFactorOneMinusSource1Alpha",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

static const char *
blend_operation_name(unsigned op)
{
   static const char *const names[] = {
      "MTLBlendOperationAdd", "MTLBlendOperationSubtract",
      "MTLBlendOperationReverseSubtract", "MTLBlendOperationMin",
      "MTLBlendOperationMax",
   };
   return op < ARRAY_SIZE(names) ? names[op] : NULL;
}

/* MTLBlendFactor as VkBlendFactor, by table: a cast is right for the first three and wrong
 * for the rest (Vulkan orders SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
 * SrcAlpha..., Metal SourceColor, OneMinusSourceColor, SourceAlpha...). */
static bool
blend_factor_from_metal(unsigned f, bool alpha_channel, VkBlendFactor *out, const char *what,
                        char *why, size_t why_size)
{
   static const VkBlendFactor factor[] = {
      VK_BLEND_FACTOR_ZERO,                     /* MTLBlendFactorZero                  0 */
      VK_BLEND_FACTOR_ONE,                      /* ...One                              1 */
      VK_BLEND_FACTOR_SRC_COLOR,                /* ...SourceColor                      2 */
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,      /* ...OneMinusSourceColor              3 */
      VK_BLEND_FACTOR_SRC_ALPHA,                /* ...SourceAlpha                      4 */
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,      /* ...OneMinusSourceAlpha              5 */
      VK_BLEND_FACTOR_DST_COLOR,                /* ...DestinationColor                 6 */
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,      /* ...OneMinusDestinationColor         7 */
      VK_BLEND_FACTOR_DST_ALPHA,                /* ...DestinationAlpha                 8 */
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,      /* ...OneMinusDestinationAlpha         9 */
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,       /* ...SourceAlphaSaturated            10 */
      VK_BLEND_FACTOR_CONSTANT_COLOR,           /* ...BlendColor                      11 */
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, /* ...OneMinusBlendColor              12 */
      VK_BLEND_FACTOR_CONSTANT_ALPHA,           /* ...BlendAlpha                      13 */
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, /* ...OneMinusBlendAlpha              14 */
   };
   if (f > 18) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendFactor's nineteen values",
          what, f);
      return false;
   }
   if (f >= 15) {
      say(why, why_size, "%s is %s: dual-source blending reads a SECOND fragment output at "
                         "[[color(0), index(1)]], which this contract has never drawn with "
                         "on either back end and Gallium refuses, so it is not in the Vulkan "
                         "back end (M5a)", what, blend_factor_name(f));
      return false;
   }
   if (alpha_channel && f == 10) {
      say(why, why_size, "%s is MTLBlendFactorSourceAlphaSaturated, and the two "
                         "specifications disagree about what that means for the ALPHA "
                         "channel: Vulkan defines the alpha factor as 1 where Metal's own "
                         "equation gives min(As, 1 - Ad) for all four channels. It is served "
                         "for the RGB factors, where they agree (M5a)", what);
      return false;
   }
   *out = factor[f];
   return true;
}

/* MTLBlendOperation as VkBlendOp: Add 0, Subtract 1, ReverseSubtract 2, Min 3, Max 4 on both
 * sides, written out for the reason air_metal.c writes its own out. */
static bool
blend_operation_from_metal(unsigned op, VkBlendOp *out, const char *what, char *why,
                           size_t why_size)
{
   static const VkBlendOp ops[] = {
      VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN,
      VK_BLEND_OP_MAX,
   };
   if (op >= ARRAY_SIZE(ops)) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendOperation's five values",
          what, op);
      return false;
   }
   *out = ops[op];
   return true;
}

static VkBlendFactor
dst_alpha_to_one(VkBlendFactor f)
{
   return f == VK_BLEND_FACTOR_DST_ALPHA ? VK_BLEND_FACTOR_ONE
        : f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ? VK_BLEND_FACTOR_ZERO : f;
}

/*
 * The whole blend state, from the shape's color_blend[] to one Vulkan struct per attachment.
 * `color_blend_count` of 0 is a shape that carries no blend state: blending off, every
 * channel written, which is what this back end drew everything with before M5a. An entry
 * with blending off has only its write mask read, which is Metal's own rule and the
 * difference between a right home screen and a doubly blended one (air_metal.h: 504 of 1079
 * of CoreAnimation's pipelines arrive with One/Zero and blendingEnabled NO).
 */
static bool
blend_state_from_metal(struct rwa_device *device, const struct rwa_render_shape *shape,
                       VkPipelineColorBlendAttachmentState *out, unsigned *enabled_mask,
                       char *why, size_t why_size)
{
   *enabled_mask = 0;
   for (unsigned i = 0; i < shape->color_count && i < 8; i++)
      out[i] = (VkPipelineColorBlendAttachmentState){
         .blendEnable = VK_FALSE,
         .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      };
   if (shape->color_blend_count == 0)
      return true;
   if (shape->color_blend_count > 8) {
      say(why, why_size, "blend state for %u colour attachments, and Metal's descriptor "
                         "has eight", shape->color_blend_count);
      return false;
   }
   for (unsigned i = 0; i < shape->color_count; i++) {
      const struct rwa_color_blend *b = &shape->color_blend[i];
      VkPipelineColorBlendAttachmentState *rt = &out[i];
      char what[160];

      if (b->write_mask > 0xf) {
         say(why, why_size, "colour attachment %u's writeMask is 0x%x, and MTLColorWriteMask "
                            "is four bits (MTLColorWriteMaskAll is 0xf)", i, b->write_mask);
         return false;
      }
      rt->colorWriteMask = write_mask_from_metal(b->write_mask);
      if (!b->blending_enabled)
         continue;

      const struct rwa_vk_format *fmt = vk_format_of(shape->color_format[i]);
      if (fmt && fmt->kind != RWA_KIND_FLOAT) {
         say(why, why_size, "colour attachment %u is %s and this pipeline enables blending on "
                            "it: a blend equation over integers has no defined meaning in "
                            "Metal, in GL or in Vulkan", i, fmt->name);
         return false;
      }
      if (fmt) {
         VkFormatProperties fp;
         device->vk.GetPhysicalDeviceFormatProperties(device->physical, fmt->vk, &fp);
         if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
            say(why, why_size, "colour attachment %u is %s and this pipeline enables blending "
                               "on it: %s does not blend that format (M5a)", i, fmt->name,
                device->name);
            return false;
         }
      }
      rt->blendEnable = VK_TRUE;
      *enabled_mask |= 1u << i;

      snprintf(what, sizeof(what), "colour attachment %u's rgbBlendOperation", i);
      if (!blend_operation_from_metal(b->rgb_blend_operation, &rt->colorBlendOp, what, why,
                                      why_size))
         return false;
      snprintf(what, sizeof(what), "colour attachment %u's alphaBlendOperation", i);
      if (!blend_operation_from_metal(b->alpha_blend_operation, &rt->alphaBlendOp, what, why,
                                      why_size))
         return false;
      snprintf(what, sizeof(what), "colour attachment %u's sourceRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->source_rgb_blend_factor, false, &rt->srcColorBlendFactor,
                                   what, why, why_size))
         return false;
      snprintf(what, sizeof(what), "colour attachment %u's destinationRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->destination_rgb_blend_factor, false,
                                   &rt->dstColorBlendFactor, what, why, why_size))
         return false;
      snprintf(what, sizeof(what), "colour attachment %u's sourceAlphaBlendFactor", i);
      if (!blend_factor_from_metal(b->source_alpha_blend_factor, true, &rt->srcAlphaBlendFactor,
                                   what, why, why_size))
         return false;
      snprintf(what, sizeof(what), "colour attachment %u's destinationAlphaBlendFactor", i);
      if (!blend_factor_from_metal(b->destination_alpha_blend_factor, true,
                                   &rt->dstAlphaBlendFactor, what, why, why_size))
         return false;

      for (unsigned half = 0; half < 2; half++) {
         VkBlendOp op = half ? rt->alphaBlendOp : rt->colorBlendOp;
         VkBlendFactor sf = half ? rt->srcAlphaBlendFactor : rt->srcColorBlendFactor;
         VkBlendFactor df = half ? rt->dstAlphaBlendFactor : rt->dstColorBlendFactor;
         unsigned msf = half ? b->source_alpha_blend_factor : b->source_rgb_blend_factor;
         unsigned mdf = half ? b->destination_alpha_blend_factor
                             : b->destination_rgb_blend_factor;
         if ((op != VK_BLEND_OP_MIN && op != VK_BLEND_OP_MAX) ||
             (sf == VK_BLEND_FACTOR_ONE && df == VK_BLEND_FACTOR_ONE))
            continue;
         say(why, why_size, "colour attachment %u's %sBlendOperation is %s and its factors "
                            "are %s and %s: Metal's equation multiplies by the factors and "
                            "then takes the minimum or maximum, Vulkan ignores the factors "
                            "for these two operations entirely, and the two readings differ "
                            "unless both factors are MTLBlendFactorOne -- which is served, "
                            "and is the only form a maximum-blend is written in", i,
             half ? "alpha" : "rgb",
             blend_operation_name(half ? b->alpha_blend_operation : b->rgb_blend_operation),
             blend_factor_name(msf), blend_factor_name(mdf));
         return false;
      }

      /* the format decides whether there is an alpha to read; only these two words do */
      if (fmt && !strstr(fmt->name, "RGBA") && !strstr(fmt->name, "BGRA")) {
         rt->srcColorBlendFactor = dst_alpha_to_one(rt->srcColorBlendFactor);
         rt->dstColorBlendFactor = dst_alpha_to_one(rt->dstColorBlendFactor);
         rt->srcAlphaBlendFactor = dst_alpha_to_one(rt->srcAlphaBlendFactor);
         rt->dstAlphaBlendFactor = dst_alpha_to_one(rt->dstAlphaBlendFactor);
      }
   }
   bool differ = false;
   for (unsigned i = 1; i < shape->color_count; i++)
      differ = differ || memcmp(&out[i], &out[0], sizeof(out[0])) != 0;
   if (differ && !device->independent_blend) {
      say(why, why_size, "this pipeline's %u colour attachments do not all ask for the same "
                         "blend state, and %s has no independentBlend: the others would "
                         "silently blend with the first one's equation (M5a)",
          shape->color_count, device->name);
      return false;
   }
   return true;
}

/*
 * THE DRAW PATH (M5a): VERTEX INPUT. MTLVertexFormat in Vulkan's terms, row for row what
 * air_metal.c's rwa_vertex_formats says in Gallium's, and what each delivers to the
 * register, which is what has to match the member's declared type.
 *
 * WHAT IS DIFFERENT FROM GALLIUM, and it is what makes this shorter: Vulkan matches an
 * attribute to a shader input by LOCATION, and turnip reads the location off the input's own
 * slot (tu_shader.cc, tu6_emit_vs_input: inputs[i].slot - VERT_ATTRIB_GENERIC0, the form the
 * frontend gives it). So location is Metal's [[attribute(n)]] verbatim and binding is Metal's
 * buffer index verbatim, unused attributes need no thought, and the driver-location order
 * air_metal.c's long comment is about is not an order here. It is still reported, in
 * rwa_render_info, because that is what the field means on both back ends.
 *
 * WHAT IS REFUSED: everything air_metal.c refuses -- the packed formats (40, 41, and 54 and
 * 55, which this SDK has not got), a type that does not match, a tessellation step function,
 * per-vertex at a rate other than one, a stride of 0 outside Constant, an element past its
 * stride, a stream index the module also reads as [[buffer(n)]], a descriptor over the
 * generated stage -- in its words; and with "(M5a)" what Vulkan or turnip adds: a stride over
 * maxVertexInputBindingStride, which is 2048 on turnip where Gallium serves up to 4095 (a
 * limit of turnip's report rather than of the fetch, VFD_FETCH_STRIDE is the same register,
 * and a real application's element is nowhere near either); an offset over
 * maxVertexInputAttributeOffset; a buffer index past maxVertexInputBindings; a format turnip
 * will not fetch; and a step rate above one on a device without the divisor extension.
 */
struct rwa_vk_vertex_format {
   unsigned mtl;
   const char *name;
   VkFormat vk;
   enum air_attribute_base_type delivers;
   unsigned bytes;
};

static const struct rwa_vk_vertex_format rwa_vk_vertex_formats[] = {
   {  1, "MTLVertexFormatUChar2", VK_FORMAT_R8G8_UINT, AIR_ATTRIB_UINT, 2 },
   {  2, "MTLVertexFormatUChar3", VK_FORMAT_R8G8B8_UINT, AIR_ATTRIB_UINT, 3 },
   {  3, "MTLVertexFormatUChar4", VK_FORMAT_R8G8B8A8_UINT, AIR_ATTRIB_UINT, 4 },
   {  4, "MTLVertexFormatChar2", VK_FORMAT_R8G8_SINT, AIR_ATTRIB_INT, 2 },
   {  5, "MTLVertexFormatChar3", VK_FORMAT_R8G8B8_SINT, AIR_ATTRIB_INT, 3 },
   {  6, "MTLVertexFormatChar4", VK_FORMAT_R8G8B8A8_SINT, AIR_ATTRIB_INT, 4 },
   {  7, "MTLVertexFormatUChar2Normalized", VK_FORMAT_R8G8_UNORM, AIR_ATTRIB_FLOAT, 2 },
   {  8, "MTLVertexFormatUChar3Normalized", VK_FORMAT_R8G8B8_UNORM, AIR_ATTRIB_FLOAT, 3 },
   {  9, "MTLVertexFormatUChar4Normalized", VK_FORMAT_R8G8B8A8_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 10, "MTLVertexFormatChar2Normalized", VK_FORMAT_R8G8_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 11, "MTLVertexFormatChar3Normalized", VK_FORMAT_R8G8B8_SNORM, AIR_ATTRIB_FLOAT, 3 },
   { 12, "MTLVertexFormatChar4Normalized", VK_FORMAT_R8G8B8A8_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 13, "MTLVertexFormatUShort2", VK_FORMAT_R16G16_UINT, AIR_ATTRIB_UINT, 4 },
   { 14, "MTLVertexFormatUShort3", VK_FORMAT_R16G16B16_UINT, AIR_ATTRIB_UINT, 6 },
   { 15, "MTLVertexFormatUShort4", VK_FORMAT_R16G16B16A16_UINT, AIR_ATTRIB_UINT, 8 },
   { 16, "MTLVertexFormatShort2", VK_FORMAT_R16G16_SINT, AIR_ATTRIB_INT, 4 },
   { 17, "MTLVertexFormatShort3", VK_FORMAT_R16G16B16_SINT, AIR_ATTRIB_INT, 6 },
   { 18, "MTLVertexFormatShort4", VK_FORMAT_R16G16B16A16_SINT, AIR_ATTRIB_INT, 8 },
   { 19, "MTLVertexFormatUShort2Normalized", VK_FORMAT_R16G16_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 20, "MTLVertexFormatUShort3Normalized", VK_FORMAT_R16G16B16_UNORM, AIR_ATTRIB_FLOAT, 6 },
   { 21, "MTLVertexFormatUShort4Normalized", VK_FORMAT_R16G16B16A16_UNORM, AIR_ATTRIB_FLOAT, 8 },
   { 22, "MTLVertexFormatShort2Normalized", VK_FORMAT_R16G16_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 23, "MTLVertexFormatShort3Normalized", VK_FORMAT_R16G16B16_SNORM, AIR_ATTRIB_FLOAT, 6 },
   { 24, "MTLVertexFormatShort4Normalized", VK_FORMAT_R16G16B16A16_SNORM, AIR_ATTRIB_FLOAT, 8 },
   { 25, "MTLVertexFormatHalf2", VK_FORMAT_R16G16_SFLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 26, "MTLVertexFormatHalf3", VK_FORMAT_R16G16B16_SFLOAT, AIR_ATTRIB_FLOAT, 6 },
   { 27, "MTLVertexFormatHalf4", VK_FORMAT_R16G16B16A16_SFLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 28, "MTLVertexFormatFloat", VK_FORMAT_R32_SFLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 29, "MTLVertexFormatFloat2", VK_FORMAT_R32G32_SFLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 30, "MTLVertexFormatFloat3", VK_FORMAT_R32G32B32_SFLOAT, AIR_ATTRIB_FLOAT, 12 },
   { 31, "MTLVertexFormatFloat4", VK_FORMAT_R32G32B32A32_SFLOAT, AIR_ATTRIB_FLOAT, 16 },
   { 32, "MTLVertexFormatInt", VK_FORMAT_R32_SINT, AIR_ATTRIB_INT, 4 },
   { 33, "MTLVertexFormatInt2", VK_FORMAT_R32G32_SINT, AIR_ATTRIB_INT, 8 },
   { 34, "MTLVertexFormatInt3", VK_FORMAT_R32G32B32_SINT, AIR_ATTRIB_INT, 12 },
   { 35, "MTLVertexFormatInt4", VK_FORMAT_R32G32B32A32_SINT, AIR_ATTRIB_INT, 16 },
   { 36, "MTLVertexFormatUInt", VK_FORMAT_R32_UINT, AIR_ATTRIB_UINT, 4 },
   { 37, "MTLVertexFormatUInt2", VK_FORMAT_R32G32_UINT, AIR_ATTRIB_UINT, 8 },
   { 38, "MTLVertexFormatUInt3", VK_FORMAT_R32G32B32_UINT, AIR_ATTRIB_UINT, 12 },
   { 39, "MTLVertexFormatUInt4", VK_FORMAT_R32G32B32A32_UINT, AIR_ATTRIB_UINT, 16 },
   { 42, "MTLVertexFormatUChar4Normalized_BGRA", VK_FORMAT_B8G8R8A8_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 45, "MTLVertexFormatUChar", VK_FORMAT_R8_UINT, AIR_ATTRIB_UINT, 1 },
   { 46, "MTLVertexFormatChar", VK_FORMAT_R8_SINT, AIR_ATTRIB_INT, 1 },
   { 47, "MTLVertexFormatUCharNormalized", VK_FORMAT_R8_UNORM, AIR_ATTRIB_FLOAT, 1 },
   { 48, "MTLVertexFormatCharNormalized", VK_FORMAT_R8_SNORM, AIR_ATTRIB_FLOAT, 1 },
   { 49, "MTLVertexFormatUShort", VK_FORMAT_R16_UINT, AIR_ATTRIB_UINT, 2 },
   { 50, "MTLVertexFormatShort", VK_FORMAT_R16_SINT, AIR_ATTRIB_INT, 2 },
   { 51, "MTLVertexFormatUShortNormalized", VK_FORMAT_R16_UNORM, AIR_ATTRIB_FLOAT, 2 },
   { 52, "MTLVertexFormatShortNormalized", VK_FORMAT_R16_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 53, "MTLVertexFormatHalf", VK_FORMAT_R16_SFLOAT, AIR_ATTRIB_FLOAT, 2 },
};

static const char *
vertex_format_refusal(unsigned f)
{
   switch (f) {
   case 0:
      return "MTLVertexFormatInvalid, which is what an unconfigured attribute holds";
   case 40:
      return "MTLVertexFormatInt1010102Normalized: four channels packed in one word, declined "
             "as on Gallium because which end Metal's channels count from has not been "
             "measured on this GPU, and a mistake shifts a channel in a picture that still "
             "looks like a picture (M5a)";
   case 41:
      return "MTLVertexFormatUInt1010102Normalized: the same packed word as 40, declined for "
             "the same unmeasured channel order (M5a)";
   case 54:
      return "MTLVertexFormatFloatRG11B10, which the iOS 16.5 SDK's MTLVertexFormat has not "
             "got (it ends at Half, 53)";
   case 55:
      return "MTLVertexFormatFloatRGB9E5, which the iOS 16.5 SDK's MTLVertexFormat has not "
             "got, and which is texture-only on a6xx";
   case 43: case 44:
      return "a gap in Apple's own MTLVertexFormat enumeration";
   default:
      return "not a value of MTLVertexFormat";
   }
}

static const char *
attribute_type_name(enum air_attribute_base_type t)
{
   switch (t) {
   case AIR_ATTRIB_FLOAT: return "floating-point";
   case AIR_ATTRIB_INT:   return "signed integer";
   case AIR_ATTRIB_UINT:  return "unsigned integer";
   }
   return "?";
}

/* Where a gated, unconfigured attribute reads (below): one past Metal's highest buffer index,
 * so it cannot collide with a stream or an argument. air_metal.c uses the same slot. */
#define RWA_VK_PLACEHOLDER_BINDING RWA_MAX_VERTEX_BUFFERS

/*
 * The tables of one pipeline, from the module's own interface and the descriptor's layout,
 * in air_metal.c's build_vertex_state's order and sentences.
 *
 * An attribute gated by a function-constant predicate that the descriptor leaves
 * unconfigured still has a shader input (the parameter exists whatever the predicate says),
 * and Vulkan requires a fetch for every input the stage declares. So it gets what it gets
 * on Gallium: a placeholder fetched at stride 0 from sixteen bytes this library owns, here
 * in the format its declared type needs -- Vulkan requires the numeric type to match, where
 * Gallium fetches float always -- and the shader, whose predicate is false, never looks.
 */
static bool
build_vertex_state(struct rwa_device *device, const struct air_shader_info *vinfo,
                   const struct rwa_render_shape *shape, const char *vname,
                   struct rwa_vertex_state *out, char *why, size_t why_size)
{
   static const char *const step_names[] = {
      "MTLVertexStepFunctionConstant", "MTLVertexStepFunctionPerVertex",
      "MTLVertexStepFunctionPerInstance", "MTLVertexStepFunctionPerPatch",
      "MTLVertexStepFunctionPerPatchControlPoint",
   };
   /* what each stream has been made, so that two attributes of one buffer index make one
    * binding; indexed by that index, and one past Metal's for the placeholder's */
   bool made[RWA_MAX_VERTEX_BUFFERS + 1] = { false };

   memset(out, 0, sizeof(*out));

   for (unsigned i = 0; i < vinfo->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &vinfo->vertex_attributes[i];

      if (!a->used)
         continue;
      if (a->index >= RWA_MAX_VERTEX_ATTRIBUTES) {
         say(why, why_size, "%s reads vertex attribute %u and Metal has %u", vname, a->index,
             RWA_MAX_VERTEX_ATTRIBUTES);
         return false;
      }
      if (a->driver_location >= ARRAY_SIZE(out->attribute)) {
         say(why, why_size, "%s puts vertex attribute %u at driver location %u, past the %u "
                            "NIR has generic slots for", vname, a->index, a->driver_location,
             (unsigned)ARRAY_SIZE(out->attribute));
         return false;
      }

      const struct rwa_vertex_attribute *d = &shape->vertex_attribute[a->index];
      unsigned loc = a->driver_location;

      out->count = MAX2(out->count, loc + 1);
      out->attribute[loc] = a->index;

      if (d->format == 0) {
         if (!a->function_constant) {
            say(why, why_size, "%s reads vertex attribute %u (%s, %s) and the vertex "
                               "descriptor configures no format there, so the fetch would "
                               "read a slot nothing was set up for", vname, a->index,
                a->name ? a->name : "unnamed", a->type_name ? a->type_name : "?");
            return false;
         }
         if (device->max_vi_bindings <= RWA_VK_PLACEHOLDER_BINDING) {
            say(why, why_size, "%s reads vertex attribute %u, gated by a function constant and "
                               "with nothing configured, and this back end fetches such an "
                               "attribute from binding %u, past the %u %s has (M5a)", vname,
                a->index, RWA_VK_PLACEHOLDER_BINDING, device->max_vi_bindings, device->name);
            return false;
         }
         out->attribute_vk[out->attributes++] = (VkVertexInputAttributeDescription){
            .location = a->index,
            .binding = RWA_VK_PLACEHOLDER_BINDING,
            .format = a->base_type == AIR_ATTRIB_FLOAT ? VK_FORMAT_R32G32B32A32_SFLOAT
                    : a->base_type == AIR_ATTRIB_INT ? VK_FORMAT_R32G32B32A32_SINT
                                                     : VK_FORMAT_R32G32B32A32_UINT,
            .offset = 0,
         };
         if (!made[RWA_VK_PLACEHOLDER_BINDING]) {
            made[RWA_VK_PLACEHOLDER_BINDING] = true;
            out->binding[out->bindings++] = (VkVertexInputBindingDescription){
               .binding = RWA_VK_PLACEHOLDER_BINDING, .stride = 0,
               .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
         }
         out->placeholder = true;
         continue;
      }

      const struct rwa_vk_vertex_format *f = NULL;
      for (unsigned k = 0; k < ARRAY_SIZE(rwa_vk_vertex_formats); k++)
         if (rwa_vk_vertex_formats[k].mtl == d->format)
            f = &rwa_vk_vertex_formats[k];
      if (!f) {
         say(why, why_size, "vertex attribute %u of %s: format %u is %s", a->index, vname,
             d->format, vertex_format_refusal(d->format));
         return false;
      }
      if (f->delivers != a->base_type) {
         say(why, why_size, "vertex attribute %u of %s is %s, which wants %s components, and "
                            "%s delivers %s ones", a->index, vname,
             a->type_name ? a->type_name : (a->name ? a->name : "unnamed"),
             attribute_type_name(a->base_type), f->name, attribute_type_name(f->delivers));
         return false;
      }
      VkFormatProperties fp;
      device->vk.GetPhysicalDeviceFormatProperties(device->physical, f->vk, &fp);
      if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
         say(why, why_size, "%s cannot fetch %s as a vertex attribute (M5a)", device->name,
             f->name);
         return false;
      }
      if (d->buffer_index >= RWA_MAX_VERTEX_BUFFERS) {
         say(why, why_size, "vertex attribute %u of %s is fetched from buffer index %u, and "
                            "Metal has %u", a->index, vname, d->buffer_index,
             RWA_MAX_VERTEX_BUFFERS);
         return false;
      }
      if (d->buffer_index >= device->max_vi_bindings) {
         say(why, why_size, "vertex attribute %u of %s is fetched from buffer index %u, and "
                            "%s has %u vertex input bindings (M5a)", a->index, vname,
             d->buffer_index, device->name, device->max_vi_bindings);
         return false;
      }
      if (d->offset > device->max_vi_offset) {
         say(why, why_size, "vertex attribute %u of %s is at offset %u, past the %u %s takes "
                            "(maxVertexInputAttributeOffset, M5a)", a->index, vname, d->offset,
             device->max_vi_offset, device->name);
         return false;
      }

      const struct rwa_vertex_layout *l = &shape->vertex_layout[d->buffer_index];
      unsigned rate = l->step_rate ? l->step_rate : 1;
      unsigned stride = l->stride;
      VkVertexInputRate input = VK_VERTEX_INPUT_RATE_VERTEX;

      if (l->step_function >= ARRAY_SIZE(step_names)) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor has step function %u, "
                            "which is not one of MTLVertexStepFunction's five",
             d->buffer_index, vname, l->step_function);
         return false;
      }
      switch (l->step_function) {
      case 0:   /* Constant: every vertex and every instance reads element 0 */
         stride = 0;
         rate = 1;
         break;
      case 1:   /* PerVertex */
         if (rate != 1) {
            say(why, why_size, "buffer index %u of %s's vertex descriptor steps per vertex at "
                               "a rate of %u: Vulkan has a divisor for instances and nothing "
                               "to express a rate over vertices with", d->buffer_index, vname,
                rate);
            return false;
         }
         out->per_vertex |= 1u << d->buffer_index;
         break;
      case 2:   /* PerInstance: stepRate is Vulkan's divisor exactly */
         input = VK_VERTEX_INPUT_RATE_INSTANCE;
         if (rate != 1 && !device->divisors) {
            say(why, why_size, "buffer index %u of %s's vertex descriptor steps per instance "
                               "at a rate of %u, and %s has no vertex attribute divisor "
                               "(M5a)", d->buffer_index, vname, rate, device->name);
            return false;
         }
         break;
      default:
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s, which "
                            "is tessellation: there is no tessellation stage in this port",
             d->buffer_index, vname, step_names[l->step_function]);
         return false;
      }
      if (stride > device->max_vi_stride) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor has a stride of %u "
                            "bytes, past the %u %s takes (maxVertexInputBindingStride, M5a)",
             d->buffer_index, vname, stride, device->max_vi_stride, device->name);
         return false;
      }
      if (l->step_function != 0 && stride == 0) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s and has "
                            "a stride of 0, so every vertex would read the same element: "
                            "that is what %s is for", d->buffer_index, vname,
             step_names[l->step_function], step_names[0]);
         return false;
      }
      if (stride && (size_t)d->offset + f->bytes > stride) {
         say(why, why_size, "vertex attribute %u of %s is %s at offset %u of a %u-byte element, "
                            "so it reads %u bytes of the next vertex", a->index, vname, f->name,
             d->offset, stride, (unsigned)((size_t)d->offset + f->bytes - stride));
         return false;
      }

      out->attribute_vk[out->attributes++] = (VkVertexInputAttributeDescription){
         .location = a->index,
         .binding = d->buffer_index,
         .format = f->vk,
         .offset = d->offset,
      };
      if (!made[d->buffer_index]) {
         made[d->buffer_index] = true;
         out->binding[out->bindings++] = (VkVertexInputBindingDescription){
            .binding = d->buffer_index, .stride = stride, .inputRate = input,
         };
         if (input == VK_VERTEX_INPUT_RATE_INSTANCE && rate != 1)
            out->divisor[out->divisors++] = (VkVertexInputBindingDivisorDescriptionKHR){
               .binding = d->buffer_index, .divisor = rate,
            };
      }
      out->stream_mask |= 1u << d->buffer_index;
      out->stream_count = MAX2(out->stream_count, d->buffer_index + 1);
      out->stride[d->buffer_index] = stride;
      out->bytes[d->buffer_index] = MAX2(out->bytes[d->buffer_index], d->offset + f->bytes);
   }

   /* Metal's one buffer table: an index is a stream or an argument, never both. */
   for (unsigned i = 0; i < vinfo->num_resources; i++) {
      const struct air_resource *r = &vinfo->resources[i];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER)
         continue;
      if (r->index < RWA_MAX_VERTEX_BUFFERS && (out->stream_mask & (1u << r->index))) {
         say(why, why_size, "buffer index %u is a vertex stream in this descriptor and %s "
                            "reads it as [[buffer(%u)]] (%s): Metal has one table behind "
                            "-setVertexBuffer:offset:atIndex: and an index can be a stream or "
                            "an argument, not both", r->index, vname, r->index,
             r->name ? r->name : "unnamed");
         return false;
      }
   }
   return true;
}

/* The two readings of "does this stage fetch anything", air_metal.c's cross-check. */
static bool
vertex_reads_attributes(nir_shader *vs)
{
   nir_foreach_variable_with_modes(var, vs, nir_var_shader_in)
      if (var->data.location >= VERT_ATTRIB_GENERIC0 && var->data.location < VERT_ATTRIB_MAX)
         return true;
   return false;
}

/* MTLIndexType, by table: UInt16 0, UInt32 1, and nothing else is one of them. */
static bool
index_type_from_metal(unsigned type, unsigned *size, VkIndexType *vk, char *why, size_t why_size)
{
   switch (type) {
   case 0:
      *size = 2;
      *vk = VK_INDEX_TYPE_UINT16;
      return true;
   case 1:
      *size = 4;
      *vk = VK_INDEX_TYPE_UINT32;
      return true;
   }
   say(why, why_size, "index type %u is neither MTLIndexTypeUInt16 (0) nor MTLIndexTypeUInt32 "
                      "(1), which is all MTLIndexType has", type);
   return false;
}

/*
 * The lowest and highest of `count` indices, as air_metal.c's index_range, with one thing
 * added, which is the difference between the two back ends and it is Metal's: on the two
 * STRIP types the largest value of the index type is a RESTART and not a vertex, so it is
 * not a vertex to bound (THE DRAW PATH, INDEXED DRAWS). Nothing but restarts reads as 0..0.
 */
static void
index_range(const uint8_t *indices, unsigned count, unsigned index_size, bool restart,
            unsigned *min_out, unsigned *max_out)
{
   unsigned lo = UINT32_MAX, hi = 0;
   for (unsigned i = 0; i < count; i++) {
      unsigned v;
      if (index_size == 2) {
         uint16_t u;
         memcpy(&u, indices + 2 * (size_t)i, 2);
         if (restart && u == 0xffff)
            continue;
         v = u;
      } else {
         uint32_t u;
         memcpy(&u, indices + 4 * (size_t)i, 4);
         if (restart && u == 0xffffffffu)
            continue;
         v = u;
      }
      lo = MIN2(lo, v);
      hi = MAX2(hi, v);
   }
   *min_out = lo == UINT32_MAX && hi == 0 ? 0 : lo;
   *max_out = hi;
}

/* What a stage binds, counted as air_metal.c's count_resources counts it for
 * rwa_render_info: the highest index of each kind, plus one, the shader's own samplers
 * not among them (nothing for a driver to bind there).
 */
static void
count_stage(const struct rwa_stage *st, unsigned *buffers, unsigned *textures,
            unsigned *samplers)
{
   for (unsigned i = 0; i < st->count; i++) {
      const struct rwa_binding *b = &st->b[i];
      if (b->kind == RWA_BIND_BUFFER && buffers)
         *buffers = MAX2(*buffers, b->index + 1);
      else if (b->kind == RWA_BIND_TEXTURE && textures)
         *textures = MAX2(*textures, b->index + 1);
      else if (b->kind == RWA_BIND_SAMPLER && !b->inline_sampler && samplers)
         *samplers = MAX2(*samplers, b->index + 1);
   }
}

/*
 * FRAMEBUFFER FETCH (M5d). Metal's [[color(n)]] argument, what render target n holds now. The frontend
 * makes it load_output with io_semantics.fb_fetch_output, nir_lower_io's form of a GLSL `inout`
 * (air_to_nir.c), which Gallium's ir3 turns into txf_ms_fb. Turnip has no use for that form
 * (bindless_fb_read_descriptor is -1 in tu_device.cc): what it has is Vulkan's, a subpass input.
 * So each read becomes what spirv_to_nir makes of OpImageRead on a SubpassData image with
 * InputAttachmentIndex n: image_deref_load of a variable whose data.index is n, which
 * nir_lower_input_attachments makes a txf at the fragment's own position.
 *
 * The pass is dynamic rendering, and the attachment is one being rendered to, which is
 * VK_KHR_dynamic_rendering_local_read: with no VkRenderingInputAttachmentIndexInfoKHR, input
 * attachment n IS colour attachment n, and tu_shader.cc's build_bindless reads it by turnip's
 * own "non bindless workaround", from the descriptors tu_emit_input_attachments writes from
 * the pass's attachments. That is why there is a binding in the layout (it asks the layout what
 * type this is) and never a descriptor written.
 *
 * Coherent, which is Metal's guarantee: an overlap in one draw reads what the primitive before it
 * wrote. Vulkan's word for that is RASTERIZATION_ORDER_ATTACHMENT_ACCESS on the pipeline, and for
 * turnip that is FLUSH_PER_OVERLAP in the tile buffer and FLUSH_PER_OVERLAP_AND_OVERWRITE in
 * system memory (tu6_emit_prim_mode_sysmem, tu_pipeline_builder_parse_rasterization_order):
 * what Gallium's fd6_emit.cc does for the same flag.
 */
static void
lower_fbfetch(struct rwa_render *render)
{
   nir_shader *nir = render->fs;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);
   nir_variable *vars[8] = { NULL };

   render->fetch_targets = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_load_output ||
             !nir_intrinsic_io_semantics(in).fb_fetch_output)
            continue;
         unsigned n = nir_intrinsic_io_semantics(in).location - FRAG_RESULT_DATA0;
         if (!vars[n]) {
            vars[n] = nir_variable_create(nir, nir_var_image,
                                          glsl_image_type(GLSL_SAMPLER_DIM_SUBPASS, false,
                                                          GLSL_TYPE_FLOAT),
                                          "air_color_input");
            vars[n]->data.descriptor_set = 0;
            vars[n]->data.binding = RWA_BIND_FETCH_BASE + n;
            vars[n]->data.index = n;
            vars[n]->data.image.format = PIPE_FORMAT_NONE;
            render->fetch_targets |= 1u << n;
         }
         b.cursor = nir_before_instr(instr);
         nir_deref_instr *deref = nir_build_deref_var(&b, vars[n]);
         nir_def *value = nir_image_deref_load(&b, 4, 32, &deref->def, nir_imm_ivec4(&b, 0, 0, 0, 0),
                                               nir_undef(&b, 1, 32), nir_imm_int(&b, 0),
                                               .image_dim = GLSL_SAMPLER_DIM_SUBPASS,
                                               .dest_type = nir_type_float32);
         nir_def_rewrite_uses(&in->def, value);
         nir_instr_remove(instr);
      }
   }
   nir_metadata_preserve(impl, nir_metadata_none);
   nir->info.outputs_read = 0;
   nir->info.fs.uses_fbfetch_output = false;
   nir->info.fs.fbfetch_coherent = false;
}

/* The one push set's layout, out of both stages' bindings (RESOURCES), and the pipeline
 * layout around it; the device's empty layout when neither stage binds anything.
 */
static bool
make_layouts(struct rwa_render *render, char *why, size_t why_size)
{
   struct rwa_device *device = render->device;
   unsigned total = render->vstage.count + render->fstage.count +
                    util_bitcount(render->fetch_targets);
   if (total == 0) {
      render->layout = device->empty_layout;
      return true;
   }
   /* [M5c: past maxPushDescriptors, an ordinary set per draw (ARGUMENT BUFFERS, MORE THAN A PUSH
    * SET HOLDS); this said "a second, ordinary set is not in the Vulkan back end yet (M3b)"] */
   render->ordinary_set =
      stage_descriptors(&render->vstage) + stage_descriptors(&render->fstage) > device->max_push;
   VkDescriptorSetLayoutBinding *lb = ralloc_array(render->mem_ctx,
                                                   VkDescriptorSetLayoutBinding, total);
   if (!lb) {
      say(why, why_size, "out of memory making a descriptor set layout");
      return false;
   }
   unsigned n = 0;
   const struct rwa_stage *stages[2] = { &render->vstage, &render->fstage };
   for (unsigned s = 0; s < 2; s++)
      for (unsigned i = 0; i < stages[s]->count; i++)
         lb[n++] = (VkDescriptorSetLayoutBinding){
            .binding = stages[s]->b[i].binding,
            .descriptorType = stages[s]->b[i].type,
            .descriptorCount = stages[s]->b[i].span,
            .stageFlags = stages[s]->flags,
         };
   u_foreach_bit(t, render->fetch_targets)
      lb[n++] = (VkDescriptorSetLayoutBinding){
         .binding = RWA_BIND_FETCH_BASE + t,
         .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };
   const VkDescriptorSetLayoutCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = render->ordinary_set ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = n,
      .pBindings = lb,
   };
   VkResult r = device->vk.CreateDescriptorSetLayout(device->dev, &sci, NULL,
                                                     &render->set_layout);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make the push descriptor set layout of %u "
                         "bindings (%s)", n, vk_Result_to_str(r));
      render->set_layout = VK_NULL_HANDLE;
      return false;
   }
   const VkPipelineLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &render->set_layout,
   };
   if ((r = device->vk.CreatePipelineLayout(device->dev, &lci, NULL, &render->layout)) !=
       VK_SUCCESS) {
      say(why, why_size, "turnip would not make a pipeline layout (%s)", vk_Result_to_str(r));
      render->layout = VK_NULL_HANDLE;
      return false;
   }
   return true;
}

/* M5c: FUNCTION CONSTANTS, at rwa_compute_create: the frontend's options for one module. */
static void
constant_options(void *mem_ctx, const void *bitcode, size_t size,
                 const struct rwa_function_constant *given, unsigned count, unsigned is_given,
                 struct air_to_nir_options *out)
{
   struct air_constant_value *values;
   if (!is_given) {
      out->num_constants = air_module_function_constants(mem_ctx, bitcode, size, &values);
      out->constants = values;
      return;
   }
   values = rzalloc_array(mem_ctx, struct air_constant_value, MAX2(count, 1));
   for (unsigned i = 0; values && i < count; i++)
      values[i] = (struct air_constant_value){ given[i].index, given[i].bits, given[i].bits_hi };
   out->constants = values;
   out->num_constants = values ? count : 0;
   out->partial_constants = true;
}

struct rwa_render *
rwa_render_create(struct rwa_device *device,
                  const void *vertex_bitcode, size_t vertex_size,
                  const char *vertex_entry,
                  const void *fragment_bitcode, size_t fragment_size,
                  const char *fragment_entry,
                  const struct rwa_render_shape *shape,
                  struct rwa_render_info *info, char *why, size_t why_size)
{
   if (!shape) {
      say(why, why_size, "a render pipeline with no attachment description");
      return NULL;
   }
   if (!fragment_bitcode || fragment_size < 4) {
      say(why, why_size, "%zu bytes is not a fragment shader module", fragment_size);
      return NULL;
   }
   /* The shape M3b draws: colour attachments only, one sample, no vertex layout, no
    * blending. Each other shape is refused naming what it asked for. [M3c-2: and a
    * depth/stencil attachment, below, by air_metal.c's checks in air_metal.c's order.]
    *
    * No colour attachment is allowed only with a depth one, which is Metal's own rule (a
    * depth-only pass is a shadow map) and air_metal.c's.
    */
   if (shape->color_count == 0 && shape->depth_format == 0 && shape->stencil_format == 0) {
      say(why, why_size, "a render pipeline with no colour attachments and no depth "
                         "attachment: there would be nothing for the fragment shader to "
                         "write");
      return NULL;
   }
   if (shape->color_count > 8) {
      say(why, why_size, "%u colour attachments, more than Metal's eight", shape->color_count);
      return NULL;
   }
   /* [M3c-3: a pipeline of more than one sample is real now (MULTISAMPLE, THE PASS); what
    * used to stand here was "a render pipeline of N samples per pixel: multisampling is
    * not in the Vulkan back end yet (M3b)". What is refused instead is a count Vulkan has
    * no bit for, and one this GPU's passes do not take -- framebufferColorSampleCounts for
    * the colour attachments and the depth and stencil counts for a depth/stencil one --
    * each asked of turnip and named.] */
   unsigned samples = shape->sample_count ? shape->sample_count : 1;
   VkSampleCountFlagBits sample_flag = sample_bit(samples);
   if (!sample_flag) {
      say(why, why_size, "a render pipeline of %u samples per pixel: Vulkan counts samples "
                         "in powers of two (VkSampleCountFlagBits), and Metal's own counts "
                         "are 1, 2, 4 and 8", samples);
      return NULL;
   }
   {
      char words[48];
      const char *which = NULL;
      VkSampleCountFlags have = 0;
      if (shape->color_count && !(device->fb_color_samples & sample_flag)) {
         which = "colour";
         have = device->fb_color_samples;
      }
      if (!which && shape->depth_format && !(device->fb_depth_samples & sample_flag)) {
         which = "depth";
         have = device->fb_depth_samples;
      }
      if (!which && shape->stencil_format && !(device->fb_stencil_samples & sample_flag)) {
         which = "stencil";
         have = device->fb_stencil_samples;
      }
      if (which) {
         say(why, why_size, "a render pipeline of %u samples per pixel: %s renders a pass "
                            "with %s attachments of %s samples (framebuffer sample counts), "
                            "and %u is not among them (M3c)", samples, device->name, which,
             sample_count_words(have, words, sizeof(words)), samples);
         return NULL;
      }
   }
   /* [M5a: a descriptor is served over a vertex function of the application's own
    * (THE DRAW PATH, VERTEX INPUT), and refused, as on Gallium and in its words, over the stage
    * this library generates. What stood here was "a vertex descriptor configures attribute N:
    * vertex attributes are not in the Vulkan back end yet (M3b)", for any stage.] */
   for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES && !vertex_bitcode; i++) {
      if (shape->vertex_attribute[i].format == 0)
         continue;
      say(why, why_size, "a vertex descriptor configures attribute %u and this descriptor names "
                         "no vertex function: the stage this library would generate draws its "
                         "own full-screen quad and has no attribute to lay out", i);
      return NULL;
   }
   if (shape->color_blend_count && shape->color_blend_count < shape->color_count) {
      say(why, why_size, "blend state for %u colour attachment%s and %u attachments: the "
                         "rest would get blending silently off", shape->color_blend_count,
          shape->color_blend_count == 1 ? "" : "s", shape->color_count);
      return NULL;
   }

   /*
    * The depth/stencil attachment's format (M3c-2), by air_metal.c's checks and sentences:
    * Metal's two properties name one texture, so they are equal or one is zero; the format
    * is a depth/stencil one this back end maps; each property names an aspect the format
    * has; and the GPU can test in it -- asked of turnip, DEPTH_STENCIL_ATTACHMENT. Then the
    * one thing Vulkan adds: the state a draw sets is Vulkan 1.3's dynamic state, and a
    * device without it is refused by name rather than given a pipeline it cannot draw.
    */
   unsigned zs_mtl = shape->depth_format ? shape->depth_format : shape->stencil_format;
   const struct rwa_vk_format *zs_fmt = NULL;
   if (shape->depth_format && shape->stencil_format &&
       shape->depth_format != shape->stencil_format) {
      say(why, why_size, "a depth attachment of pixel format %u and a stencil attachment "
                         "of %u: one texture carries both aspects, so this library renders "
                         "into one attachment and cannot be given two",
          shape->depth_format, shape->stencil_format);
      return NULL;
   }
   if (zs_mtl) {
      zs_fmt = vk_format_of(zs_mtl);
      if (!zs_fmt) {
         format_refusal(why, why_size, "the depth/stencil attachment", zs_mtl);
         return NULL;
      }
      if (!format_is_depth_stencil(zs_fmt)) {
         say(why, why_size, "%s as the depth/stencil attachment: it is a colour format, and "
                            "a pass tests depth against a depth format", zs_fmt->name);
         return NULL;
      }
      if (shape->stencil_format && !(zs_fmt->zs_aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) {
         say(why, why_size, "%s as the stencil attachment: that format has a depth aspect "
                            "and no stencil one, so there would be nothing to test",
             zs_fmt->name);
         return NULL;
      }
      if (shape->depth_format && !(zs_fmt->zs_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
         say(why, why_size, "%s as the depth attachment: that format has a stencil aspect "
                            "and no depth one, so there would be nothing to test",
             zs_fmt->name);
         return NULL;
      }
      VkFormatProperties fp;
      device->vk.GetPhysicalDeviceFormatProperties(device->physical, zs_fmt->vk, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         say(why, why_size, "%s cannot use %s as a depth/stencil attachment", device->name,
             zs_fmt->name);
         return NULL;
      }
      /* and of this many samples (MULTISAMPLE), as air_metal.c asks Gallium with its count */
      if (samples > 1 &&
          !(image_sample_counts(device, zs_fmt->vk,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) & sample_flag)) {
         say(why, why_size, "%s cannot use %s of %u samples per pixel as a depth/stencil "
                            "attachment (M3c)", device->name, zs_fmt->name, samples);
         return NULL;
      }
      if (!device->CmdSetDepthTestEnable || !device->CmdSetDepthWriteEnable ||
          !device->CmdSetDepthCompareOp || !device->CmdSetStencilTestEnable ||
          !device->CmdSetStencilOp) {
         say(why, why_size, "a render pipeline with a depth/stencil attachment (%s) on %s, "
                            "which is Vulkan %u.%u: this back end sets MTLDepthStencilState "
                            "as Vulkan 1.3's dynamic depth/stencil state, and a device "
                            "without it is not in the Vulkan back end yet (M3c)",
             zs_fmt->name, device->name, VK_API_VERSION_MAJOR(device->api_version),
             VK_API_VERSION_MINOR(device->api_version));
         return NULL;
      }
   }

   struct rwa_render_info out = { 0 };
   VkFormat color_format[8];
   VkColorComponentFlags write_mask[8];
   for (unsigned i = 0; i < shape->color_count; i++) {
      char what[32];
      snprintf(what, sizeof(what), "colour attachment %u", i);
      const struct rwa_vk_format *fmt = vk_format_of(shape->color_format[i]);
      if (!fmt) {
         format_refusal(why, why_size, what, shape->color_format[i]);
         return NULL;
      }
      /* air_metal.c's refusal: a depth format in a colour slot is a descriptor that put
       * the attachment in the wrong place, which is what the caller needs told. */
      if (format_is_depth_stencil(fmt)) {
         say(why, why_size, "colour attachment %u is %s, a depth or stencil format: it "
                            "belongs in the depth attachment of the shape and not among "
                            "the colours", i, fmt->name);
         return NULL;
      }
      VkFormatProperties fp;
      device->vk.GetPhysicalDeviceFormatProperties(device->physical, fmt->vk, &fp);
      if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         say(why, why_size, "%s cannot render into %s", device->name, fmt->name);
         return NULL;
      }
      /* and of this many samples (MULTISAMPLE), as air_metal.c asks Gallium with its count */
      if (samples > 1 &&
          !(image_sample_counts(device, fmt->vk, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &
            sample_flag)) {
         say(why, why_size, "%s cannot render into %s of %u samples per pixel (M3c)",
             device->name, fmt->name, samples);
         return NULL;
      }
      color_format[i] = fmt->vk;
      unsigned m = 0xf;   /* Metal's default write mask, and what a shape without blend
                           * state asks for (air_metal.h, color_blend_count) */
      if (shape->color_blend_count) {
         const struct rwa_color_blend *cb = &shape->color_blend[i];
         /* [M5a: blending is real (blend_state_from_metal, below); what stood here was "%s
          * blends: blending is not in the Vulkan back end yet (M3b)".] */
         if (cb->write_mask > 0xf) {
            say(why, why_size, "%s has write mask %#x, which is not a MTLColorWriteMask "
                               "(four bits)", what, cb->write_mask);
            return NULL;
         }
         m = cb->write_mask;
      }
      write_mask[i] = write_mask_from_metal(m);
      out.color_write_masks |= (unsigned)write_mask[i] << (4 * i);
   }
   /* THE DRAW PATH (M5a), BLENDING: the whole state, before anything is made of it. */
   VkPipelineColorBlendAttachmentState blend[8];
   if (!blend_state_from_metal(device, shape, blend, &out.blend_enabled_mask, why, why_size))
      return NULL;

   struct rwa_render *render = calloc(1, sizeof(*render));
   if (!render) {
      say(why, why_size, "out of memory making a render pipeline");
      return NULL;
   }
   render->device = device;
   render->mem_ctx = ralloc_context(NULL);
   render->shape = *shape;
   memcpy(render->color_format, color_format, sizeof(color_format));
   memcpy(render->write_mask, write_mask, sizeof(write_mask));
   memcpy(render->blend, blend, sizeof(blend));
   render->zs_mtl = zs_mtl;
   render->zs_format = zs_fmt ? zs_fmt->vk : VK_FORMAT_UNDEFINED;
   render->zs_aspects = zs_fmt ? zs_fmt->zs_aspects : 0;
   render->samples = samples;
   render->sample_bit = sample_flag;
   render->vstage.name = "the generated vertex stage";
   render->fstage.name = "the fragment function";
   p_atomic_inc(&device->children);

   /* The fragment shader, through the frontend as air_metal.c calls it -- function
    * constants, then air_to_nir -- with turnip's options in place of a Gallium screen's.
    */
   /* [M5c: or what MTLFunctionConstantValues said (FUNCTION CONSTANTS, at rwa_compute_create)] */
   struct air_to_nir_options fopts = { 0 };
   constant_options(render->mem_ctx, fragment_bitcode, fragment_size, shape->fragment_constants,
                    shape->fragment_constant_count, shape->fragment_constants_given, &fopts);
   const char *error;
   render->fs = air_to_nir(render->mem_ctx, fragment_bitcode, fragment_size, fragment_entry,
                           &fopts, device->nir_options, &render->finfo, &error);
   if (!render->fs) {
      say(why, why_size, "%s", error ? error : "the fragment module could not be translated");
      goto fail;
   }
   if (render->finfo.stage != MESA_SHADER_FRAGMENT) {
      say(why, why_size, "%s is a %s shader, and this makes render pipelines with a "
                         "fragment stage",
          render->finfo.entry_point ? render->finfo.entry_point : "the function",
          render->finfo.stage >= MESA_SHADER_VERTEX && render->finfo.stage < MESA_SHADER_STAGES
             ? _mesa_shader_stage_to_string(render->finfo.stage) : "non-fragment");
      goto fail;
   }
   /* [M5d: what stood here refused any of it: "reads its own render targets (framebuffer fetch,
    * mask M), which is not in the Vulkan back end yet (M3b)" (FRAMEBUFFER FETCH, at lower_fbfetch).] */
   if (render->finfo.fbfetch_targets && samples > 1) {
      say(why, why_size, "%s reads its own render targets (framebuffer fetch, mask %#x) in a pipeline "
                         "of %u samples per pixel, which this back end does not read a sample of",
          render->finfo.entry_point ? render->finfo.entry_point : "the fragment function",
          render->finfo.fbfetch_targets, samples);
      goto fail;
   }
   /*
    * A store to [[depth]] (M3c-2): reported as air_metal.c reports it
    * (rwa_render_info.fragment_writes_depth, which the driver's -writesDepth answers with),
    * accepted by a pipeline that has a depth attachment -- turnip takes FRAG_RESULT_DEPTH
    * as it takes SPIR-V's FragDepth -- and refused by name, as on Gallium, by one that has
    * none: the sentence below is the one this back end has always given, and
    * metal_drv_test --draw-all looks for its "stores to [[depth]]" to retry with a depth
    * attachment.
    *
    * A store to [[stencil]] needs a stencil attachment for the same reason, and with one
    * it is Vulkan's VK_EXT_shader_stencil_export, which turnip has and this device is not
    * asked for: refused by name (M3c). No shader of the corpus has one.
    */
   bool writes_depth = writes_location(render->fs, FRAG_RESULT_DEPTH);
   bool writes_stencil = writes_location(render->fs, FRAG_RESULT_STENCIL);
   if ((writes_depth && !shape->depth_format) || (writes_stencil && !zs_mtl)) {
      say(why, why_size, "%s stores to [[depth]] or [[stencil]] and this pipeline has no "
                         "depth attachment, so there is nowhere for that store to go",
          render->finfo.entry_point ? render->finfo.entry_point : "the fragment function");
      goto fail;
   }
   if (writes_stencil) {
      say(why, why_size, "%s stores to [[stencil]]: a fragment shader's own stencil "
                         "reference (VK_EXT_shader_stencil_export) is not in the Vulkan "
                         "back end yet (M3c)",
          render->finfo.entry_point ? render->finfo.entry_point : "the fragment function");
      goto fail;
   }
   out.fragment_writes_depth = writes_depth ? 1 : 0;
   shape_like_spirv(render->fs);
   if (!stage_bindings(device, render->mem_ctx, render->fs, &render->finfo, "fragment", 0,
                       false, &render->fstage, why, why_size))
      goto fail;

   if (vertex_bitcode) {
      /* A vertex function of the application's own: it may bind buffers, textures and
       * samplers as the fragment one may (Apple's own three read their corners out of a
       * buffer by air.vertex_id), and fetches no attribute -- [[stage_in]] is M3c.
       * [M5a: [[stage_in]] is real (THE DRAW PATH, VERTEX INPUT); those three keep the
       * push-descriptor path they had, with no vertex input at all.]
       */
      if (vertex_size < 4) {
         say(why, why_size, "%zu bytes is not a vertex shader module", vertex_size);
         goto fail;
      }
      struct air_to_nir_options vopts = { 0 };
      constant_options(render->mem_ctx, vertex_bitcode, vertex_size, shape->vertex_constants,
                       shape->vertex_constant_count, shape->vertex_constants_given, &vopts);
      render->vs = air_to_nir(render->mem_ctx, vertex_bitcode, vertex_size, vertex_entry,
                              &vopts, device->nir_options, &render->vinfo, &error);
      if (!render->vs) {
         say(why, why_size, "%s", error ? error : "the vertex module could not be translated");
         goto fail;
      }
      if (render->vinfo.stage != MESA_SHADER_VERTEX) {
         say(why, why_size, "%s is a %s shader, and the vertex stage of a render pipeline "
                            "takes a vertex function",
             render->vinfo.entry_point ? render->vinfo.entry_point : "the function",
             render->vinfo.stage >= MESA_SHADER_VERTEX &&
             render->vinfo.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(render->vinfo.stage) : "non-vertex");
         goto fail;
      }
      /* [M5a: what stood here was "%s reads vertex attributes ([[stage_in]], mask %#x): vertex
       * attributes are not in the Vulkan back end yet (M3b)". Now air_metal.c's two steps: the
       * two readings of what this stage fetches compared rather than one of them trusted,
       * and the tables built from the module's interface and the descriptor's layout.] */
      {
         const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point
                                                    : "the vertex function";
         unsigned fetched = 0;
         for (unsigned i = 0; i < render->vinfo.num_vertex_attributes; i++)
            fetched += render->vinfo.vertex_attributes[i].used ? 1 : 0;
         if (vertex_reads_attributes(render->vs) != (fetched != 0)) {
            say(why, why_size, "%s: the translated shader %s vertex attribute inputs and its "
                               "interface lists %u fetched attribute%s, and the fetches are "
                               "built from the interface", vn,
                vertex_reads_attributes(render->vs) ? "has" : "has no", fetched,
                fetched == 1 ? "" : "s");
            goto fail;
         }
         if (fetched && !build_vertex_state(device, &render->vinfo, shape, vn, &render->vstate,
                                            why, why_size))
            goto fail;
      }
      /* the sixteen bytes a gated, unconfigured attribute reads: the first corner of
       * air_metal.c's quad, which is what it reads on Gallium, and nothing looks at them */
      if (render->vstate.placeholder) {
         static const float corner[4] = { -1.0f, -1.0f, 0.0f, 1.0f };
         if (!raw_create(device, sizeof(corner), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         &render->placeholder, why, why_size))
            goto fail;
         memcpy(render->placeholder.map, corner, sizeof(corner));
      }
      shape_like_spirv(render->vs);
      if (!stage_bindings(device, render->mem_ctx, render->vs, &render->vinfo, "vertex",
                          RWA_BIND_VERTEX_BASE, false, &render->vstage, why, why_size))
         goto fail;
      render->vs_writes_point_size = writes_location(render->vs, VARYING_SLOT_PSIZ);
      render->generated_vertex = false;
   } else {
      render->vs = quad_vertex_shader(render->mem_ctx, device->nir_options,
                                      render->finfo.num_varyings);
      render->vstage.name = "the generated vertex stage";
      render->vstage.flags = VK_SHADER_STAGE_VERTEX_BIT;
      render->generated_vertex = true;
      out.generated_vertex_stage = 1;
   }

   /* The resources: the shader's own samplers made, AIR's indices rewritten into the
    * push set's bindings, and the layouts the pipeline is made with.
    */
   if (!stage_own_samplers(device, &render->fstage, why, why_size) ||
       !stage_own_samplers(device, &render->vstage, why, why_size) ||
       !lower_stage(&render->fstage, render->fs, why, why_size) ||
       (!render->generated_vertex && !lower_stage(&render->vstage, render->vs, why, why_size)))
      goto fail;
   lower_fbfetch(render);
   if (!make_layouts(render, why, why_size))
      goto fail;

   /* The pipeline for the strip, now: it is what every draw of the generated stage is,
    * and a shader turnip will not take is a creation failure the driver puts in the
    * NSError of -newRenderPipelineStateWithDescriptor:error:, where an application looks
    * for it. Another topology gets its pipeline at the first draw that asks.
    */
   if (!pipeline_for(render, 4, why, why_size))
      goto fail;

   /* What each stage binds, counted as air_metal.c counts it; [M5a and M5c: the vertex streams, the
    * argument-buffer masks and the blend mask are below and above.] The write masks are in Gallium's
    * (and Vulkan's) bit order, as air_metal.c reports them. fragment_writes_depth was set
    * where the fragment shader's stores were read (M3c-2), as air_metal.c sets it.
    */
   count_stage(&render->fstage, &out.fragment_buffers, &out.fragment_textures,
               &out.fragment_samplers);
   count_stage(&render->vstage, &out.vertex_buffers, NULL, NULL);
   /* [M5a: the streams are no longer zero, and the blend mask was set above: what the layout
    * became, for a driver that has to fill the stream slots, as air_metal.c reports it. Its
    * mask is what splits Metal's one table at the draw (hkmetal.m).] */
   out.vertex_attribute_count = render->vstate.count;
   for (unsigned i = 0; i < render->vstate.count && i < ARRAY_SIZE(out.vertex_attribute_index); i++)
      out.vertex_attribute_index[i] = render->vstate.attribute[i];
   out.vertex_streams = render->vstate.stream_count;
   out.vertex_stream_mask = render->vstate.stream_mask;
   /* [M5c: which buffer indices are sets, as air_metal.c reports them, and its one refusal] */
   if (!argbuf_mask_of(&render->finfo, render->fstage.name, &out.fragment_argbuf_mask, why, why_size))
      goto fail;
   if (!render->generated_vertex &&
       !argbuf_mask_of(&render->vinfo, render->vstage.name, &out.vertex_argbuf_mask, why, why_size))
      goto fail;
   if (out.vertex_argbuf_mask & out.vertex_stream_mask) {
      unsigned both = out.vertex_argbuf_mask & out.vertex_stream_mask;
      say(why, why_size, "the vertex descriptor fetches attributes from buffer index %u and %s "
                         "declares an argument buffer there: Metal has one table behind "
                         "-setVertexBuffer:offset:atIndex: and an index is either a stream or a "
                         "set", ffs(both) - 1, render->vstage.name);
      goto fail;
   }
   if (info)
      *info = out;
   return render;

fail:
   rwa_render_destroy(render);
   return NULL;
}

static void
render_free(struct rwa_device *device, void *object)
{
   struct rwa_render *render = object;
   for (unsigned i = 0; i < RWA_PRIMITIVE_TYPES; i++)
      if (render->pipeline[i])
         device->vk.DestroyPipeline(device->dev, render->pipeline[i], NULL);
   if (render->layout && render->layout != device->empty_layout)
      device->vk.DestroyPipelineLayout(device->dev, render->layout, NULL);
   if (render->set_layout)
      device->vk.DestroyDescriptorSetLayout(device->dev, render->set_layout, NULL);
   stage_free_samplers(&render->fstage);
   stage_free_samplers(&render->vstage);
   raw_destroy(device, &render->placeholder);
   ralloc_free(render->mem_ctx);
   p_atomic_dec(&device->children);
   free(render);
}

void
rwa_render_destroy(struct rwa_render *render)
{
   if (!render)
      return;
   when_done(render->device, render->use, render_free, render);
}

/*
 * What one binding of a stage is bound to at this draw, judged as air_metal.c's
 * check_bindings judges it, each refusal naming what was asked for -- and, for a buffer,
 * the descriptor's buffer, offset and range, with a copy planned where the offset is one
 * Vulkan does not accept (RESOURCES, OFFSETS).
 *
 * [M3d: and at a dispatch, with `draw` NULL -- a kernel has no attachment for a texture to
 * collide with -- and a storage image judged by MTLTextureUsageShaderWrite, in
 * check_bindings' words ("writes texture n"), where a sampled one is judged by ShaderRead.
 * The device comes in as itself rather than through the render pipeline, which is the one
 * thing of that pipeline this ever read.]
 */
struct rwa_draw_binding {
   const struct rwa_binding *b;
   const struct rwa_stage *st;    /* M5b: whose */
   unsigned element;              /* M5c: which descriptor of an array, 0 otherwise */
   bool absent;                   /* M5c: nothing fills it, and nothing reaches it (resolve_stage) */
   struct rwa_buffer *buffer;     /* a buffer's */
   size_t offset;
   VkDeviceSize range;
   bool copy;                     /* misaligned and read-only: staged first */
   struct rwa_texture *texture;   /* a texture's */
   VkImageView texture_view;      /* M5b: which of its views: choose_texture_views */
   VkSampler sampler;             /* a sampler's */
   struct rwa_sampler *sampler_object;   /* M4-1: which that is one of, to name it */
};

/* Whether `t`, a texture a stage samples, covers the one level and slice `a` is rendered at. Of
 * two images, no; of one, by what each says of the image's own levels and layers (M5b). */
static bool
window_overlaps_attachment(const struct rwa_texture *t, const struct rwa_texture *a,
                           unsigned level, unsigned slice)
{
   if (t->root != a->root)
      return false;
   const unsigned at_level = a->base_level + level, at_layer = a->base_layer + slice;
   /* a 3D image's depth planes are inside its one layer: a level is as fine as it can be said */
   if (t->root->shape.type == RWA_TYPE_3D)
      return at_level >= t->base_level && at_level < t->base_level + t->levels;
   return at_level >= t->base_level && at_level < t->base_level + t->levels &&
          at_layer >= t->base_layer && at_layer < t->base_layer + t->layers;
}

/* The view a sampler in pixel coordinates binds with (TEXTURES, LEVELS AND SLICES, SAMPLERS): one
 * level, the one it reaches. Vulkan's rule for such a sampler; turnip would not know the
 * difference, and a shader that only works by breaking valid usage works on one driver. */
static VkImageView
base_view_of(struct rwa_texture *t, char *why, size_t why_size)
{
   if (t->levels == 1)
      return t->view;
   if (t->base_view)
      return t->base_view;
   const VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t->image,
      .viewType = t->view_type,
      .format = t->format->vk,
      .components = t->swizzle,
      .subresourceRange = { format_is_depth_stencil(t->format)
                               ? ((t->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                                     ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT)
                               : VK_IMAGE_ASPECT_COLOR_BIT,
                            t->base_level, 1, t->base_layer, t->layers },
   };
   VkResult r = t->device->vk.CreateImageView(t->device->dev, &vci, NULL, &t->base_view);
   if (r != VK_SUCCESS) {
      t->base_view = VK_NULL_HANDLE;
      say(why, why_size, "turnip would not make a view of one level of a %s image for a sampler "
                         "in pixel coordinates (%s)", t->format->name, vk_Result_to_str(r));
   }
   return t->base_view;
}

/*
 * ARGUMENT BUFFERS (M5c), which are air_metal.c's, and docs/air.md 8.2 has the design. The
 * frontend resolves every member of a set to a binding of its own, so there is no buffer of
 * descriptors for a GPU to read: an MTLArgumentEncoder RECORDS (resource, [[id(n)]]) against
 * (destination buffer, offset), and a draw looks up what is bound at the set's Metal index and
 * binds what was recorded. A buffer nobody encoded is refused by name, and that is the feature's
 * one limit: handles written by the processor, by a kernel or by a blit have no record.
 *
 * Where this differs from Gallium's, with the reason.
 *
 *   - A member no instruction reaches is not bound here, so it need not have been encoded. A
 *     set declares every member its struct has and a shader reads what it reads, and real Metal
 *     never looks at the handle of one it does not read. The layout (rwa_module_argbufs) is
 *     still over all of them, because the length an application allocates is.
 *   - Members have a binding range of their own, RWA_BIND_MEMBER_BASE and up, in the order they
 *     come: the flat index counts members nothing reads, and it is past Metal's ranges.
 *   - The record is read when the draw is RECORDED and not when the GPU reads it, as on Gallium.
 *     Real Metal reads the handles at execution, so a set re-encoded after the draw is encoded
 *     and before the command buffer runs has its new contents there and its old ones here.
 *     Nothing in the corpus does that; KNOWN_ISSUES says so.
 *
 * MORE THAN A PUSH SET HOLDS. maxPushDescriptors is 32 on turnip, and one set of Resident Evil
 * 4's has 43 members, so a pipeline of more than that takes an ordinary descriptor set per draw
 * or dispatch, from a pool that belongs to the recording (rwa_fence.sets), reset with it. The
 * layout has no PUSH flag then, which is all a pipeline knows of it. A push set stays the case
 * below that: it has no pool and no allocation to go wrong.
 *
 * ARRAYS OF DESCRIPTORS. An `array<texture2d<float>, 3>` as a direct argument or a member, and a
 * C array of buffer pointers as a member, are one binding of that many descriptors, `index` the
 * first of them. The frontend says the element as a constant in the index, or as a value when the
 * shader chose it at run time (air_ptr_index, and texture_offset and sampler_offset on a tex):
 * lower_stage makes the deref_array or the vulkan_resource_index that Vulkan's form wants, and
 * a draw has one rwa_draw_binding and one write per descriptor, dstArrayElement its element.
 * Real Metal takes an array with elements nothing was bound to and a shader that does not reach them,
 * and Vulkan wants every statically used descriptor valid, so an element nothing fills is written
 * as another element of the same array: reading one is undefined in Metal, and here it reads
 * a wrong texture rather than faulting. A binding nothing at all fills is a refusal, unless a
 * function constant gates it (air_resource.gated) or no instruction reaches it. Then it has no
 * write at all: a branch the constant turned off is still in the NIR here, because the constant is
 * loaded through the global the module's constructor stores, and ir3 folds it.
 *
 * A TEXTURE A FRAGMENT OR VERTEX SHADER WRITES is a kernel's, COMPUTE, STORAGE IMAGES, in the same
 * descriptor type and in GENERAL. A store in a draw is ordered against the draw after it by that
 * draw's barrier_all, and within a pass by nothing, as in Metal without memoryBarrier. Whether
 * a pass with one runs once per covered pixel is turnip's (it leaves GMEM and binning for a
 * shader with side effects), and rows of the M5c tests count them.
 */
static struct rwa_argbuf_encoding *
argbuf_find(const struct rwa_buffer *dest, size_t offset)
{
   for (struct rwa_argbuf_encoding *e = dest ? dest->encodings : NULL; e; e = e->next) {
      if (e->offset == offset)
         return e;
   }
   return NULL;
}

/* The encoding at `offset`, made if it is not there, with room for `id`. Not cleared when found:
 * air_metal.c's argbuf_encoding_for says why that is Metal's behaviour. Under argbuf_lock. */
static struct rwa_argbuf_encoding *
argbuf_encoding_for(struct rwa_buffer *dest, size_t offset, unsigned id, char *why, size_t why_size)
{
   struct rwa_argbuf_encoding *e = argbuf_find(dest, offset);

   if (!e) {
      e = calloc(1, sizeof(*e));
      if (!e) {
         say(why, why_size, "out of memory recording an argument buffer");
         return NULL;
      }
      e->offset = offset;
      e->next = dest->encodings;
      dest->encodings = e;
      if (!dest->listed) {
         dest->listed = true;
         dest->next_encoded = dest->device->encoded;
         dest->device->encoded = dest;
      }
   }
   if (id >= e->slots) {
      unsigned want = (id + 8) & ~7u;
      struct rwa_argbuf_slot *grown = realloc(e->slot, want * sizeof(*grown));

      if (!grown) {
         say(why, why_size, "out of memory recording member %u of an argument buffer", id);
         return NULL;
      }
      memset(grown + e->slots, 0, (want - e->slots) * sizeof(*grown));
      e->slot = grown;
      e->slots = want;
   }
   return e;
}

/* What every one of the three set calls checks before it records anything. Under argbuf_lock. */
static struct rwa_argbuf_encoding *
argbuf_begin_member(struct rwa_buffer *dest, size_t dest_offset, unsigned id, const char *what,
                    char *why, size_t why_size)
{
   if (id >= RWA_MAX_ARGBUF_MEMBERS) {
      say(why, why_size, "%s was encoded at id %u and this library records %u members of a set",
          what, id, RWA_MAX_ARGBUF_MEMBERS);
      return NULL;
   }
   /* the handle Metal would write is eight bytes at dest_offset + 8 * id */
   size_t need = dest_offset + (size_t)8 * id + 8;
   if (need > dest->size) {
      say(why, why_size, "%s was encoded at id %u of an argument buffer at offset %zu, which "
                         "Metal would write %zu bytes into, and that buffer holds %zu", what,
          id, dest_offset, need, dest->size);
      return NULL;
   }
   return argbuf_encoding_for(dest, dest_offset, id, why, why_size);
}

/* One member into its slot, whichever it is, and the count of them. */
static int
argbuf_record(struct rwa_buffer *dest, size_t dest_offset, unsigned id, const char *what,
              struct rwa_argbuf_slot slot, char *why, size_t why_size)
{
   if (!dest) {
      say(why, why_size, "%s was encoded into no argument buffer at all: Metal's "
                         "-setArgumentBuffer:offset: has to name one first", what);
      return -1;
   }
   pthread_mutex_lock(&dest->device->argbuf_lock);
   struct rwa_argbuf_encoding *e = argbuf_begin_member(dest, dest_offset, id, what, why, why_size);
   if (e) {
      if (!e->slot[id].set)
         e->members++;
      slot.set = true;
      e->slot[id] = slot;
   }
   pthread_mutex_unlock(&dest->device->argbuf_lock);
   return e ? 0 : -1;
}

int
rwa_argbuf_encode_buffer(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                         struct rwa_buffer *buffer, size_t offset, char *why, size_t why_size)
{
   if (buffer && dest && buffer->device != dest->device) {
      say(why, why_size, "a buffer of another device was encoded at id %u of an argument buffer",
          id);
      return -1;
   }
   if (buffer && offset > buffer->size) {
      say(why, why_size, "a buffer was encoded at id %u of an argument buffer at offset %zu, and "
                         "it holds %zu bytes", id, offset, buffer->size);
      return -1;
   }
   return argbuf_record(dest, dest_offset, id, "a buffer",
                        (struct rwa_argbuf_slot){ .kind = RWA_ARGBUF_BUFFER, .buffer = buffer,
                                                  .offset = offset }, why, why_size);
}

int
rwa_argbuf_encode_texture(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_texture *texture, char *why, size_t why_size)
{
   if (texture && dest && texture->device != dest->device) {
      say(why, why_size, "a texture of another device was encoded at id %u of an argument buffer",
          id);
      return -1;
   }
   return argbuf_record(dest, dest_offset, id, "a texture",
                        (struct rwa_argbuf_slot){ .kind = RWA_ARGBUF_TEXTURE, .texture = texture },
                        why, why_size);
}

int
rwa_argbuf_encode_sampler(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_sampler *sampler, char *why, size_t why_size)
{
   if (sampler && dest && sampler->device != dest->device) {
      say(why, why_size, "a sampler of another device was encoded at id %u of an argument buffer",
          id);
      return -1;
   }
   return argbuf_record(dest, dest_offset, id, "a sampler",
                        (struct rwa_argbuf_slot){ .kind = RWA_ARGBUF_SAMPLER, .sampler = sampler },
                        why, why_size);
}

unsigned
rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset)
{
   if (!dest)
      return 0;
   pthread_mutex_lock(&dest->device->argbuf_lock);
   const struct rwa_argbuf_encoding *e = argbuf_find(dest, dest_offset);
   unsigned members = e ? e->members : 0;
   pthread_mutex_unlock(&dest->device->argbuf_lock);
   return members;
}

/* A resource going away takes itself out of every record that names it, leaving the flag the draw
 * names it by (air_metal.c: the alternative is a crash in this library and not a wrong picture). */
static void
argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
             const struct rwa_texture *texture, const struct rwa_sampler *sampler)
{
   pthread_mutex_lock(&device->argbuf_lock);
   for (struct rwa_buffer *b = device->encoded; b; b = b->next_encoded) {
      for (struct rwa_argbuf_encoding *e = b->encodings; e; e = e->next) {
         for (unsigned i = 0; i < e->slots; i++) {
            struct rwa_argbuf_slot *sl = &e->slot[i];

            if (!sl->set || sl->destroyed)
               continue;
            if ((buffer && sl->buffer == buffer) || (texture && sl->texture == texture) ||
                (sampler && sl->sampler == sampler)) {
               sl->destroyed = true;
               sl->buffer = NULL;
               sl->texture = NULL;
               sl->sampler = NULL;
            }
         }
      }
   }
   pthread_mutex_unlock(&device->argbuf_lock);
}

static void
argbuf_forget_all(struct rwa_buffer *dest)
{
   struct rwa_device *device = dest->device;

   pthread_mutex_lock(&device->argbuf_lock);
   for (struct rwa_argbuf_encoding *e = dest->encodings, *next; e; e = next) {
      next = e->next;
      free(e->slot);
      free(e);
   }
   dest->encodings = NULL;
   if (dest->listed) {
      struct rwa_buffer **link = &device->encoded;

      while (*link && *link != dest)
         link = &(*link)->next_encoded;
      if (*link == dest)
         *link = dest->next_encoded;
      dest->listed = false;
      dest->next_encoded = NULL;
   }
   pthread_mutex_unlock(&device->argbuf_lock);
}

/* One member, at the draw: what the frontend resolved out of a set and what was recorded for it,
 * with air_metal.c's argbuf_member's refusals in its sentences. */
static bool
argbuf_member(struct rwa_device *device, const struct air_resource *r, unsigned element,
              const struct rwa_bindings *bind,
              const struct rwa_buffer_bind *table, unsigned table_count, const char *name,
              struct rwa_buffer **buffer, size_t *offset, struct rwa_texture **texture,
              struct rwa_sampler **sampler, char *why, size_t why_size)
{
   const char *kind = r->kind == AIR_RESOURCE_SAMPLER ? "sampler"
                    : r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE ? "texture"
                                                                                       : "buffer";
   const char *member = r->name ? r->name : "an unnamed member";
   struct rwa_buffer *set = NULL;
   size_t at = 0;

   if (table && r->argbuf_set < table_count && table[r->argbuf_set].buffer) {
      set = table[r->argbuf_set].buffer;
      at = table[r->argbuf_set].offset;
   } else if (bind && bind->buffers && r->argbuf_set < bind->buffer_count) {
      set = bind->buffers[r->argbuf_set];
   }
   if (!set) {
      say(why, why_size, "%s reads %s `%s` out of the argument buffer at Metal buffer index %u "
                         "and nothing is bound there", name, kind, member, r->argbuf_set);
      return false;
   }

   pthread_mutex_lock(&device->argbuf_lock);
   const struct rwa_argbuf_encoding *e = argbuf_find(set, at);
   const unsigned id = r->argbuf_id + element;
   const struct rwa_argbuf_slot *sl = e && id < e->slots ? &e->slot[id] : NULL;
   struct rwa_argbuf_slot found = sl ? *sl : (struct rwa_argbuf_slot){ 0 };
   const unsigned members = e ? e->members : 0;
   pthread_mutex_unlock(&device->argbuf_lock);

   if (members == 0) {
      say(why, why_size, "the buffer bound as the argument buffer at Metal buffer index %u "
                         "(offset %zu) was never filled through an MTLArgumentEncoder, so there "
                         "is no record of what %s's %s `%s` (id %u) should be. This driver binds "
                         "an argument buffer's members from what the encoder recorded and never "
                         "from the handles in the buffer, which are an Apple GPU's descriptors; "
                         "one written by the processor, by a kernel or by a blit cannot be read "
                         "here at all", r->argbuf_set, at, name, kind, member, r->argbuf_id);
      return false;
   }
   if (!found.set) {
      say(why, why_size, "%s reads %s `%s` at id %u of the argument buffer at Metal buffer index "
                         "%u, and that id was never set on the encoder (%u member%s encoded at "
                         "offset %zu)", name, kind, member, id, r->argbuf_set, members,
          members == 1 ? "" : "s", at);
      return false;
   }
   if (found.destroyed) {
      say(why, why_size, "the %s encoded at id %u of the argument buffer at Metal buffer index %u "
                         "has been destroyed since, and %s reads `%s` out of it", kind,
          id, r->argbuf_set, name, member);
      return false;
   }
   const unsigned want = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
                       : r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE
                            ? RWA_ARGBUF_TEXTURE : RWA_ARGBUF_BUFFER;
   if (found.kind != want) {
      static const char *const encoded_as[] = { "a buffer", "a texture", "a sampler" };
      say(why, why_size, "%s declares %s `%s` at id %u of the argument buffer at Metal buffer "
                         "index %u, and %s was encoded there", name, kind, member, id,
          r->argbuf_set, found.kind < 3 ? encoded_as[found.kind] : "something else");
      return false;
   }
   /* a member set to nil, which Metal allows and a read of which is the application's */
   if ((want == RWA_ARGBUF_BUFFER && !found.buffer) || (want == RWA_ARGBUF_TEXTURE && !found.texture) ||
       (want == RWA_ARGBUF_SAMPLER && !found.sampler)) {
      say(why, why_size, "%s reads %s `%s` at id %u of the argument buffer at Metal buffer index "
                         "%u, and nil was encoded there", name, kind, member, id,
          r->argbuf_set);
      return false;
   }
   *buffer = found.buffer;
   *offset = found.offset;
   *texture = found.texture;
   *sampler = found.sampler;
   return true;
}

/* Which Metal buffer indices of a stage are sets. air_metal.c's, and its reason: an index missing
 * from this mask is one a driver would hand over as a plain buffer. */
static bool
argbuf_mask_of(const struct air_shader_info *info, const char *name, unsigned *mask, char *why,
               size_t why_size)
{
   *mask = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];

      if (r->argbuf_set == ~0u)
         continue;
      if (r->argbuf_set >= 32) {
         say(why, why_size, "%s declares an argument buffer at Metal buffer index %u, and this "
                            "contract reports the first 32 indices", name, r->argbuf_set);
         return false;
      }
      *mask |= 1u << r->argbuf_set;
   }
   return true;
}

int
rwa_module_argbufs(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_argbuf_layout *out, unsigned max,
                   unsigned *count, char *why, size_t why_size)
{
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u argument buffer layouts", max);
      return -1;
   }
   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }
   /* function constants taken as zero: air_metal.c's, and rwa_module_textures' above */
   struct air_constant_value *constants;
   unsigned num_constants = air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants, .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options, device->nir_options,
                                &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   const char *name = info.entry_point ? info.entry_point : "the function";
   unsigned n = 0;
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->argbuf_set == ~0u)
         continue;
      struct rwa_argbuf_layout *l = NULL;
      for (unsigned k = 0; k < n; k++)
         if (out[k].metal_index == r->argbuf_set)
            l = &out[k];
      if (!l) {
         if (n >= max) {
            say(why, why_size, "%s declares an argument buffer at Metal buffer index %u and "
                               "there is room for %u layout%s", name, r->argbuf_set, max,
                max == 1 ? "" : "s");
            ralloc_free(mem_ctx);
            return -1;
         }
         l = &out[n++];
         memset(l, 0, sizeof(*l));
         l->metal_index = r->argbuf_set;
         l->alignment = 8;
      }
      /* [an array member is one entry per id, because the encoder sets each id and the length is
       * eight bytes for each of them] */
      for (unsigned e = 0; e < r->array_size; e++) {
         if (l->member_count >= RWA_MAX_ARGBUF_MEMBERS ||
             r->argbuf_id + e >= RWA_MAX_ARGBUF_MEMBERS) {
            say(why, why_size, "member `%s` (id %u) of the argument buffer %s declares at Metal "
                               "buffer index %u is past the %u members this library records",
                r->name ? r->name : "?", r->argbuf_id + e, name, r->argbuf_set,
                RWA_MAX_ARGBUF_MEMBERS);
            ralloc_free(mem_ctx);
            return -1;
         }
         struct rwa_argbuf_member *m = &l->members[l->member_count++];
         m->id = r->argbuf_id + e;
         m->index = r->index + e;
         m->write = r->write ? 1 : 0;
         m->kind = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
                 : r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE
                      ? RWA_ARGBUF_TEXTURE : RWA_ARGBUF_BUFFER;
         snprintf(m->name, sizeof(m->name), "%s", r->name ? r->name : "");
      }
   }
   /* the ids have to be 0..member_count-1, which is what makes 8 x members the length Metal would
    * have answered: air_metal.c refuses one that is not dense, for that reason, and so here */
   for (unsigned k = 0; k < n; k++) {
      struct rwa_argbuf_layout *l = &out[k];
      uint64_t seen = 0;
      for (unsigned i = 0; i < l->member_count; i++) {
         if (l->members[i].id >= l->member_count || (seen & (1ull << l->members[i].id))) {
            say(why, why_size, "the argument buffer %s declares at Metal buffer index %u has %u "
                               "members and `%s` is id %u, which is past them or a second one: "
                               "Metal's encodedLength is over the highest id, and this library "
                               "will not answer a length for a set with a gap in its ids", name,
                l->metal_index, l->member_count, l->members[i].name, l->members[i].id);
            ralloc_free(mem_ctx);
            return -1;
         }
         seen |= 1ull << l->members[i].id;
      }
      l->encoded_length = 8 * l->member_count;
   }
   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

static bool
resolve_binding(struct rwa_device *device, const struct rwa_stage *st,
                const struct rwa_binding *b, unsigned element, const struct rwa_bindings *bind,
                const struct rwa_buffer_bind *table, unsigned table_count,
                const struct rwa_draw *draw, struct rwa_draw_binding *out, char *why,
                size_t why_size)
{
   memset(out, 0, sizeof(*out));
   out->b = b;
   out->st = st;
   out->element = element;
   /* [M5c: a member comes out of the encoder's record, and everything below judges what it finds
    * there as it judges a direct argument (ARGUMENT BUFFERS)] */
   struct rwa_buffer *member_buffer = NULL;
   size_t member_offset = 0;
   struct rwa_texture *member_texture = NULL;
   struct rwa_sampler *member_sampler = NULL;
   if (b->member && !argbuf_member(device, b->r, element, bind, table, table_count, st->name,
                                   &member_buffer, &member_offset, &member_texture,
                                   &member_sampler, why, why_size))
      return false;
   switch (b->kind) {
   case RWA_BIND_BUFFER: {
      struct rwa_buffer *buf = NULL;
      size_t offset = 0;
      if (b->member) {
         buf = member_buffer;
         offset = member_offset;
      } else if (bind->buffers && b->index < bind->buffer_count) {
         buf = bind->buffers[b->index];
      }
      if (!b->member && table && b->index < table_count && table[b->index].buffer) {
         if (buf && buf != table[b->index].buffer) {
            say(why, why_size, "the two buffer tables of this draw disagree about index %u: "
                               "%s reads a buffer there, and one table names one buffer and "
                               "the other names another", b->index, st->name);
            return false;
         }
         buf = table[b->index].buffer;
         offset = table[b->index].offset;
      }
      if (!buf) {
         say(why, why_size, "%s reads buffer %u and %u buffer%s bound", st->name, b->index,
             bind->buffer_count, bind->buffer_count == 1 ? " was" : "s were");
         return false;
      }
      if (buf->device != device) {
         say(why, why_size, "buffer %u of %s belongs to another device", b->index, st->name);
         return false;
      }
      if (offset >= buf->size) {
         say(why, why_size, "buffer %u of %s is bound at offset %zu of a buffer that holds "
                            "%zu bytes, which leaves nothing to read", b->index, st->name,
             offset, buf->size);
         return false;
      }
      if (b->r->size && buf->size - offset < b->r->size) {
         say(why, why_size, "buffer %u is %zu bytes%s and %s declares it as %u", b->index,
             buf->size - offset, offset ? " after the offset it is bound at" : "", st->name,
             b->r->size);
         return false;
      }
      bool ubo = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      VkDeviceSize align = ubo ? device->ubo_align : device->ssbo_align;
      out->buffer = buf;
      out->offset = offset;
      out->range = ubo ? b->ubo_size : (VkDeviceSize)(buf->size - offset);
      if (offset % align) {
         if (b->written) {
            say(why, why_size, "buffer %u of %s is bound at offset %zu, and %s writes it: this "
                               "GPU binds a storage buffer at multiples of %llu bytes, and a "
                               "copy at an aligned offset would take the writes with it when "
                               "it was thrown away", b->index, st->name, offset, st->name,
                (unsigned long long)align);
            return false;
         }
         out->copy = true;
      }
      return true;
   }
   case RWA_BIND_TEXTURE: {
      struct rwa_texture *t = b->member ? member_texture
                            : bind->textures && b->index + element < bind->texture_count
                                 ? bind->textures[b->index + element] : NULL;
      if (!t) {
         say(why, why_size, "%s %s texture %u and %u texture%s bound", st->name,
             b->stored ? "writes" : "reads", b->index, bind->texture_count,
             bind->texture_count == 1 ? " was" : "s were");
         return false;
      }
      if (t->device != device) {
         say(why, why_size, "texture %u of %s belongs to another device", b->index, st->name);
         return false;
      }
      /* [M3d: a storage image by its own usage bit, in air_metal.c's check_bindings
       * sentence: Metal refuses to bind a texture to a shader that writes it unless the
       * application asked for ShaderWrite, and without it the image has no STORAGE usage
       * for a descriptor to name.] */
      if (b->stored && (!t->shader_write || !t->store_view)) {
         say(why, why_size, "%s writes texture %u and that texture was made without "
                            "MTLTextureUsageShaderWrite", st->name, b->index);
         return false;
      }
      /* [M5b: image_deref's constant, and it is the one thing it could be wrong about] */
      if (b->asks_levels && t->levels != 1) {
         say(why, why_size, "%s asks texture %u, which it writes, how many levels it has, and it "
                            "has %u: Vulkan has no such query on a storage image, this back end "
                            "answers 1 for it, and that would be wrong here (M5b)", st->name,
             b->index, t->levels);
         return false;
      }
      if (!b->stored && (!t->shader_read || !t->view)) {
         say(why, why_size, "%s samples texture %u and that texture was made without "
                            "MTLTextureUsageShaderRead", st->name, b->index);
         return false;
      }
      /* [M5b: the same image is another matter, and Metal allows it: a pass into one level of a
       * texture may sample another through a view (rwa_render_draw has them in GENERAL, TEXTURES,
       * LEVELS AND SLICES). What it still refuses is reading the very level and slice it renders
       * into, as it did the one texture.] */
      /* [M5d: only a view of ONE level can be known to read what it renders. CoreAnimation's
       * variable_blur_downsample_frag binds the whole mipmapped texture, renders into level N and reads
       * level N-1 with an explicit lod out of a buffer, and a whole frame went with the refusal
       * (run 142550). Which level a shader reads is a run-time number here, as it is on Apple's GPU.] */
      for (unsigned i = 0; draw && i < draw->color_count; i++) {
         if (draw->color && draw->color[i] && t->levels == 1 &&
             window_overlaps_attachment(t, draw->color[i],
                                        draw->color_level ? draw->color_level[i] : 0,
                                        draw->color_slice ? draw->color_slice[i] : 0)) {
            say(why, why_size, "%s samples texture %u, which is colour attachment %u of the "
                               "same pass: a texture is either read or rendered into in one "
                               "pass, in Metal as in Vulkan", st->name, b->index, i);
            return false;
         }
      }
      /* and the depth attachment likewise (M3c-2): sampled in SHADER_READ_ONLY_OPTIMAL and
       * tested in DEPTH_STENCIL_ATTACHMENT_OPTIMAL is two layouts at once, and Metal's one
       * exception -- a read-only depth attachment -- is not something this contract says */
      if (draw && draw->depth &&
          window_overlaps_attachment(t, draw->depth, draw->depth_level, draw->depth_slice)) {
         say(why, why_size, "%s samples texture %u, which is the depth attachment of the "
                            "same pass: a texture is either read or rendered into in one "
                            "pass here", st->name, b->index);
         return false;
      }
      if (b->shape_known) {
         unsigned declared;
         if (!metal_texture_type(b->dim, b->is_array, &declared) ||
             declared != t->shape.type) {
            say(why, why_size, "%s declares texture %u as a %s and the texture bound there "
                               "is a %s", st->name, b->index,
                shader_texture_name(b->dim, b->is_array), texture_type_name(t->shape.type));
            return false;
         }
      }
      /* A comparison needs a depth texture: Metal's rule, and air_metal.c's refusal of a
       * colour one. Since M3c-2 there is a depth texture to bind, and its view is the depth
       * aspect alone, which is what a comparing sampler reads. A Stencil8 texture has no
       * depth to compare and Vulkan compares none (SAMPLED_IMAGE_DEPTH_COMPARISON is a
       * depth format's feature), so it is named apart. */
      if (b->shadow && !format_is_depth_stencil(t->format)) {
         say(why, why_size, "%s samples texture %u with a comparison and the texture bound "
                            "there is %s, which is a colour format: Metal declares "
                            "sample_compare on depth2d alone", st->name, b->index,
             t->format->name);
         return false;
      }
      if (b->shadow && !(t->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
         say(why, why_size, "%s samples texture %u with a comparison and the texture bound "
                            "there is %s, which has a stencil aspect and no depth to "
                            "compare against", st->name, b->index, t->format->name);
         return false;
      }
      nir_alu_type have;
      const char *word;
      unsigned bits;
      if (binding_element(b, &have, &word, &bits)) {
         nir_alu_type want = t->format->kind == RWA_KIND_UINT ? nir_type_uint
                           : t->format->kind == RWA_KIND_SINT ? nir_type_int
                                                              : nir_type_float;
         if (want != have) {
            say(why, why_size, "%s reads texture %u as a %s<%s> and %s is %s format",
                st->name, b->index, shader_texture_name(b->dim, b->is_array), word,
                t->format->name,
                want == nir_type_float ? "a floating-point or normalised"
                : want == nir_type_uint ? "an unsigned integer" : "a signed integer");
            return false;
         }
      }
      out->texture = t;
      return true;
   }
   case RWA_BIND_SAMPLER: {
      const struct rwa_sampler *s = b->inline_sampler ? &b->own : NULL;
      if (!s) {
         s = b->member ? member_sampler
           : bind->samplers && b->index + element < bind->sampler_count
                ? bind->samplers[b->index + element] : NULL;
         if (!s) {
            say(why, why_size, "%s takes sampler %u from the API and %u sampler%s bound",
                st->name, b->index, bind->sampler_count,
                bind->sampler_count == 1 ? " was" : "s were");
            return false;
         }
         if (s->device != device) {
            say(why, why_size, "sampler %u of %s belongs to another device", b->index,
                st->name);
            return false;
         }
      }
      /* [M3d-2: since the shader's own pixel-coordinate sampler is made normalised for a
       * texture3d (its own.pixel_coords is false), only an API sampler reaches this. Its
       * kind is known HERE and nowhere earlier: Metal binds a MTLSamplerState per encoder,
       * after the pipeline, so lower_stage, which runs once at pipeline creation, cannot
       * have divided the coordinates for it, and a sampler that may be pixel or normalised
       * at each draw would need a second translation and a second VkPipeline per topology
       * (or per threadgroup size) chosen here, and a normalised twin of every API
       * sampler -- none of which is in this back end. Said with "(M3d)"; until M3d-2 it was
       * "... is not in the Vulkan back end yet (M3c)" and covered both kinds.] */
      if (b->samples_3d && s->pixel_coords) {
         say(why, why_size, "%s samples a texture3d with sampler %u from the API, which is "
                            "in pixel coordinates (normalizedCoordinates NO): Vulkan allows "
                            "unnormalizedCoordinates on 1D and 2D views only, the shader's "
                            "coordinates are turned into normalised ones at pipeline creation "
                            "for its own constexpr samplers alone, and an API sampler's kind "
                            "is not known until the draw, so a texture3d sampled in pixel "
                            "coordinates through one is not in the Vulkan back end (M3d)",
             st->name, b->index);
         return false;
      }
      /* [M5b: an integer texture's border colour (TEXTURES, LEVELS AND SLICES, SAMPLERS). One
       * sampler reads the one colour in a pipeline's stage, so a sampler of both kinds through
       * an opaque border cannot be served by either.] */
      if (b->samples_int && b->samples_float && s->plain_int) {
         say(why, why_size, "%s samples integer and floating-point textures through sampler %u, "
                            "whose address mode reads an opaque border colour: Vulkan has one "
                            "border colour of one type in a descriptor, and this is both (M5b)",
             st->name, b->index);
         return false;
      }
      out->sampler = b->compare ? s->compare
                   : b->samples_int && s->plain_int ? s->plain_int : s->plain;
      out->sampler_object = (struct rwa_sampler *)s;
      if (!out->sampler) {
         say(why, why_size, "%s compares with sampler %u, which is in pixel coordinates, and "
                            "Vulkan has no comparing sampler of that kind", st->name,
             b->index);
         return false;
      }
      return true;
   }
   }
   return false;
}

/* M5c: whether anything fills this descriptor. A set that was encoded and an id set in it count as
 * something, however wrong: resolve_binding says what is wrong with them. */
static bool
element_filled(const struct rwa_binding *b, unsigned element, const struct rwa_bindings *bind,
               const struct rwa_buffer_bind *table, unsigned table_count)
{
   if (b->kind == RWA_BIND_SAMPLER && b->inline_sampler)
      return true;
   if (b->member) {
      const struct rwa_buffer *set = NULL;
      size_t at = 0;
      if (table && b->r->argbuf_set < table_count && table[b->r->argbuf_set].buffer) {
         set = table[b->r->argbuf_set].buffer;
         at = table[b->r->argbuf_set].offset;
      } else if (bind && bind->buffers && b->r->argbuf_set < bind->buffer_count) {
         set = bind->buffers[b->r->argbuf_set];
      }
      if (!set)
         return false;
      pthread_mutex_lock(&set->device->argbuf_lock);
      const struct rwa_argbuf_encoding *e = argbuf_find(set, at);
      const unsigned id = b->r->argbuf_id + element;
      const bool filled = e && id < e->slots && e->slot[id].set;
      pthread_mutex_unlock(&set->device->argbuf_lock);
      return filled;
   }
   const unsigned i = b->index + element;
   switch (b->kind) {
   case RWA_BIND_BUFFER:
      return (table && i < table_count && table[i].buffer) ||
             (bind->buffers && i < bind->buffer_count && bind->buffers[i]);
   case RWA_BIND_TEXTURE:
      return bind->textures && i < bind->texture_count && bind->textures[i];
   default:
      return bind->samplers && i < bind->sampler_count && bind->samplers[i];
   }
}

/*
 * M5c: every descriptor of a stage, in `db` from *n. ARRAYS OF DESCRIPTORS has the rules: an element
 * of an array nothing fills reads like another element of that array, and a binding wholly unfilled
 * is a refusal, with resolve_binding's sentence, unless a function constant gates it or no
 * instruction reaches it.
 */
static bool
resolve_stage(struct rwa_device *device, const struct rwa_stage *st, const struct rwa_bindings *bind,
              const struct rwa_buffer_bind *table, unsigned table_count, const struct rwa_draw *draw,
              struct rwa_draw_binding *db, unsigned *n, char *why, size_t why_size)
{
   for (unsigned i = 0; i < st->count; i++) {
      const struct rwa_binding *b = &st->b[i];
      const unsigned first = *n;
      int filled = -1;
      for (unsigned k = 0; k < b->span; k++, (*n)++) {
         if (!element_filled(b, k, bind, table, table_count)) {
            db[*n] = (struct rwa_draw_binding){ .b = b, .st = st, .element = k, .absent = true };
            continue;
         }
         if (!resolve_binding(device, st, b, k, bind, table, table_count, draw, &db[*n], why,
                              why_size))
            return false;
         if (filled < 0)
            filled = (int)*n;
      }
      if (filled < 0) {
         if (b->gated || !b->used)
            continue;
         struct rwa_draw_binding unused;
         return resolve_binding(device, st, b, 0, bind, table, table_count, draw, &unused, why,
                                why_size) && false;
      }
      for (unsigned k = first; k < *n; k++) {
         if (db[k].absent) {
            const unsigned element = db[k].element;
            db[k] = db[filled];
            db[k].element = element;
         }
      }
   }
   return true;
}

/*
 * M5b: which view of each texture a stage binds: `view`, or for one read through a sampler in
 * pixel coordinates the one-level view Vulkan's rule asks for (base_view_of). A sampler's kind is
 * known here and not before, for the reason resolve_binding gives of an API one. A storage image
 * has its own (store_view). Sampler bindings are in the same array, so a texture's mask is read
 * against the samplers of its own stage.
 */
static bool
choose_texture_views(struct rwa_draw_binding *db, unsigned total, char *why, size_t why_size)
{
   for (unsigned i = 0; i < total; i++) {
      struct rwa_texture *t = db[i].texture;
      if (!t)
         continue;
      if (db[i].b->stored) {
         db[i].texture_view = t->store_view;
         continue;
      }
      db[i].texture_view = t->view;
      if (t->levels == 1 || !db[i].b->sampler_mask)
         continue;
      for (unsigned j = 0; j < total; j++) {
         const struct rwa_binding *sb = db[j].b;
         if (sb->kind != RWA_BIND_SAMPLER || sb->index >= 64 ||
             !(db[i].b->sampler_mask & (1ull << sb->index)) || !db[j].sampler_object ||
             !db[j].sampler_object->pixel_coords)
            continue;
         if (db[j].st != db[i].st)
            continue;   /* each stage numbers its samplers from 0 */
         if (!(db[i].texture_view = base_view_of(t, why, why_size)))
            return false;
         break;
      }
   }
   return true;
}

/* Whether these bindings write texture `t`'s image as a storage image anywhere (COMPUTE, STORAGE
 * IMAGES): a texture the kernel also samples through another slot is then in GENERAL for
 * both descriptors, the one layout both kinds may share. [M5c: and a draw's stages likewise; this
 * was dispatch_writes_texture.] */
static bool
stages_write_image(const struct rwa_draw_binding *db, unsigned total, const struct rwa_texture *t)
{
   /* [M5b: one image, through whichever views: a kernel that reads level 0 and writes level 1] */
   for (unsigned i = 0; i < total; i++)
      if (db[i].texture && db[i].texture->root == t->root && db[i].b->stored)
         return true;
   return false;
}

/* Whether a draw renders into the image of this texture, through another window (M5b). */
static bool
draw_shares_image(const struct rwa_draw *draw, const struct rwa_texture *t)
{
   for (unsigned i = 0; i < draw->color_count; i++)
      if (draw->color[i] && draw->color[i]->root == t->root)
         return true;
   return draw->depth && draw->depth->root == t->root;
}

/* And the other way round: whether a stage samples the image of this attachment. */
static bool
stages_sample_image(const struct rwa_draw_binding *db, unsigned total, const struct rwa_texture *t)
{
   for (unsigned i = 0; i < total; i++)
      if (db[i].texture && db[i].texture->root == t->root)
         return true;
   return false;
}

/*
 * Whether one attachment of a pass has the shape the pipeline was made for, in the two ways
 * that are not its pixel format: air_metal.c's attachment_shape_fits, check for check and
 * sentence for sentence (MULTISAMPLE, THE PASS). Metal spells "more than one sample" in the
 * texture's TYPE and in its sampleCount and requires the pipeline's count to be the
 * attachment's; both are checked, in both directions, and Vulkan adds no second opinion to
 * fall back on -- without VK_AMD_mixed_attachment_samples (or multisampled-render-to-
 * single-sampled, neither asked for here) a draw's attachments have exactly the bound
 * pipeline's rasterizationSamples, and anything else is not allowed at all.
 */
static bool
attachment_shape_fits(const struct rwa_texture *t, unsigned samples, const char *what,
                      char *why, size_t why_size)
{
   unsigned want_type = samples > 1 ? RWA_TYPE_2D_MULTISAMPLE : RWA_TYPE_2D;
   /* [M5b: and of each kind, every type that has a level and a slice to render into (ATTACHMENTS,
    * below): a one-sample pass into a 1D, a 2D, an array of either, a cube or a depth plane of a
    * 3D texture, a multisample one into one slice of its array.] */
   const bool is_ms = t->shape.type == RWA_TYPE_2D_MULTISAMPLE ||
                      t->shape.type == RWA_TYPE_2D_MULTISAMPLE_ARRAY;
   if ((samples > 1) != is_ms) {
      say(why, why_size, "%s is a %s and a pass of %u sample%s per pixel renders into a %s",
          what, texture_type_name(t->shape.type), samples, samples == 1 ? "" : "s",
          texture_type_name(want_type));
      return false;
   }
   if (t->samples != samples) {
      say(why, why_size, "%s has %u sample%s per pixel and the pipeline was made for %u",
          what, t->samples, t->samples == 1 ? "" : "s", samples);
      return false;
   }
   return true;
}

/* ATTACHMENTS (M5b, rwa_draw.color_level, _slice, TEXTURES, LEVELS AND SLICES): whether this level
 * and slice of an attachment exist. A slice of a 3D texture is a depth plane, of that level's
 * depth, and the image has to have been made so it can be rendered one plane at a time. */
static bool
attachment_window_fits(const struct rwa_texture *t, unsigned level, unsigned slice,
                       const char *what, char *why, size_t why_size)
{
   if (level >= t->levels) {
      say(why, why_size, "%s is level %u of a texture with %u", what, level, t->levels);
      return false;
   }
   const bool volume = t->shape.type == RWA_TYPE_3D;
   const unsigned have = volume ? level_depth(t, level) : t->layers;
   if (slice >= have) {
      say(why, why_size, "%s is %s %u of a level with %u", what, volume ? "depth plane" : "slice",
          slice, have);
      return false;
   }
   if (volume && !t->root->plane_compatible) {
      say(why, why_size, "%s is a depth plane of a 3D texture made without "
                         "MTLTextureUsageRenderTarget", what);
      return false;
   }
   return true;
}

int
rwa_render_draw(struct rwa_render *render, const struct rwa_draw *draw,
                char *why, size_t why_size)
{
   if (!render || !draw) {
      say(why, why_size, "a draw with no description");
      return -1;
   }
   struct rwa_device *device = render->device;

   /* air_metal.c's checks, in its order, for the part of a draw M3b has. */
   if (draw->color_count != render->shape.color_count) {
      say(why, why_size, "drawn into %u colour attachments and the pipeline was made for "
                         "%u", draw->color_count, render->shape.color_count);
      return -1;
   }
   /* And the depth attachment either is there or is not, on both sides (M3c-2), with
    * air_metal.c's sentence: a pass with a depth texture the pipeline knows nothing of, or
    * a pipeline made for depth drawn without one, would quietly test nothing. A
    * depth/stencil STATE without a depth attachment is not refused -- air_metal.c binds its
    * zeroed state there, because Metal tests against nothing -- and is not read below. */
   if (!!draw->depth != !!render->zs_mtl) {
      say(why, why_size, "%s and %s", draw->depth ? "the pass has a depth attachment"
                                                  : "the pass has no depth attachment",
          render->zs_mtl ? "the pipeline was made with one"
                         : "the pipeline was made without one");
      return -1;
   }
   if (draw->depth && draw->depth_stencil && draw->depth_stencil->device != device) {
      say(why, why_size, "the draw's depth/stencil state belongs to another device");
      return -1;
   }
   /* a clearDepth is read only where there is a depth to clear, as air_metal.c's mask is */
   if (draw->depth && (draw->depth->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && draw->clear_depth &&
       !clear_depth_fits(*draw->clear_depth, why, why_size))
      return -1;
   if (draw->vertex_count == 0) {
      say(why, why_size, "a draw of no vertices, which would leave every attachment as it "
                         "is");
      return -1;
   }
   /*
    * Indexed or not, and everything the index buffer has to satisfy (THE DRAW PATH, M5a,
    * INDEXED DRAWS): air_metal.c's checks in its order and its sentences. What stood first
    * here was "an indexed draw (-drawIndexedPrimitives:): index buffers are not in the Vulkan
    * back end yet (M3b)".
    */
   unsigned index_size = 0;
   VkIndexType index_vk = VK_INDEX_TYPE_UINT16;
   unsigned min_index = draw->vertex_start;
   unsigned max_index = draw->vertex_start + draw->vertex_count - 1;
   if (!draw->index_buffer) {
      if (draw->base_vertex != 0 || draw->index_type != 0 || draw->index_buffer_offset != 0) {
         say(why, why_size, "a draw with no index buffer, and a base vertex of %d, index type "
                            "%u and index buffer offset %zu: -drawPrimitives: has none of the "
                            "three", draw->base_vertex, draw->index_type,
             draw->index_buffer_offset);
         return -1;
      }
      if (render->generated_vertex && draw->vertex_start + draw->vertex_count > 4) {
         say(why, why_size, "vertices %u..%u of the generated full-screen quad, which has four "
                            "(0..3)", draw->vertex_start,
             draw->vertex_start + draw->vertex_count - 1);
         return -1;
      }
   } else {
      if (draw->index_buffer->device != device) {
         say(why, why_size, "the index buffer belongs to another device");
         return -1;
      }
      if (draw->vertex_start != 0) {
         say(why, why_size, "an indexed draw starting at vertex %u: Metal's indexed draws have "
                            "no vertexStart, and where the indices start is the index buffer "
                            "offset", draw->vertex_start);
         return -1;
      }
      if (!index_type_from_metal(draw->index_type, &index_size, &index_vk, why, why_size))
         return -1;
      if (draw->index_buffer_offset % index_size != 0) {
         say(why, why_size, "an index buffer offset of %zu bytes with indices of %u bytes each: "
                            "Metal requires the offset to be a multiple of the index size",
             draw->index_buffer_offset, index_size);
         return -1;
      }
      size_t need = draw->index_buffer_offset + (size_t)draw->vertex_count * index_size;
      if (need > draw->index_buffer->size) {
         say(why, why_size, "%u indices of %u bytes at offset %zu want %zu bytes of an index "
                            "buffer that holds %zu", draw->vertex_count, index_size,
             draw->index_buffer_offset, need, draw->index_buffer->size);
         return -1;
      }
      /* Read on the processor only where something is bounded by them: the stage's own quad
       * or a stream that steps per vertex. A supplied stage with no stream reads its own
       * buffer with the index, and that is the application's business, exactly as in Metal. */
      if (render->generated_vertex || render->vstate.per_vertex) {
         index_range((const uint8_t *)draw->index_buffer->raw.map + draw->index_buffer_offset,
                     draw->vertex_count, index_size,
                     draw->primitive == 2 || draw->primitive == 4, &min_index, &max_index);
      }
      if (render->generated_vertex) {
         int64_t low = (int64_t)min_index + draw->base_vertex;
         int64_t high = (int64_t)max_index + draw->base_vertex;
         if (low < 0 || high > 3) {
            say(why, why_size, "indices %u..%u and a base vertex of %d, which is vertices "
                               "%lld..%lld of the generated full-screen quad, and it has four "
                               "(0..3)", min_index, max_index, draw->base_vertex,
                (long long)low, (long long)high);
            return -1;
         }
      }
   }
   unsigned instances = draw->instance_count ? draw->instance_count : 1;

   /*
    * THE VERTEX STREAMS (M5a), air_metal.c's checks before anything is recorded: bound, this
    * device's, and for a stream that steps per vertex, the elements this draw walks inside
    * the buffer -- reading past a vertex buffer is bounded on this GPU, not wild, and what it
    * makes is a picture made partly of zeros. One thing Vulkan adds: an offset AT the end of
    * the buffer is not a binding at all (vkCmdBindVertexBuffers wants less than the size).
    */
   const struct rwa_vertex_state *vst = &render->vstate;
   int64_t low_vertex = (int64_t)min_index + (index_size ? draw->base_vertex : 0);
   int64_t high_vertex = (int64_t)max_index + (index_size ? draw->base_vertex : 0);
   for (unsigned k = 0; vst->count && k < RWA_MAX_VERTEX_BUFFERS; k++) {
      if (!(vst->stream_mask & (1u << k)))
         continue;
      const struct rwa_vertex_stream *st =
         k < draw->vertex_stream_count && draw->vertex_streams ? &draw->vertex_streams[k] : NULL;
      if (!st || !st->buffer) {
         say(why, why_size, "the vertex descriptor fetches from buffer index %u and nothing is "
                            "bound there (the draw covers %u stream slot%s): the fetch would "
                            "read whatever that slot held", k, draw->vertex_stream_count,
             draw->vertex_stream_count == 1 ? "" : "s");
         return -1;
      }
      if (st->buffer->device != device) {
         say(why, why_size, "vertex stream %u belongs to another device", k);
         return -1;
      }
      size_t have = st->buffer->size;
      if (st->offset >= have) {
         say(why, why_size, "vertex stream %u is bound at offset %zu of a buffer that holds %zu "
                            "bytes", k, st->offset, have);
         return -1;
      }
      if (!(vst->per_vertex & (1u << k)))
         continue;
      int64_t first = (int64_t)st->offset + (int64_t)vst->stride[k] * low_vertex;
      if (first < 0) {
         say(why, why_size, "vertex %lld of stream %u is %lld bytes before the start of its "
                            "buffer: a base vertex of %d at a stride of %u",
             (long long)low_vertex, k, (long long)-first, draw->base_vertex, vst->stride[k]);
         return -1;
      }
      size_t need = (size_t)((int64_t)st->offset + (int64_t)vst->stride[k] * high_vertex) +
                    vst->bytes[k];
      if (need > have) {
         say(why, why_size, "vertex %lld of stream %u ends %zu bytes into a buffer that holds "
                            "%zu: %u attribute byte%s at a stride of %u from offset %zu",
             (long long)high_vertex, k, need, have, vst->bytes[k],
             vst->bytes[k] == 1 ? "" : "s", vst->stride[k], st->offset);
         return -1;
      }
   }

   /* The face of a triangle (THE DRAW PATH, M5a, ORIENTATION): Metal's numbers, by table. */
   VkCullModeFlags cull;
   switch (draw->cull_mode) {
   case 0: cull = VK_CULL_MODE_NONE; break;
   case 1: cull = VK_CULL_MODE_FRONT_BIT; break;
   case 2: cull = VK_CULL_MODE_BACK_BIT; break;
   default:
      say(why, why_size, "a cull mode of %u, which is none of MTLCullMode's None 0, Front 1 and "
                         "Back 2", draw->cull_mode);
      return -1;
   }
   if (draw->front_winding > 1) {
      say(why, why_size, "a winding of %u, which is neither MTLWindingClockwise (0) nor "
                         "MTLWindingCounterClockwise (1)", draw->front_winding);
      return -1;
   }
   if (cull != VK_CULL_MODE_NONE && (!device->CmdSetCullMode || !device->CmdSetFrontFace)) {
      say(why, why_size, "a draw that culls, on %s, which is Vulkan %u.%u: this back end sets "
                         "them as Vulkan 1.3's dynamic state (M5a)", device->name,
          VK_API_VERSION_MAJOR(device->api_version), VK_API_VERSION_MINOR(device->api_version));
      return -1;
   }
   float znear = draw->depth_range ? draw->depth_range[0] : 0.0f;
   float zfar = draw->depth_range ? draw->depth_range[1] : 1.0f;
   if (!(znear >= 0.0f && znear <= 1.0f && zfar >= 0.0f && zfar <= 1.0f)) {
      say(why, why_size, "a depth range of [%g, %g]: Metal's and Vulkan's both lie in "
                         "[0, 1]", znear, zfar);
      return -1;
   }

   unsigned w = 0, h = 0;
   /* [M5b: the view of each attachment's level and slice, got here where a refusal costs nothing,
    * and whether its image is also sampled by this draw through another window of it] */
   VkImageView att_views[8] = { VK_NULL_HANDLE };
   VkImageView zs_att_view = VK_NULL_HANDLE;
   for (unsigned i = 0; i < draw->color_count; i++) {
      struct rwa_texture *t = draw->color ? draw->color[i] : NULL;
      if (!t) {
         say(why, why_size, "colour attachment %u is not a texture", i);
         return -1;
      }
      if (t->device != device) {
         say(why, why_size, "colour attachment %u belongs to another device", i);
         return -1;
      }
      if (!t->render_target) {
         say(why, why_size, "colour attachment %u (%s) was made without "
                            "MTLTextureUsageRenderTarget", i, t->format->name);
         return -1;
      }
      /* Its one view is 3D, which samples and cannot be rendered into; Metal renders
       * into one depth plane of it (MTLRenderPassAttachmentDescriptor.depthPlane), which
       * this contract does not carry, and a 2D view of one plane would need the image
       * made VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT. air_metal.c refuses it as well
       * (attachment_shape_fits: a pass renders into a MTLTextureType2D).
       */
      /* [M5b: the contract carries it now (color_slice, a depth plane of a 3D texture), and the
       * image is made that way when it is a render target. air_metal.c still refuses it.] */
      /* [M3c-3: and a multisample one, of the pipeline's count and no other: air_metal.c's
       * attachment_shape_fits, with its sentences (MULTISAMPLE, THE PASS).] */
      char what[48];
      snprintf(what, sizeof(what), "colour attachment %u", i);
      if (!attachment_shape_fits(t, render->samples, what, why, why_size))
         return -1;
      const unsigned at_level = draw->color_level ? draw->color_level[i] : 0;
      const unsigned at_slice = draw->color_slice ? draw->color_slice[i] : 0;
      if (!attachment_window_fits(t, at_level, at_slice, what, why, why_size) ||
          !(att_views[i] = att_view_for(t, at_level, at_slice, why, why_size)))
         return -1;
      if (t->shape.pixel_format != render->shape.color_format[i]) {
         say(why, why_size, "colour attachment %u is pixel format %u and the pipeline's "
                            "attachment %u is %u", i, t->shape.pixel_format, i,
             render->shape.color_format[i]);
         return -1;
      }
      /* [M5b: the size of the level it is rendered at, which is what the pass's is] */
      const unsigned aw = level_width(t, at_level), ah = level_height(t, at_level);
      /* [M5d: this refused any two sizes ("colour attachment %u is %ux%u and attachment 0 is
       * %ux%u"). CoreAnimation renders a 576x112 piece of a blur with a 1152x224 scratch as attachment
       * 1, and the whole frame went with the refusal: Maps' and Clock's bottoms, run 142131. The pass is
       * the part all of them have, which is what Apple's GPU draws and what Vulkan's renderArea asks for.] */
      w = i == 0 ? aw : MIN2(w, aw);
      h = i == 0 ? ah : MIN2(h, ah);
   }

   /* The depth/stencil attachment (M3c-2), by air_metal.c's checks and the colour ones':
    * this device's, a 2D texture (rwa_texture_create makes no other kind of depth
    * texture), a depth/stencil format and the pipeline's, made as a render target -- so it
    * has its every-aspect view -- and the pass's size; a depth-only pass takes its size
    * from it. */
   struct rwa_texture *zt = draw->depth;
   if (zt) {
      if (zt->device != device) {
         say(why, why_size, "the depth attachment belongs to another device");
         return -1;
      }
      if (!format_is_depth_stencil(zt->format)) {
         say(why, why_size, "the depth attachment is %s, which is a colour format",
             zt->format->name);
         return -1;
      }
      if (zt->shape.pixel_format != render->zs_mtl) {
         say(why, why_size, "the depth attachment is pixel format %u and the pipeline's is "
                            "%u", zt->shape.pixel_format, render->zs_mtl);
         return -1;
      }
      if (!attachment_shape_fits(zt, render->samples, "the depth attachment", why, why_size))
         return -1;
      if (!zt->render_target) {
         say(why, why_size, "the depth attachment (%s) was made without "
                            "MTLTextureUsageRenderTarget, so the GPU was never told it "
                            "could be drawn into", zt->format->name);
         return -1;
      }
      if (!attachment_window_fits(zt, draw->depth_level, draw->depth_slice,
                                  "the depth attachment", why, why_size) ||
          !(zs_att_view = att_view_for(zt, draw->depth_level, draw->depth_slice, why, why_size)))
         return -1;
      const unsigned zw = level_width(zt, draw->depth_level);
      const unsigned zh = level_height(zt, draw->depth_level);
      if (draw->color_count == 0) {
         w = zw;    /* a depth-only pass: the depth attachment is its size */
         h = zh;
      } else if (zw != w || zh != h) {
         say(why, why_size, "the depth attachment is %ux%u and colour attachment 0 is %ux%u",
             zw, zh, w, h);
         return -1;
      }
   }

   /*
    * The rectangles (THE DRAW PATH, M5a, ORIENTATION), judged here because they are judged
    * against the pass's size. Metal's viewport is { originX, originY, width, height } from the
    * attachment's top left, and row 0 is on top: y_window = originY + height/2 * (1 - y_ndc).
    * Vulkan's is y_window = y + height/2 * (y_ndc + 1), so y = originY + height with a NEGATIVE
    * height gives Metal's exactly -- which is the whole-attachment case the header's Y section
    * has always had, with an origin. A size must be positive here (Vulkan requires it of
    * width, and the sign of height is ours), and Vulkan's limits are said by name.
    * A scissor is cut to the pass, which Metal's validation would have refused and Vulkan's
    * rectangle does not care about; a scissor of no area draws nothing, in both.
    */
   float vp_x = 0.0f, vp_y = 0.0f, vp_w = (float)w, vp_h = (float)h;
   if (draw->viewport) {
      vp_x = draw->viewport[0];
      vp_y = draw->viewport[1];
      vp_w = draw->viewport[2];
      vp_h = draw->viewport[3];
      if (!(vp_w > 0.0f && vp_h > 0.0f)) {
         say(why, why_size, "a viewport of %g x %g: Metal's has a size, and a draw into none is "
                            "not a Vulkan viewport (M5a)", vp_w, vp_h);
         return -1;
      }
      if (vp_w > device->max_viewport[0] || vp_h > device->max_viewport[1] ||
          !(vp_x >= device->viewport_bounds[0]) || !(vp_y >= device->viewport_bounds[0]) ||
          !(vp_x + vp_w <= device->viewport_bounds[1]) ||
          !(vp_y + vp_h <= device->viewport_bounds[1])) {
         say(why, why_size, "a viewport of %g x %g at %g, %g: %s takes up to %g x %g inside "
                            "[%g, %g] (M5a)", vp_w, vp_h, vp_x, vp_y, device->name,
             device->max_viewport[0], device->max_viewport[1], device->viewport_bounds[0],
             device->viewport_bounds[1]);
         return -1;
      }
   }
   VkRect2D scissor = { { 0, 0 }, { w, h } };
   if (draw->scissor) {
      unsigned sx = MIN2(draw->scissor[0], w), sy = MIN2(draw->scissor[1], h);
      unsigned sw = MIN2(draw->scissor[2], w - sx), sh = MIN2(draw->scissor[3], h - sy);
      scissor = (VkRect2D){ { (int32_t)sx, (int32_t)sy }, { sw, sh } };
   }

   /* Every binding judged before anything is recorded, so a refused draw leaves the
    * command buffer as it was. A binding the stages do not declare is not looked at,
    * which is what Metal does with one too: a driver binds whatever the application set
    * on the encoder, and the application may set more than this pipeline reads.
    */
   unsigned total = stage_descriptors(&render->vstage) + stage_descriptors(&render->fstage);
   struct rwa_draw_binding *db = total ? calloc(total, sizeof(*db)) : NULL;
   if (total && !db) {
      say(why, why_size, "out of memory binding a draw");
      return -1;
   }
   unsigned n = 0;
   if (!resolve_stage(render->device, &render->fstage, &draw->fragment, draw->fragment_buffer_table,
                      draw->fragment_buffer_table_count, draw, db, &n, why, why_size) ||
       !resolve_stage(render->device, &render->vstage, &draw->vertex, draw->vertex_buffer_table,
                      draw->vertex_buffer_table_count, draw, db, &n, why, why_size))
      goto refused;
   if (!choose_texture_views(db, total, why, why_size))
      goto refused;

   VkPipeline pipeline = pipeline_for(render, draw->primitive, why, why_size);
   if (!pipeline)
      goto refused;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      goto refused;

   /* The writes the descriptors point at. A misaligned read-only buffer is copied on the
    * GPU into this command buffer's upload memory, in order with everything recorded
    * before it, and bound there; the descriptors are filled as the copies are planned.
    */
   VkWriteDescriptorSet *writes = total ? calloc(total, sizeof(*writes)) : NULL;
   VkDescriptorBufferInfo *bufs = total ? calloc(total, sizeof(*bufs)) : NULL;
   VkDescriptorImageInfo *imgs = total ? calloc(total, sizeof(*imgs)) : NULL;
   if (total && (!writes || !bufs || !imgs)) {
      free(writes);
      free(bufs);
      free(imgs);
      say(why, why_size, "out of memory binding a draw");
      goto refused;
   }
   /* [M4-1, the cost part: what this draw uses, named before anything of it is recorded (a
    * texture is named by its barrier below, a buffer and a sampler have none)] */
   named(&render->use, cmd);
   for (unsigned i = 0; i < total; i++) {
      if (db[i].buffer)
         named(&db[i].buffer->use, cmd);
      if (db[i].sampler_object)
         named(&db[i].sampler_object->use, cmd);
   }
   /* [M5a: and the buffers the fixed-function fetch reads, which no descriptor names] */
   if (draw->index_buffer)
      named(&draw->index_buffer->use, cmd);
   for (unsigned k = 0; vst->count && k < RWA_MAX_VERTEX_BUFFERS; k++)
      if (vst->stream_mask & (1u << k))
         named(&draw->vertex_streams[k].buffer->use, cmd);
   barrier_all(device, cmd->cmd);
   bool copied = false;
   for (unsigned i = 0; i < total; i++) {
      const struct rwa_binding *b = db[i].b;
      if (db[i].absent)
         continue;   /* M5c: no write: bind_descriptors */
      writes[i] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = b->binding,
         .dstArrayElement = db[i].element,
         .descriptorCount = 1,
         .descriptorType = b->type,
      };
      if (b->kind == RWA_BIND_BUFFER) {
         VkBuffer target = db[i].buffer->raw.buffer;
         VkDeviceSize at = db[i].offset;
         if (db[i].copy) {
            VkDeviceSize size = db[i].range;
            if (!upload_alloc(device, cmd, size, &target, &at, why, why_size)) {
               free(writes);
               free(bufs);
               free(imgs);
               goto refused;
            }
            const VkBufferCopy region = { db[i].offset, at, size };
            device->vk.CmdCopyBuffer(cmd->cmd, db[i].buffer->raw.buffer, target, 1, &region);
            copied = true;
         }
         bufs[i] = (VkDescriptorBufferInfo){ target, at, db[i].range };
         writes[i].pBufferInfo = &bufs[i];
      } else if (b->kind == RWA_BIND_TEXTURE) {
         imgs[i] = (VkDescriptorImageInfo){
            .imageView = db[i].texture_view,
            .imageLayout = draw_shares_image(draw, db[i].texture) ||
                           stages_write_image(db, total, db[i].texture)
                              ? VK_IMAGE_LAYOUT_GENERAL
                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         };
         writes[i].pImageInfo = &imgs[i];
      } else {
         imgs[i] = (VkDescriptorImageInfo){ .sampler = db[i].sampler };
         writes[i].pImageInfo = &imgs[i];
      }
   }
   if (copied)
      barrier_all(device, cmd->cmd);

   /* The textures the stages sample, then the attachments, each moved to the layout the
    * pass uses them in behind a barrier whose source is every earlier write -- outside
    * the pass, where Vulkan wants its barriers. A barrier even when the image is already
    * in that layout: two passes over one attachment are not ordered against each other by
    * anything else, and the second one's load reads what the first one's store wrote.
    */
   /* [M5b: one image sampled through one window and rendered through another is in one layout for
    * both, and that layout is GENERAL (TEXTURES, LEVELS AND SLICES, THE LAYOUT OF ONE IMAGE).] */
   for (unsigned i = 0; i < total; i++)
      if (db[i].texture) {
         /* [M5c: and a texture a stage writes (ARGUMENT BUFFERS, A TEXTURE A FRAGMENT OR VERTEX
          * SHADER WRITES)] */
         const bool writes_it = stages_write_image(db, total, db[i].texture);
         image_barrier(db[i].texture, cmd->cmd,
                       draw_shares_image(draw, db[i].texture) || writes_it
                          ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       writes_it ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                 : VK_ACCESS_SHADER_READ_BIT);
      }

   /*
    * The clear colour goes in as FOUR FLOATS whatever the format, which is exactly what
    * air_metal.c hands Gallium (`clear.f[c] = draw->clear_color[c]`): for an integer
    * attachment the float's bits land in the channel, which is the right answer for the
    * zero every caller of this contract clears with, and the same answer as the other
    * back end for any other colour -- so a picture can be compared between the two.
    * VkClearColorValue is a union, and turnip reads it by the attachment's format.
    */
   VkRenderingAttachmentInfo att[8];
   for (unsigned i = 0; i < draw->color_count; i++) {
      struct rwa_texture *t = draw->color[i];
      const bool shared = stages_sample_image(db, total, t);
      const VkImageLayout att_layout = shared ? VK_IMAGE_LAYOUT_GENERAL
                                              : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      image_barrier(t, cmd->cmd, att_layout, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      /* [M5a: per attachment where the caller says so, as Metal keeps it (rwa_draw.color_clears):
       * a pass whose attachment 0 keeps and attachment 1 clears, or clears to another colour,
       * is ordinary, and one colour for all of them could not say it. Without them the one
       * colour above, as on Gallium. MTLLoadActionDontCare arrives as a keep and
       * MTLStoreActionDontCare as a store, which are what those actions allow, and a pass of
       * several draws cannot do otherwise: each draw is a Vulkan pass of its own that the
       * next one loads, and the library cannot know which draw is the last.] */
      const float *clear = draw->color_clears
         ? ((draw->color_clear_mask & (1u << i)) ? draw->color_clears[i] : NULL)
         : draw->clear_color;
      att[i] = (VkRenderingAttachmentInfo){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = att_views[i],
         .imageLayout = att_layout,
         .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
      for (unsigned c = 0; c < 4; c++)
         att[i].clearValue.color.float32[c] = clear ? clear[c] : 0.0f;
   }
   /*
    * The depth/stencil attachment (DEPTH AND STENCIL, THE ATTACHMENT): its every-aspect
    * view in DEPTH_STENCIL_ATTACHMENT_OPTIMAL, read by the early and late fragment tests
    * and written by them. Depth and stencil each get their own load action, because
    * Metal's clearDepth and clearStencil are two pointers in rwa_draw and a pass that
    * clears one and keeps the other is ordinary; an aspect the format has not got is not
    * in the rendering info at all (air_metal.c clears only the aspects the attachment
    * has), and one that is there is always stored, as Gallium's zsbuf is.
    */
   VkRenderingAttachmentInfo zatt = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
   VkRenderingAttachmentInfo satt = zatt;
   if (zt) {
      const VkImageLayout zs_layout = stages_sample_image(db, total, zt)
         ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      image_barrier(zt, cmd->cmd, zs_layout,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
      zatt.imageView = satt.imageView = zs_att_view;
      zatt.imageLayout = satt.imageLayout = zs_layout;
      zatt.storeOp = satt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      zatt.loadOp = draw->clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      satt.loadOp = draw->clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : VK_ATTACHMENT_LOAD_OP_LOAD;
      zatt.clearValue.depthStencil.depth = draw->clear_depth ? *draw->clear_depth : 0.0f;
      satt.clearValue.depthStencil.stencil = draw->clear_stencil ? *draw->clear_stencil & 0xff
                                                                 : 0;
   }
   const VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { { 0, 0 }, { w, h } },
      .layerCount = 1,
      .colorAttachmentCount = draw->color_count,
      .pColorAttachments = att,
      .pDepthAttachment = zt && (zt->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &zatt : NULL,
      .pStencilAttachment = zt && (zt->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &satt : NULL,
   };
   device->CmdBeginRendering(cmd->cmd, &ri);
   device->vk.CmdBindPipeline(cmd->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   const bool bound = bind_descriptors(device, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       render->ordinary_set, render->set_layout, render->layout,
                                       total, writes, why, why_size);
   free(writes);
   free(bufs);
   free(imgs);
   free(db);
   /* [M5c: no set is no draw, and the sentence says (out of memory: a full pool only makes another).
    * The pass ends here with what it loaded, inside a recording that goes on.] */
   if (!bound) {
      device->CmdEndRendering(cmd->cmd);
      return -1;
   }
   /*
    * The depth/stencil state (DEPTH AND STENCIL, THE STATE), every one of the eight states
    * the pipeline declares dynamic, at every draw. The caller's rwa_depth_stencil, or
    * Metal's state when an application has set none -- no test and no write, which is
    * air_metal.c's zeroed state: depthTestEnable off, and with it no depth write in
    * Vulkan. The stencil reference is the draw's, front and back, its low eight bits.
    */
   if (zt) {
      const struct rwa_depth_stencil *dss = draw->depth_stencil;
      const VkStencilOpState off = {
         .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
         .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
         .compareMask = 0xff, .writeMask = 0xff,
      };
      const VkStencilOpState *face[2] = { dss ? &dss->front : &off, dss ? &dss->back : &off };
      const VkStencilFaceFlags which[2] = { VK_STENCIL_FACE_FRONT_BIT,
                                            VK_STENCIL_FACE_BACK_BIT };
      device->CmdSetDepthTestEnable(cmd->cmd, dss ? VK_TRUE : VK_FALSE);
      device->CmdSetDepthWriteEnable(cmd->cmd, dss ? dss->depth_write : VK_FALSE);
      device->CmdSetDepthCompareOp(cmd->cmd, dss ? dss->depth_compare : VK_COMPARE_OP_ALWAYS);
      device->CmdSetStencilTestEnable(cmd->cmd, dss ? dss->stencil_test : VK_FALSE);
      for (unsigned f = 0; f < 2; f++) {
         device->CmdSetStencilOp(cmd->cmd, which[f], face[f]->failOp, face[f]->passOp,
                                 face[f]->depthFailOp, face[f]->compareOp);
         device->vk.CmdSetStencilCompareMask(cmd->cmd, which[f], face[f]->compareMask);
         device->vk.CmdSetStencilWriteMask(cmd->cmd, which[f], face[f]->writeMask);
         device->vk.CmdSetStencilReference(cmd->cmd, which[f],
                                           draw->stencil_reference[f] & 0xff);
      }
   }
   /* Metal's y: a NEGATIVE height, so clip y = +1 is row 0 -- the header comment and
    * air_metal.c's set_viewport_states say why. The depth range is Metal's znear/zfar.
    * [M5a: and an origin and a size of the encoder's, judged above; the whole attachment
    * without one.] */
   const VkViewport viewport = {
      .x = vp_x, .y = vp_y + vp_h, .width = vp_w, .height = -vp_h,
      .minDepth = znear, .maxDepth = zfar,
   };
   device->vk.CmdSetViewport(cmd->cmd, 0, 1, &viewport);
   device->vk.CmdSetScissor(cmd->cmd, 0, 1, &scissor);
   /*
    * THE ENCODER'S OWN STATE (THE DRAW PATH, M5a): the blend constant, Metal's (0, 0, 0, 0)
    * until it is set, and the face of a triangle. The winding goes across as it is, which is
    * the one thing here that looks as if it should not: ORIENTATION says why it is not
    * reversed by the negative height.
    */
   static const float no_blend_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
   device->vk.CmdSetBlendConstants(cmd->cmd, draw->blend_color ? draw->blend_color
                                                               : no_blend_color);
   if (device->CmdSetCullMode && device->CmdSetFrontFace) {
      device->CmdSetCullMode(cmd->cmd, cull);
      device->CmdSetFrontFace(cmd->cmd, draw->front_winding ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                            : VK_FRONT_FACE_CLOCKWISE);
   }
   /* The streams, at Metal's own indices and offsets, each one the binding its layout was
    * made as; the placeholder's where a gated attribute needs it. */
   for (unsigned k = 0; vst->count && k < RWA_MAX_VERTEX_BUFFERS; k++) {
      if (!(vst->stream_mask & (1u << k)))
         continue;
      const VkDeviceSize at = draw->vertex_streams[k].offset;
      device->vk.CmdBindVertexBuffers(cmd->cmd, k, 1, &draw->vertex_streams[k].buffer->raw.buffer,
                                      &at);
   }
   if (vst->placeholder) {
      const VkDeviceSize at = 0;
      device->vk.CmdBindVertexBuffers(cmd->cmd, RWA_VK_PLACEHOLDER_BINDING, 1,
                                      &render->placeholder.buffer, &at);
   }
   if (index_size) {
      device->vk.CmdBindIndexBuffer(cmd->cmd, draw->index_buffer->raw.buffer,
                                    draw->index_buffer_offset, index_vk);
      device->vk.CmdDrawIndexed(cmd->cmd, draw->vertex_count, instances, 0, draw->base_vertex,
                                draw->base_instance);
   } else {
      device->vk.CmdDraw(cmd->cmd, draw->vertex_count, instances, draw->vertex_start,
                         draw->base_instance);
   }
   device->CmdEndRendering(cmd->cmd);
   return recorded(device, "draw", why, why_size);

refused:
   free(db);
   return -1;
}


/* ---------------------------------------------------------------------------------- */
/* Compute pipelines and dispatches (COMPUTE, M3d)                                     */
/* ---------------------------------------------------------------------------------- */

static void
compute_free(struct rwa_device *device, void *object)
{
   struct rwa_compute *compute = object;
   /* a pipeline of it may be in a submission still running, and Vulkan counts no
    * references on a pipeline (idle, above)
    * [M4-1, the cost part: so this comes after that submission's fence (rwa_compute_destroy)] */
   for (unsigned i = 0; i < compute->sizes; i++)
      device->vk.DestroyPipeline(device->dev, compute->size[i].pipeline, NULL);
   if (compute->layout && compute->layout != device->empty_layout)
      device->vk.DestroyPipelineLayout(device->dev, compute->layout, NULL);
   if (compute->set_layout)
      device->vk.DestroyDescriptorSetLayout(device->dev, compute->set_layout, NULL);
   stage_free_samplers(&compute->stage);
   ralloc_free(compute->mem_ctx);
   p_atomic_dec(&device->children);
   free(compute);
}

void
rwa_compute_destroy(struct rwa_compute *compute)
{
   if (!compute)
      return;
   when_done(compute->device, compute->use, compute_free, compute);
}

/* The kernel's push set and the pipeline layout around it (COMPUTE, THE BINDINGS):
 * make_layouts for one stage, with its own refusal -- a kernel that binds more than one
 * push set holds is named, never cut down to fit. The device's empty layout when the
 * kernel binds nothing. */
static bool
compute_layouts(struct rwa_compute *compute, char *why, size_t why_size)
{
   struct rwa_device *device = compute->device;
   const struct rwa_stage *st = &compute->stage;
   if (st->count == 0) {
      compute->layout = device->empty_layout;
      return true;
   }
   /* [M5c: as make_layouts: this said "a second, ordinary descriptor set would serve it and is
    * not in the Vulkan back end (M3d)"] */
   compute->ordinary_set = stage_descriptors(st) > device->max_push;
   VkDescriptorSetLayoutBinding *lb = ralloc_array(compute->mem_ctx,
                                                   VkDescriptorSetLayoutBinding, st->count);
   if (!lb) {
      say(why, why_size, "out of memory making a descriptor set layout");
      return false;
   }
   for (unsigned i = 0; i < st->count; i++)
      lb[i] = (VkDescriptorSetLayoutBinding){
         .binding = st->b[i].binding,
         .descriptorType = st->b[i].type,
         .descriptorCount = st->b[i].span,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };
   const VkDescriptorSetLayoutCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = compute->ordinary_set ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = st->count,
      .pBindings = lb,
   };
   VkResult r = device->vk.CreateDescriptorSetLayout(device->dev, &sci, NULL,
                                                     &compute->set_layout);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make the push descriptor set layout of %u "
                         "bindings (%s)", st->count, vk_Result_to_str(r));
      compute->set_layout = VK_NULL_HANDLE;
      return false;
   }
   const VkPipelineLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &compute->set_layout,
   };
   if ((r = device->vk.CreatePipelineLayout(device->dev, &lci, NULL, &compute->layout)) !=
       VK_SUCCESS) {
      say(why, why_size, "turnip would not make a pipeline layout (%s)", vk_Result_to_str(r));
      compute->layout = VK_NULL_HANDLE;
      return false;
   }
   return true;
}

/*
 * AIR in, a compute pipeline out, in air_metal.c's order: the translation, Gallium's two
 * sentences for a module that is not a kernel, the counts a driver is told, and -- here
 * instead of there -- the RESOURCES lowering and the layout, which are the same for every
 * threadgroup size and so are done once (COMPUTE, THE PIPELINE). Nothing is compiled
 * until a dispatch names a size.
 */
/*
 * FUNCTION CONSTANTS (M5c). MTLFunctionConstantValues reaches a driver only as hkmetal.m makes it:
 * Metal's specialization runs inside its own classes through a compiler this device has not got, so
 * -newFunctionWithName:constantValues:error: is answered there, by a function that remembers its
 * values, and they come here with the pipeline. `is_given` says the list is what the application
 * set, so a constant not on it is not defined; without it every constant is defined and zero, which
 * is what every pipeline had until this and what a plain -newFunctionWithName: still gets.
 *
 * The values go through the frontend, and only that: a pipeline is one specialization, as it is in
 * Metal. The shader reads them as the module's constructor does (air.static_init), and ir3 folds
 * what they gate.
 */
int
rwa_module_function_constants(struct rwa_device *device, const void *bitcode, size_t size,
                              struct rwa_function_constant_decl *out, unsigned max, unsigned *count,
                              char *why, size_t why_size)
{
   (void)device;
   if (count)
      *count = 0;
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }
   struct air_constant_decl *decls;
   unsigned n = air_module_function_constant_decls(mem_ctx, bitcode, size, &decls);
   if (n > max) {
      say(why, why_size, "the module declares %u function constants and there is room for %u", n,
          max);
      ralloc_free(mem_ctx);
      return -1;
   }
   for (unsigned i = 0; i < n; i++) {
      memset(&out[i], 0, sizeof(out[i]));
      out[i].index = decls[i].index;
      out[i].required = decls[i].required;
      snprintf(out[i].type, sizeof(out[i].type), "%s", decls[i].type ? decls[i].type : "");
      snprintf(out[i].name, sizeof(out[i].name), "%s", decls[i].name ? decls[i].name : "");
   }
   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

static struct rwa_compute *
compute_create(struct rwa_device *device, const void *bitcode, size_t size, const char *entry,
               const struct rwa_function_constant *given, unsigned given_count, unsigned is_given,
               struct rwa_compute_info *info, char *why, size_t why_size)
{
   if (!device) {
      say(why, why_size, "no device to make a compute pipeline on");
      return NULL;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return NULL;
   }
   struct rwa_compute *compute = calloc(1, sizeof(*compute));
   if (!compute) {
      say(why, why_size, "out of memory compiling a shader");
      return NULL;
   }
   compute->device = device;
   compute->mem_ctx = ralloc_context(NULL);
   p_atomic_inc(&device->children);
   if (!compute->mem_ctx) {
      say(why, why_size, "out of memory compiling a shader");
      goto fail;
   }

   /* Function constants as zero, for air_metal.c's reason: Metal's specialization runs
    * inside its own classes through a nil compiler and never reaches a driver, and a
    * module that needs real values says so -- the translator refuses what it cannot fold.
    * No library wrapper either: a wrapped air.visible function is not a pipeline.
    * [M5c: or the application's, FUNCTION CONSTANTS above.] */
   struct air_to_nir_options options = { 0 };
   constant_options(compute->mem_ctx, bitcode, size, given, given_count, is_given, &options);
   const char *error;
   compute->nir = air_to_nir(compute->mem_ctx, bitcode, size, entry, &options,
                             device->nir_options, &compute->info, &error);
   if (!compute->nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      goto fail;
   }
   if (compute->info.stage != MESA_SHADER_COMPUTE) {
      if (compute->info.stage == MESA_SHADER_NONE)
         say(why, why_size, "%s is an air.visible library function and not an entry "
                            "point, so it is not a compute pipeline: Metal stitches "
                            "these into a caller at pipeline time",
             compute->info.entry_point ? compute->info.entry_point : "the function");
      else
         say(why, why_size, "%s is a %s shader, and this makes compute pipelines",
             compute->info.entry_point ? compute->info.entry_point : "the function",
             compute->info.stage >= MESA_SHADER_VERTEX &&
             compute->info.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(compute->info.stage) : "non-compute");
      goto fail;
   }

   /* A threadgroup buffer is recorded rather than refused, as air_metal.c records it: the
    * pipeline is perfectly compilable, and only a dispatch has to name its size
    * (COMPUTE, THREADGROUP MEMORY). */
   for (unsigned i = 0; i < compute->info.num_resources; i++)
      if (compute->info.resources[i].kind == AIR_RESOURCE_THREADGROUP_BUFFER)
         compute->dynamic_threadgroup = true;

   /* The threadgroup memory the kernel declares, against what a Vulkan compute stage may
    * have: Gallium does not ask, and Vulkan does not allow more (VUID-RuntimeSpirv-
    * Workgroup-06530), so a kernel past it is named here rather than handed to turnip. */
   if (compute->info.threadgroup_bytes > device->max_shared) {
      say(why, why_size, "%s declares %u bytes of threadgroup memory and a compute stage "
                         "on %s has at most %u (maxComputeSharedMemorySize), which Vulkan "
                         "does not let a pipeline exceed (M3d)",
          compute->info.entry_point ? compute->info.entry_point : "the function",
          compute->info.threadgroup_bytes, device->name, device->max_shared);
      goto fail;
   }

   /* RESOURCES, exactly as a draw's stage: what vk_spirv_to_nir would have done, the
    * bindings read and refused where they must be, the shader's own samplers made, AIR's
    * indices rewritten into the push set's (now with storage images), and the layout. */
   shape_like_spirv(compute->nir);
   if (!stage_bindings(device, compute->mem_ctx, compute->nir, &compute->info, "kernel",
                       RWA_BIND_COMPUTE_BASE, false, &compute->stage, why, why_size) ||
       !stage_own_samplers(device, &compute->stage, why, why_size) ||
       !lower_stage(&compute->stage, compute->nir, why, why_size) ||
       !compute_layouts(compute, why, why_size))
      goto fail;

   /* What Metal must be told, counted as air_metal.c counts it (count_stage is its
    * count_resources' rule: the highest index of each kind plus one, the shader's own
    * samplers not among them), and the two numbers THE WAVE explains. */
   count_stage(&compute->stage, &compute->caps.buffers, &compute->caps.textures,
               &compute->caps.samplers);
   compute->caps.static_shared_bytes = compute->info.threadgroup_bytes;
   compute->caps.max_threads_per_group = device->max_threads;
   compute->caps.thread_execution_width = device->simd_width;
   /* [M5c: a kernel's sets are bound now (ARGUMENT BUFFERS); this was 0] */
   if (!argbuf_mask_of(&compute->info, compute->stage.name, &compute->caps.argbuf_mask, why, why_size))
      goto fail;
   if (info)
      *info = compute->caps;
   return compute;

fail:
   rwa_compute_destroy(compute);
   return NULL;
}

/*
 * The VkPipeline for one threadgroup size, made the first time a dispatch asks for it and
 * kept (COMPUTE, THE PIPELINE): compute_state_for's clone -- workgroup_size set,
 * workgroup_size_variable cleared -- handed to turnip through the NIR door, with the wave
 * contract Gallium's (THE WAVE). turnip clones what it is given and hashes it
 * (vk_pipeline_shader_stage_to_nir, vk_pipeline_hash_shader_stage), so the clone is freed
 * as soon as the pipeline exists, and two sizes are two pipelines in its cache too.
 */
static VkPipeline
compute_pipeline_for(struct rwa_compute *compute, const unsigned block[3], char *why,
                     size_t why_size)
{
   struct rwa_device *device = compute->device;
   for (unsigned i = 0; i < compute->sizes; i++)
      if (compute->size[i].block[0] == block[0] && compute->size[i].block[1] == block[1] &&
          compute->size[i].block[2] == block[2])
         return compute->size[i].pipeline;

   nir_shader *cs = nir_shader_clone(NULL, compute->nir);
   if (!cs) {
      say(why, why_size, "out of memory compiling the shader");
      return VK_NULL_HANDLE;
   }
   cs->info.workgroup_size[0] = block[0];
   cs->info.workgroup_size[1] = block[1];
   cs->info.workgroup_size[2] = block[2];
   cs->info.workgroup_size_variable = false;

   const VkPipelineShaderStageNirCreateInfoMESA nir_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir = cs,
   };
   const VkComputePipelineCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .pNext = &nir_info,
         .flags = device->varying_subgroups
                     ? VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT : 0,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = VK_NULL_HANDLE,
         /* the NIR entry point's own name, "main" (pipeline_for says why) */
         .pName = nir_shader_get_entrypoint(cs)->function->name,
      },
      .layout = compute->layout,
      .basePipelineIndex = -1,
   };
   VkPipeline p = VK_NULL_HANDLE;
   /* (A TRANSLATION KEPT ON DISK, air_to_nir.c) and what turnip made of it, one file a size of threadgroup: a cache of its
    * own for this one pipeline, so processes never have to merge theirs. */
   VkPipelineCache cache = VK_NULL_HANDLE;
   char cache_path[256];
   char suffix[40];
   static const uint8_t no_key[20];
   void *cached = NULL;
   size_t cached_size = 0;
   snprintf(suffix, sizeof(suffix), ".cs%ux%ux%u", block[0], block[1], block[2]);
   const bool keep = memcmp(compute->info.cache_key, no_key, sizeof(no_key)) != 0 &&
                     air_cache_path(compute->info.cache_key, suffix, cache_path, sizeof(cache_path));
   if (keep) {
      cached = air_cache_read(cache_path, &cached_size);
      const VkPipelineCacheCreateInfo cache_info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
         .initialDataSize = cached_size,
         .pInitialData = cached,
      };
      device->vk.CreatePipelineCache(device->dev, &cache_info, NULL, &cache);
   }
   VkResult r = device->vk.CreateComputePipelines(device->dev, cache, 1, &ci, NULL,
                                                  &p);
   if (cache != VK_NULL_HANDLE) {
      size_t made = 0;
      void *bytes = NULL;
      if (!cached && r == VK_SUCCESS &&
          device->vk.GetPipelineCacheData(device->dev, cache, &made, NULL) == VK_SUCCESS && made > 0 &&
          (bytes = malloc(made)) != NULL &&
          device->vk.GetPipelineCacheData(device->dev, cache, &made, bytes) == VK_SUCCESS)
         air_cache_write(cache_path, bytes, made);
      free(bytes);
      device->vk.DestroyPipelineCache(device->dev, cache, NULL);
   }
   free(cached);
   ralloc_free(cs);
   if (r != VK_SUCCESS) {
      say(why, why_size, "turnip would not make the compute pipeline of %s for threadgroups "
                         "of %ux%ux%u (%s)", compute->stage.name, block[0], block[1],
          block[2], vk_Result_to_str(r));
      return VK_NULL_HANDLE;
   }

   unsigned slot;
   if (compute->sizes < RWA_COMPUTE_SIZES) {
      slot = compute->sizes++;
   } else {
      /* all kept sizes are in use: the oldest goes, after everything that may still run
       * it has finished -- Vulkan's rule, and the one cost a Gallium state does not have
       * [M4-1, the cost part: and no longer a wait: it goes when the last dispatch of this
       * compute pipeline has finished, whatever size that was] */
      slot = compute->replace;
      compute->replace = (compute->replace + 1) % RWA_COMPUTE_SIZES;
      when_done(device, compute->use, pipeline_free, compute->size[slot].pipeline);
   }
   compute->size[slot].block[0] = block[0];
   compute->size[slot].block[1] = block[1];
   compute->size[slot].block[2] = block[2];
   compute->size[slot].pipeline = p;
   return p;
}

struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_compute_info *info, char *why,
                   size_t why_size)
{
   return compute_create(device, bitcode, size, entry, NULL, 0, 0, info, why, why_size);
}

struct rwa_compute *
rwa_compute_create_constants(struct rwa_device *device, const void *bitcode, size_t size,
                             const char *entry, const struct rwa_function_constant *constants,
                             unsigned constant_count, struct rwa_compute_info *info, char *why,
                             size_t why_size)
{
   return compute_create(device, bitcode, size, entry, constants, constant_count, 1, info, why,
                         why_size);
}

int
rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings,
             const unsigned groups[3], const unsigned group_size[3],
             char *why, size_t why_size)
{
   /* No buffer table, which is what every caller written before it existed means, as on
    * Gallium: every offset is zero. [M5c: a kernel with an argument buffer reaches this now, and
    * argbuf_member looks its set up in `bindings` at offset 0.] */
   return rwa_dispatch_buffers(compute, bindings, NULL, 0, groups, group_size, why,
                               why_size);
}

/*
 * One dispatch (COMPUTE, THE DISPATCH AND ITS ORDER): air_metal.c's checks in its order
 * and words, Vulkan's two limits, every binding judged before anything is recorded (so a
 * refused dispatch leaves the command buffer as it was), and then the draw's recording
 * shape with a compute bind point: barrier, copies of misaligned read-only buffers,
 * barrier, layouts, pipeline, push descriptors, vkCmdDispatch. Waited for here unless the
 * driver collects its own fence (rwa_device_defer_waits), as on Gallium.
 */
int
rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                     const struct rwa_buffer_bind *table, unsigned table_count,
                     const unsigned groups[3], const unsigned group_size[3],
                     char *why, size_t why_size)
{
   if (!compute || !groups || !group_size) {
      say(why, why_size, "a dispatch with no compute pipeline or no grid");
      return -1;
   }
   struct rwa_device *device = compute->device;
   const char *name = compute->info.entry_point ? compute->info.entry_point : "the shader";

   /* a caller with nothing to bind may say so with NULL, as on Gallium */
   static const struct rwa_bindings nothing = { 0 };
   if (!bindings)
      bindings = &nothing;

   for (unsigned i = 0; i < 3; i++) {
      if (groups[i] == 0 || group_size[i] == 0) {
         say(why, why_size, "a grid of %ux%ux%u threadgroups of %ux%ux%u threads has no "
                            "threads in it, so it would leave every buffer as it is",
             groups[0], groups[1], groups[2],
             group_size[0], group_size[1], group_size[2]);
         return -1;
      }
   }
   /* in 64 bits, so three large sides cannot wrap round into a small product */
   uint64_t threads = (uint64_t)group_size[0] * group_size[1] * group_size[2];
   if (compute->caps.max_threads_per_group && threads > compute->caps.max_threads_per_group) {
      say(why, why_size, "%ux%ux%u is %llu threads in one threadgroup and this GPU takes "
                         "at most %u", group_size[0], group_size[1], group_size[2],
          (unsigned long long)threads, compute->caps.max_threads_per_group);
      return -1;
   }
   if (compute->dynamic_threadgroup) {
      say(why, why_size, "%s takes a threadgroup buffer whose size only the dispatch can "
                         "give (Metal's -setThreadgroupMemoryLength:atIndex:), and this "
                         "call has no argument for it", name);
      return -1;
   }
   /* Vulkan's own limits, which Gallium has no counterpart of: a side of a group past
    * maxComputeWorkGroupSize or a side of the grid past maxComputeWorkGroupCount is not a
    * dispatch Vulkan allows at all (vkCmdDispatch's valid usage). */
   for (unsigned i = 0; i < 3; i++) {
      if (group_size[i] > device->max_group_size[i]) {
         say(why, why_size, "threadgroups of %ux%ux%u threads: %s takes at most %u in "
                            "dimension %u (maxComputeWorkGroupSize), and Vulkan allows no "
                            "larger group (M3d)", group_size[0], group_size[1],
             group_size[2], device->name, device->max_group_size[i], i);
         return -1;
      }
      if (groups[i] > device->max_group_count[i]) {
         say(why, why_size, "a grid of %ux%ux%u threadgroups: %s dispatches at most %u in "
                            "dimension %u (maxComputeWorkGroupCount), and Vulkan allows no "
                            "larger grid in one dispatch (M3d)", groups[0], groups[1],
             groups[2], device->name, device->max_group_count[i], i);
         return -1;
      }
   }

   /* Every binding judged before anything is recorded: resolve_binding, the draw's, with
    * no pass to collide with. */
   const struct rwa_stage *st = &compute->stage;
   unsigned total = stage_descriptors(st), n = 0;
   struct rwa_draw_binding *db = total ? calloc(total, sizeof(*db)) : NULL;
   if (total && !db) {
      say(why, why_size, "out of memory binding a dispatch");
      return -1;
   }
   if (!resolve_stage(device, st, bindings, table, table_count, NULL, db, &n, why, why_size))
      goto refused;
   if (!choose_texture_views(db, total, why, why_size))
      goto refused;

   VkPipeline pipeline = compute_pipeline_for(compute, group_size, why, why_size);
   if (!pipeline)
      goto refused;
   struct rwa_fence *cmd = record_begin(device, why, why_size);
   if (!cmd)
      goto refused;

   VkWriteDescriptorSet *writes = total ? calloc(total, sizeof(*writes)) : NULL;
   VkDescriptorBufferInfo *bufs = total ? calloc(total, sizeof(*bufs)) : NULL;
   VkDescriptorImageInfo *imgs = total ? calloc(total, sizeof(*imgs)) : NULL;
   if (total && (!writes || !bufs || !imgs)) {
      free(writes);
      free(bufs);
      free(imgs);
      say(why, why_size, "out of memory binding a dispatch");
      goto refused;
   }

   /* [M4-1, the cost part: what this dispatch uses named first, as a draw names it] */
   named(&compute->use, cmd);
   for (unsigned i = 0; i < total; i++) {
      if (db[i].buffer)
         named(&db[i].buffer->use, cmd);
      if (db[i].sampler_object)
         named(&db[i].sampler_object->use, cmd);
   }
   /* Everything earlier made visible to this dispatch (THE DISPATCH AND ITS ORDER), then
    * the descriptors, a misaligned read-only buffer copied into this command buffer's
    * upload memory first, in order, as a draw does it (RESOURCES, OFFSETS). */
   barrier_all(device, cmd->cmd);
   bool copied = false;
   for (unsigned i = 0; i < total; i++) {
      const struct rwa_binding *b = db[i].b;
      if (db[i].absent)
         continue;   /* M5c: no write: bind_descriptors */
      writes[i] = (VkWriteDescriptorSet){
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding = b->binding,
         .dstArrayElement = db[i].element,
         .descriptorCount = 1,
         .descriptorType = b->type,
      };
      if (b->kind == RWA_BIND_BUFFER) {
         VkBuffer target = db[i].buffer->raw.buffer;
         VkDeviceSize at = db[i].offset;
         if (db[i].copy) {
            VkDeviceSize size = db[i].range;
            if (!upload_alloc(device, cmd, size, &target, &at, why, why_size)) {
               free(writes);
               free(bufs);
               free(imgs);
               goto refused;
            }
            const VkBufferCopy region = { db[i].offset, at, size };
            device->vk.CmdCopyBuffer(cmd->cmd, db[i].buffer->raw.buffer, target, 1, &region);
            copied = true;
         }
         bufs[i] = (VkDescriptorBufferInfo){ target, at, db[i].range };
         writes[i].pBufferInfo = &bufs[i];
      } else if (b->kind == RWA_BIND_TEXTURE) {
         bool general = stages_write_image(db, total, db[i].texture);
         imgs[i] = (VkDescriptorImageInfo){
            .imageView = db[i].texture_view,
            .imageLayout = general ? VK_IMAGE_LAYOUT_GENERAL
                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         };
         writes[i].pImageInfo = &imgs[i];
      } else {
         imgs[i] = (VkDescriptorImageInfo){ .sampler = db[i].sampler };
         writes[i].pImageInfo = &imgs[i];
      }
   }
   if (copied)
      barrier_all(device, cmd->cmd);

   /* Each texture moved to the layout its descriptor names, behind a barrier whose source
    * is every earlier write, even when it is in that layout already: a storage image
    * written by the dispatch before is ordered against this one by nothing else. */
   for (unsigned i = 0; i < total; i++) {
      if (!db[i].texture)
         continue;
      bool general = stages_write_image(db, total, db[i].texture);
      image_barrier(db[i].texture, cmd->cmd,
                    general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    general ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                            : VK_ACCESS_SHADER_READ_BIT);
   }

   device->vk.CmdBindPipeline(cmd->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   const bool bound = bind_descriptors(device, cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       compute->ordinary_set, compute->set_layout, compute->layout,
                                       total, writes, why, why_size);
   free(writes);
   free(bufs);
   free(imgs);
   free(db);
   /* [M5c: the barriers are recorded and harmless; no set is no dispatch, and the sentence says] */
   if (!bound)
      return -1;
   device->vk.CmdDispatch(cmd->cmd, groups[0], groups[1], groups[2]);
   return recorded(device, "dispatch", why, why_size);

refused:
   free(db);
   return -1;
}


/* ---------------------------------------------------------------------------------- */
/* Everything else of the contract: present, so the driver's dlsym finds it, and       */
/* refusing by name.                                                                   */
/* ---------------------------------------------------------------------------------- */

/* rwa_compute_create and rwa_compute_destroy are with the dispatches above, and are real
 * since M3d; until then they answered not_yet here, "a compute pipeline
 * (rwa_compute_create) is not in the Vulkan back end yet (M3b)", and made none. */

/* rwa_texture_from_surface is with rwa_texture_create, and is real since M3e (TEXTURES
 * OVER AN IOSURFACE); until then it answered not_yet here, "a texture over an IOSurface
 * (rwa_texture_from_surface) is not in the Vulkan back end yet (M3b)", and made none. */

/* rwa_clear_depth_stencil is with rwa_clear_color, and is real since M3c-2. */

/* rwa_resolve_texture is with them too, and is real since M3c-3 (MULTISAMPLE, THE
 * RESOLVE); until then it answered not_yet here, "a multisample resolve
 * (rwa_resolve_texture) is not in the Vulkan back end yet (M3b)". */

/* rwa_depth_read is with rwa_texture_read and rwa_texture_write, and is real since M3c-2. */

/* rwa_module_argbufs and the encoder are with resolve_binding, and are real since M5c; until then
 * each answered not_yet here. */

/* rwa_dispatch and rwa_dispatch_buffers are with rwa_compute_create, and are real since
 * M3d; until then each answered not_yet here, "a compute dispatch (rwa_dispatch) is not in
 * the Vulkan back end yet (M3b)". */

/* rwa_depth_stencil_create and _destroy are with the samplers, and are real since M3c-2. */
