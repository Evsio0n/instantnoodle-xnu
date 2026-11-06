/*
 * The device object of a Metal driver for this port, as a real library.
 *
 * What Metal demands of a plugin's device class was measured rather than guessed
 * (docs/metal.md section 10): `MTLAddDevice` (0x181a1875c) wants an object that comes back
 * non-nil from -initWithAcceleratorPort:, conforms to MTLDevice and MTLDeviceSPI -- both
 * asserted, both fatal -- and answers -featureProfile, -maxBufferLength and
 * -supportsVertexAmplificationCount:. Four methods. Everything else in the registration
 * path, including the 64-field limits table and the 42 feature queries, _MTLDevice fills
 * in itself once -featureProfile answers.
 *
 * Three decisions in how this is written, each for a measured reason:
 *
 *   - The class is declared in SOURCE, not built with objc_allocateClassPair as the probes
 *     did. That needs the private class symbol at link time, and it is bindable: IOGPU
 *     itself imports `_OBJC_CLASS_$__MTLDevice` together with its metaclass. Only the name
 *     is declared here -- nothing about Apple's 36 ivars or their offsets, which belong to
 *     Metal 306.6.4 and are nowhere written down. Our own ivars are placed by the runtime
 *     after Apple's, which is what the non-fragile ABI is for.
 *   - `<MTLDeviceSPI>` is declared at RUNTIME, with class_addProtocol. The protocol is
 *     private and requires 337 instance methods; writing that declaration out would mean
 *     copying 337 signatures we do not implement, and the only thing the two assertions in
 *     MTLAddDevice read is conformsToProtocol:. One call says the same thing truthfully.
 *     MTLDeviceSPI adopts MTLDevice, so one declaration satisfies both.
 *   - -initWithAcceleratorPort: opens this port's OWN gpu service, `HKGPU`, the way
 *     Mesa's shim does (tools/mesa/hackios-drm/hk_drm.c: IOServiceOpen with type 0,
 *     then msm-shaped requests through IOConnectCallStructMethod). That is the road the
 *     resource layer will take, and it already carries Mesa's own dispatches today. The
 *     IOGPU road was measured shut: IOServiceOpen on our IOAcceleratorES service with
 *     Apple's type 1 answers kIOReturnUnsupported.
 *
 * The first resource is here too. A Metal buffer on this port is a GEM allocation of the
 * kernel's own gpu driver, made and mapped exactly the way Mesa's shim makes and maps one
 * (tools/mesa/hackios-drm/hk_drm.c): kMsmGemNew (0x42) for the memory, MSM_INFO_GET_IOVA
 * through kMsmGemInfo (0x43) for the address the GPU sees, IOConnectMapMemory64 for the
 * address the processor sees, kDrmGemClose (0x09) to give it back. The kernel makes that
 * memory write-combined, with no cache of this processor between the program and what the
 * GPU reads, which is also why -didModifyRange: has nothing to do.
 *
 * Metal.framework has no `_MTLBuffer` to inherit -- only `_MTLResource`, a thin base over
 * `_MTLObjectWithLabel` -- so the buffer class is ours, as it is in Apple's own drivers
 * (`IOGPUMetalBuffer`, `AGXA11FamilyBuffer`).
 *
 * The compute path is here too, and it is where subsystem (1) finally carries something.
 * Metal hands a driver one function's LLVM module through -[_MTLFunction bitcodeData], and
 * those bytes were checked against our own AIR reader over all eleven functions of
 * AV1SW.metallib -- same lengths, same checksums (docs/metal.md 10.12). The front end that
 * reads them lives in Mesa, so the compiling and the dispatching are asked of
 * /hackios/lib/libhackiosair.dylib through the contract in tools/mesa/air/air_metal.h.
 *
 * That library is loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU. The port starts the Adreno only with the
 * boot argument gpu=1, and a device that refuses to be created at all would take Metal's
 * whole discovery down with it; a device that answers its name and its limits and refuses
 * to make a pipeline is the truthful object in that case.
 *
 * Ordering is Metal's, not Gallium's: an encoder RECORDS its dispatches and the command
 * buffer runs them when it is committed. Dispatching as they are encoded would be less
 * code and would quietly reorder work an application committed in a particular order.
 *
 * Depth and stencil are here, and what that amounts to is four things: a texture of a depth
 * pixel format, an MTLDepthStencilState over the private _MTLDepthStencilState (whose SPI
 * has four methods Apple leaves answering NO, overridden here with the truth about the
 * descriptor), the render pass descriptor's depth and stencil attachments, and the
 * depth-range half of -setViewport:, which is the only thing in Metal that moves this
 * driver's quad in z at all.
 *
 * ARGUMENT BUFFERS are here, and what they are here is a RECORDER: Metal's encoder writes
 * 8-byte handles into a buffer and those are an Apple GPU's descriptors, so this driver
 * records (resource, Metal id) against (destination buffer, destination offset) instead
 * and the draw binds every member the pipeline's shaders declare. The buffer's contents
 * are never read, and an argument buffer the application did not fill through an encoder
 * is refused BY NAME (docs/air.md 8.2, and the MTLArgumentEncoder implementation at the
 * end of this file).
 *
 * BUFFER OFFSETS are honoured, which they were not: Metal's own `offset:` is recorded at
 * the encoder and judged at the draw, where the pipeline says whether an index is a
 * stream, a set or a buffer argument. A `constant` one is served at any offset -- the
 * compiler library uploads the block when the GPU cannot fetch from that offset -- and a
 * `device` one the GPU cannot fetch from is refused by name, because the shader may write
 * it and a copy would lose the writes.
 *
 * What is NOT here, and is not pretended: heaps, blit encoders, events, multisample
 * resolve, and the rectangle half of a viewport -- the rest of docs/metal.md 10.8.
 *
 * Every line this file writes carries the PROCESS it was written by
 * (`hkmetal[progname pid]:`, see rw_print): a driver is a library, four or five
 * Metal clients are alive in one boot of this port, and a refusal nobody can attribute
 * is half a refusal.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>          /* for MTLSize and the public enumerations, nothing private */
#import <objc/message.h>
#import <objc/runtime.h>
#include <arm_neon.h>
#include <notify.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <dlfcn.h>
#include <pthread.h>

#include <sys/stat.h>
#include "hk_drawable.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* The contract with Mesa's side, copied here rather than included: this file is built by
 * userland/build.sh, which knows nothing of Mesa's tree, and the declarations are few.
 * tools/mesa/air/air_metal.h is the original and explains each call. */
struct rwa_device;
struct rwa_buffer;
struct rwa_texture;
struct rwa_sampler;
struct rwa_compute;
struct rwa_fence;
/* (FUNCTION STITCHING: air_metal.h) */
struct rwa_stitch_input {
	unsigned kind;
	unsigned bind, offset, node;
};
struct rwa_stitch_node {
	const char *name;
	const void *bitcode;
	size_t size;
	bool early_return;
	unsigned num_inputs;
	const struct rwa_stitch_input *inputs;
};

struct rwa_compute_info {
	unsigned max_threads_per_group, thread_execution_width, static_shared_bytes;
	unsigned buffers, textures, samplers;
	/* which of those buffer indices are ARGUMENT BUFFERS, one bit each: Metal has one
	 * buffer table and an index in it is either a set or an argument. Appended, so
	 * every field above keeps the offset this file already reads -- and this file and
	 * the library still have to be rebuilt together. */
	unsigned argbuf_mask;
};
struct rwa_texture_shape {
	unsigned pixel_format, type, width, height, depth;
	unsigned array_length, mip_levels, sample_count, usage;
};
/* A view (M5b): the one call the contract grew for it, which the Gallium library has not got, so
 * dlsym finds nothing there and a view is nil on that back end as it has always been. */
struct rwa_texture_view_shape {
	unsigned pixel_format, type;
	unsigned base_level, level_count, base_slice, slice_count;
	unsigned swizzle[4];
};
struct rwa_sampler_state {
	unsigned address[3];
	unsigned min_filter, mag_filter, mip_filter, compare_function, max_anisotropy;
	unsigned normalized_coordinates;
	float lod_min, lod_max;
	/* MTLSamplerDescriptor.borderColor (M5b). Appended: the Gallium library reads the prefix
	 * and its border is transparent black, which -newSamplerStateWithDescriptor: says. */
	unsigned border_color;
};
struct rwa_bindings {
	struct rwa_buffer *const *buffers;
	unsigned buffer_count;
	struct rwa_texture *const *textures;
	unsigned texture_count;
	struct rwa_sampler *const *samplers;
	unsigned sampler_count;
};
/* ARGUMENT BUFFERS. There is no argument buffer to write on this driver: the frontend
 * resolves every member of a set to a binding while translating, and Metal's 8-byte
 * handles are an Apple GPU's descriptors, which a Gallium driver cannot read. So
 * MTLArgumentEncoder RECORDS (resource, Metal id) against (destination buffer,
 * destination offset) and the draw binds what it finds -- air_metal.h carries the whole
 * of why, including why an argument buffer nobody encoded has to be refused by name. */
#define RWA_MAX_ARGBUF_MEMBERS 64
enum rwa_argbuf_kind {
	RWA_ARGBUF_BUFFER = 0,
	RWA_ARGBUF_TEXTURE = 1,
	RWA_ARGBUF_SAMPLER = 2,
};
struct rwa_argbuf_member {
	unsigned id;             /* Metal's [[id(n)]], which the application names */
	unsigned kind;           /* enum rwa_argbuf_kind */
	unsigned index;          /* the flat index the frontend allocated */
	unsigned write;
	char name[48];
};
struct rwa_argbuf_layout {
	unsigned metal_index;    /* the [[buffer(n)]] the set is bound at */
	unsigned encoded_length; /* 8 bytes per member the set declares: Apple's own number */
	unsigned alignment;
	unsigned member_count;
	struct rwa_argbuf_member members[RWA_MAX_ARGBUF_MEMBERS];
};
/* Metal's buffer table for one stage: what is bound at each index AND AT WHAT OFFSET,
 * which `rwa_bindings.buffers` has no room for. One array for both of the things an
 * offset means, because Metal has one -setFragmentBuffer:offset:atIndex: behind both: for
 * an argument buffer it is WHICH ENCODING in that buffer to read, and for a buffer
 * argument it is which block of it the shader's pointer starts at. The library serves a
 * `constant` one at any offset (uploading when the GPU cannot fetch from it) and refuses
 * a `device` one the GPU cannot fetch from, by name -- air_metal.h says why. */
struct rwa_buffer_bind {
	struct rwa_buffer *buffer;   /* what the application bound at this Metal index */
	size_t offset;               /* Metal's own `offset:` */
};
struct rwa_render;
struct rwa_depth_stencil;
/* MTLVertexDescriptor's two arrays, indexed by Metal's own indices: attributes[n] by the
 * shader's [[attribute(n)]] and layouts[k] by the buffer index an attribute names. A
 * format of MTLVertexFormatInvalid (0) is "not configured", which is what a fresh
 * MTLVertexDescriptor holds everywhere, so a zeroed shape carries no layout at all. */
#define RWA_MAX_VERTEX_ATTRIBUTES 31
#define RWA_MAX_VERTEX_BUFFERS    31
struct rwa_vertex_attribute {
	unsigned format, offset, buffer_index;
};
struct rwa_vertex_layout {
	unsigned stride, step_function, step_rate;
};
/* MTLRenderPipelineColorAttachmentDescriptor's blend half, per colour attachment, as
 * Metal's own numbers: the library maps MTLBlendFactor, MTLBlendOperation and
 * MTLColorWriteMask by table and refuses what it cannot serve by name. `write_mask` is
 * MTLColorWriteMask, whose bits run the OTHER WAY from Gallium's colormask (Metal's Red is
 * 1 << 3) -- which is the library's business and not this driver's: this passes the number
 * Metal gave it and reverses nothing. */
struct rwa_color_blend {
	unsigned blending_enabled;
	unsigned rgb_blend_operation, alpha_blend_operation;
	unsigned source_rgb_blend_factor, destination_rgb_blend_factor;
	unsigned source_alpha_blend_factor, destination_alpha_blend_factor;
	unsigned write_mask;
};
struct rwa_function_constant {
	unsigned index;
	uint64_t bits;
	uint64_t bits_hi;
};
struct rwa_function_constant_decl {
	unsigned index;
	unsigned required;
	char type[24];
	char name[72];
};
struct rwa_render_shape {
	unsigned color_format[8];
	unsigned color_count;
	unsigned depth_format;
	unsigned stencil_format;
	unsigned sample_count;
	struct rwa_vertex_attribute vertex_attribute[RWA_MAX_VERTEX_ATTRIBUTES];
	struct rwa_vertex_layout vertex_layout[RWA_MAX_VERTEX_BUFFERS];
	/* APPENDED, 260 bytes at the END: every field above keeps the offset this driver
	 * already writes. A field inserted in the MIDDLE of a struct shared with the library
	 * has cost this port twice -- a primitive type that arrived as 1871292816, and
	 * struct air_resource growing by eight bytes so that every resource after the first
	 * was read eight bytes early. `color_blend_count` of 0 means this shape carries NO
	 * blend state, which is blending off and all four channels written; a zeroed entry
	 * cannot stand in for that, because MTLColorWriteMaskNone is 0 and Metal's default
	 * writeMask is All. */
	struct rwa_color_blend color_blend[8];
	unsigned color_blend_count;
	/* M5c, appended: what MTLFunctionConstantValues said (air_metal.h) */
	const struct rwa_function_constant *vertex_constants;
	const struct rwa_function_constant *fragment_constants;
	unsigned vertex_constant_count;
	unsigned fragment_constant_count;
	unsigned vertex_constants_given;
	unsigned fragment_constants_given;
};
struct rwa_render_info {
	unsigned vertex_buffers, fragment_buffers, fragment_textures, fragment_samplers;
	unsigned generated_vertex_stage;
	unsigned fragment_writes_depth;
	/* what the vertex descriptor became: how many elements were built, the Metal
	 * attribute index of each (in driver_location order, which is the shader's and not
	 * Metal's), how many stream slots a draw must cover and which of them are fetched
	 * from. The mask is what splits Metal's one buffer table at the draw. */
	unsigned vertex_attribute_count;
	unsigned vertex_attribute_index[16];
	unsigned vertex_streams;
	unsigned vertex_stream_mask;
	/* and which buffer indices of each stage are argument buffers. Metal's one table per
	 * stage now holds three things -- a vertex stream, a buffer argument and a set --
	 * and only the shader and the vertex descriptor together say which an index is. */
	unsigned vertex_argbuf_mask;
	unsigned fragment_argbuf_mask;
	/* and what the blend state became: one bit per attachment that ends up blending, and
	 * four bits per attachment of colour write mask in GALLIUM's order (R 0x1 .. A 0x8),
	 * which is MTLColorWriteMask's reversed. Printed rather than trusted: for any mask
	 * but All, None, green|blue and red|alpha these four bits MUST differ from the
	 * writeMask this driver passed, so a log line where they agree is a log line saying
	 * the bits were never reversed. Filled in BY the library, so this struct must not be
	 * shorter than the library's. */
	unsigned blend_enabled_mask;
	unsigned color_write_masks;
};
struct rwa_vertex_stream {
	struct rwa_buffer *buffer;
	size_t offset;
};
struct rwa_stencil_face {
	unsigned compare_function;
	unsigned stencil_fail_operation;
	unsigned depth_fail_operation;
	unsigned depth_stencil_pass_operation;
	unsigned read_mask, write_mask;
};
struct rwa_depth_stencil_state {
	unsigned depth_compare_function;
	unsigned depth_write_enabled;
	struct rwa_stencil_face front, back;
};
struct rwa_draw {
	struct rwa_texture *const *color;
	unsigned color_count;
	const float *clear_color;
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
	const struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	const float *depth_range;
	struct rwa_bindings vertex;
	struct rwa_bindings fragment;
	unsigned primitive;
	unsigned vertex_start, vertex_count;
	/* the indexed forms: -drawIndexedPrimitives: and its instanced ones. Appended, as
	 * air_metal.h says, so that every field above keeps the offset this driver already
	 * reads -- but the library and this file still have to be rebuilt together, and
	 * `vertex_count` is the INDEX count for an indexed draw (there is deliberately no
	 * second count field for the two to disagree in). */
	struct rwa_buffer *index_buffer;
	unsigned index_type;
	size_t index_buffer_offset;
	int base_vertex;
	unsigned instance_count;
	unsigned base_instance;
	/* the vertex streams: -setVertexBuffer:offset:atIndex:, indexed by that index, and
	 * a second array beside `vertex.buffers` because a stream carries an OFFSET and is
	 * checked against the layout rather than against the shader's [[buffer(n)]]. Both
	 * come off the one table Metal has, and the pipeline's vertex_stream_mask says
	 * which index belongs to which side. */
	const struct rwa_vertex_stream *vertex_streams;
	unsigned vertex_stream_count;
	/* Metal's buffer table for each stage: the offsets `vertex.buffers` and
	 * `fragment.buffers` have nowhere to put -- an argument buffer's encoding offset
	 * and a buffer argument's own offset alike. APPENDED rather than put inside
	 * rwa_bindings, which is embedded by value above: a field added there would move
	 * `primitive` and every index field after it, which is how a primitive type once
	 * arrived as 1871292816. */
	const struct rwa_buffer_bind *vertex_buffer_table;
	unsigned vertex_buffer_table_count;
	const struct rwa_buffer_bind *fragment_buffer_table;
	unsigned fragment_buffer_table_count;
	/* the encoder's own state (M5a, air_metal.h): the Vulkan library honours these, the
	 * Gallium library was built against the shorter struct and reads none of them, so
	 * what it draws does not change. Metal's own numbers; zero or NULL is Metal's default. */
	const float *blend_color;
	unsigned cull_mode;
	unsigned front_winding;
	const float *viewport;
	const unsigned *scissor;
	const float (*color_clears)[4];
	unsigned color_clear_mask;
	/* where in each attachment (M5b, air_metal.h): Metal's level, and its slice or depth plane,
	 * which the type tells apart. Appended, and read by the Vulkan library alone: the Gallium
	 * one renders level 0, slice 0, and this file says so once when asked for another. */
	const unsigned *color_level;
	const unsigned *color_slice;
	unsigned depth_level;
	unsigned depth_slice;
	/* one pass for many draws (M5g, air_metal.h) */
	unsigned fresh_pass;
	unsigned dont_care_mask;
};
#define RWA_DONT_CARE_DEPTH   (1u << 8)
#define RWA_DONT_CARE_STENCIL (1u << 9)
/* where a frame's time goes (M5g, air_metal.h has what each is) */
enum rwa_stat_index {
	RWA_STAT_DRAWS, RWA_STAT_PASSES, RWA_STAT_BREAK_TARGET, RWA_STAT_BREAK_CLEAR, RWA_STAT_BREAK_LAYOUT,
	RWA_STAT_BREAK_SHARED, RWA_STAT_BREAK_COPY, RWA_STAT_BREAK_BARRIER, RWA_STAT_COPIES, RWA_STAT_COPY_BYTES,
	RWA_STAT_SUBMITS, RWA_STAT_PIPELINES, RWA_STAT_PIPELINE_US, RWA_STAT_DISPATCHES, RWA_STAT_BUFFERS,
	RWA_STAT_BUFFER_US, RWA_STAT_SUBMIT_US, RWA_STAT_APART_FETCH, RWA_STAT_APART_WRITES, RWA_STAT_APART_SHARES, RWA_STAT_TEXTURES, RWA_STAT_TEXTURE_US, RWA_STAT_TEXTURES_KEPT, RWA_STAT_COUNT
};


/*
 * The library's MTLVertexFormat table, checked against Apple's own header HERE, where the
 * SDK is in hand: tools/mesa/air/air_metal.c carries those numbers as literals (it has no
 * Metal headers to read them from), this file is compiled against
 * iPhoneOS16.5.sdk/System/Library/Frameworks/Metal.framework/Headers/MTLVertexDescriptor.h,
 * and a disagreement between the two would be a vertex fetched in the wrong format rather
 * than anything that fails. So it is a build failure of this driver instead.
 *
 * Nothing in this file switches on these names; they are asserted and otherwise the
 * numbers pass through as the application gave them, exactly as MTLPixelFormat does.
 */
_Static_assert((int)MTLVertexFormatInvalid == 0 && (int)MTLVertexFormatUChar2 == 1 &&
		   (int)MTLVertexFormatChar2 == 4 &&
		   (int)MTLVertexFormatUChar2Normalized == 7 &&
		   (int)MTLVertexFormatChar2Normalized == 10,
	       "MTLVertexFormat's 8-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUShort2 == 13 && (int)MTLVertexFormatShort2 == 16 &&
		   (int)MTLVertexFormatUShort2Normalized == 19 &&
		   (int)MTLVertexFormatShort2Normalized == 22 &&
		   (int)MTLVertexFormatHalf2 == 25,
	       "MTLVertexFormat's 16-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatFloat == 28 && (int)MTLVertexFormatFloat4 == 31 &&
		   (int)MTLVertexFormatInt == 32 && (int)MTLVertexFormatUInt == 36 &&
		   (int)MTLVertexFormatInt1010102Normalized == 40 &&
		   (int)MTLVertexFormatUInt1010102Normalized == 41 &&
		   (int)MTLVertexFormatUChar4Normalized_BGRA == 42,
	       "MTLVertexFormat's 32-bit forms are not where air_metal.c's table has them");
_Static_assert((int)MTLVertexFormatUChar == 45 && (int)MTLVertexFormatChar == 46 &&
		   (int)MTLVertexFormatUCharNormalized == 47 &&
		   (int)MTLVertexFormatCharNormalized == 48 &&
		   (int)MTLVertexFormatUShort == 49 && (int)MTLVertexFormatShort == 50 &&
		   (int)MTLVertexFormatUShortNormalized == 51 &&
		   (int)MTLVertexFormatShortNormalized == 52 &&
		   (int)MTLVertexFormatHalf == 53,
	       "MTLVertexFormat's one-component forms are not where air_metal.c's table "
	       "has them");
_Static_assert((int)MTLVertexStepFunctionConstant == 0 &&
		   (int)MTLVertexStepFunctionPerVertex == 1 &&
		   (int)MTLVertexStepFunctionPerInstance == 2 &&
		   (int)MTLVertexStepFunctionPerPatch == 3 &&
		   (int)MTLVertexStepFunctionPerPatchControlPoint == 4,
	       "MTLVertexStepFunction is not what air_metal.c's refusals name");

/*
 * THE BLEND ENUMERATIONS, asserted for the same reason and with more riding on it: the
 * compiler library maps all three BY TABLE, and if Apple's numbering were not what those
 * tables assume the result would be a composite with the wrong arithmetic in it rather
 * than anything that fails. MTLBlendFactor and pipe_blendfactor agree on only two of
 * nineteen values, so there is no cast to fall back on and no accident that would be
 * harmless.
 *
 * This is one of the two independent sources for the numbering, and it is the one that
 * can be made into a BUILD FAILURE: the SDK header compiled against here is
 * iPhoneOS16.5.sdk's own MTLRenderPipeline.h, the release this driver runs on.
 *
 * MTLColorWriteMask is the one to look at twice. Its bits run from Alpha at 1 << 0 up to
 * Red at 1 << 3, which is the OPPOSITE ORDER from Gallium's colormask, where red is 0x1.
 * Four of the sixteen masks are palindromes and survive the confusion -- None, All,
 * green|blue and red|alpha -- so a driver that got this backwards would still pass any
 * test built out of "write everything" and "write nothing".
 */
_Static_assert((int)MTLBlendFactorZero == 0 && (int)MTLBlendFactorOne == 1 &&
		   (int)MTLBlendFactorSourceColor == 2 &&
		   (int)MTLBlendFactorOneMinusSourceColor == 3 &&
		   (int)MTLBlendFactorSourceAlpha == 4 &&
		   (int)MTLBlendFactorOneMinusSourceAlpha == 5 &&
		   (int)MTLBlendFactorDestinationColor == 6 &&
		   (int)MTLBlendFactorOneMinusDestinationColor == 7 &&
		   (int)MTLBlendFactorDestinationAlpha == 8 &&
		   (int)MTLBlendFactorOneMinusDestinationAlpha == 9 &&
		   (int)MTLBlendFactorSourceAlphaSaturated == 10,
	       "MTLBlendFactor's served values are not where air_metal.c's table has them");
_Static_assert((int)MTLBlendFactorBlendColor == 11 &&
		   (int)MTLBlendFactorOneMinusBlendColor == 12 &&
		   (int)MTLBlendFactorBlendAlpha == 13 &&
		   (int)MTLBlendFactorOneMinusBlendAlpha == 14 &&
		   (int)MTLBlendFactorSource1Color == 15 &&
		   (int)MTLBlendFactorOneMinusSource1Color == 16 &&
		   (int)MTLBlendFactorSource1Alpha == 17 &&
		   (int)MTLBlendFactorOneMinusSource1Alpha == 18,
	       "MTLBlendFactor's refused values are not where air_metal.c's refusals name "
	       "them, so a refusal would name the wrong factor");
_Static_assert((int)MTLBlendOperationAdd == 0 && (int)MTLBlendOperationSubtract == 1 &&
		   (int)MTLBlendOperationReverseSubtract == 2 &&
		   (int)MTLBlendOperationMin == 3 && (int)MTLBlendOperationMax == 4,
	       "MTLBlendOperation is not the identity air_metal.c's table states it is");
_Static_assert((int)MTLColorWriteMaskNone == 0 && (int)MTLColorWriteMaskAlpha == 1 &&
		   (int)MTLColorWriteMaskBlue == 2 && (int)MTLColorWriteMaskGreen == 4 &&
		   (int)MTLColorWriteMaskRed == 8 && (int)MTLColorWriteMaskAll == 0xf,
	       "MTLColorWriteMask's bits are not the reverse of Gallium's colormask, which "
	       "is what air_metal.c's table is");

/*
 * AND THE SHAPE OF THE TWO STRUCTS THIS FILE TYPES OUT BY HAND, which is the bug this
 * port has paid for twice: a primitive type that arrived as 1871292816, and struct
 * air_resource growing by eight bytes so that every resource after the first was read
 * eight bytes early.
 *
 * The numbers below are the same numbers asserted in tools/mesa/air/air_metal.c against
 * the library's own definitions. Neither file can see the other's structs, so this is
 * not a comparison -- it is two independent statements of one agreement, and an edit to
 * one side alone fails to build on that side. The offsets are asserted as well as the
 * sizes, because the whole discipline here is that a new field goes on the END: a field
 * inserted in the middle keeps the size and moves everything after it, which is exactly
 * the failure that is invisible.
 */
_Static_assert(sizeof(struct rwa_color_blend) == 32 &&
		   sizeof(struct rwa_render_shape) == 1088 &&
		   offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
		   offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
		   offsetof(struct rwa_render_shape, color_blend) == 792 &&
		   offsetof(struct rwa_render_shape, color_blend_count) == 1048,
	       "struct rwa_render_shape here is not the shape air_metal.c asserts: a field "
	       "was added in the middle, or only one of the two copies was rebuilt");
_Static_assert(sizeof(struct rwa_render_info) == 116 &&
		   offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
		   offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
	       "struct rwa_render_info here is not the shape air_metal.c asserts, and this "
	       "one the LIBRARY writes into: too short is a buffer overrun");

/*
 * WHOSE SENTENCE THIS IS. Every line this driver writes goes through here, and the reason
 * is an attribution that could not be made: a boot's log held
 *
 *     hkmetal: fragment buffer 2 at offset 1164: ...
 *     hkmetal: no drawable: pixel format 500 is not one ...
 *
 * one after the other, and four or five Metal clients were alive in that boot (backboardd,
 * SpringBoard, chronod and this port's own application). The drawable refusal was pinned on
 * backboardd only by reading QuartzCore's own os_log line beside it, which is a guess, and
 * the offset refusals could not be attributed at all. A driver is a library: its lines are
 * written by somebody else's process, and the process is the first thing a reader needs.
 *
 * So the prefix is `hkmetal[progname pid]:`, built once per process -- getprogname()
 * is the name launchd or the shell gave it, which is what the log's other lines carry too.
 *
 * It flushes, for the reason RW_ONCE below flushes: stdio to a console is block-buffered,
 * the processes that reach these sites are the ones that then abort, and a line written and
 * not flushed dies with the process. One flush per sentence costs nothing next to the draw
 * that provoked it.
 */
static void rw_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hk_console_line(const char *line, size_t length);
/* and the one method this driver installs into Metal's own function class, whose reason
 * is a measurement and is written where it is done (the end of this file) */
static void ensure_function_encoder(void);

static void
rw_print(const char *fmt, ...)
{
	static char who[96];
	va_list ap;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "hkmetal[%s %d]", getprogname(), getpid());
	}
	/* (an application's stdout is /dev/null, and what Metal refused an application is what is wanted: such a process says it on the console) */
	static int nowhere = -1;

	if (nowhere < 0) {
		struct stat out, null;

		nowhere = fstat(1, &out) != 0 || (stat("/dev/null", &null) == 0 && out.st_rdev == null.st_rdev);
	}
	if (nowhere) {
		char line[512];
		int at = snprintf(line, sizeof(line), "%s: ", who);

		va_start(ap, fmt);
		vsnprintf(line + at, sizeof(line) - (size_t)at, fmt, ap);
		va_end(ap);
		hk_console_line(line, strlen(line));
		return;
	}
	printf("%s: ", who);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void hk_serve_function_constants(void);   /* M5c: FUNCTION CONSTANTS, below */
static void hk_say_traps(void);                  /* M5d, below */

static struct {
	void *handle;
	struct rwa_device *(*device_open)(char *, size_t);
	void (*device_close)(struct rwa_device *);
	const char *(*device_name)(struct rwa_device *);
	struct rwa_buffer *(*buffer_create)(struct rwa_device *, size_t, char *, size_t);
	struct rwa_texture *(*texture_over_buffer)(struct rwa_buffer *, const struct rwa_texture_shape *, size_t, unsigned, char *, size_t);
	void *(*buffer_map)(struct rwa_buffer *);
	size_t (*buffer_size)(struct rwa_buffer *);
	void (*buffer_destroy)(struct rwa_buffer *);
	int (*buffer_busy)(struct rwa_buffer *);               /* (a library before 33ee0e40: none, and no buffers kept) */
	int (*vk_memory)(struct rwa_device *, char *, size_t);
	struct rwa_compute *(*compute_create)(struct rwa_device *, const void *, size_t,
					      const char *, struct rwa_compute_info *, char *,
					      size_t);
	void (*compute_destroy)(struct rwa_compute *);
	struct rwa_texture *(*texture_create)(struct rwa_device *,
					      const struct rwa_texture_shape *, char *, size_t);
	int (*texture_write)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			     const unsigned[3], const void *, size_t, char *, size_t);
	int (*texture_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], void *, size_t, char *, size_t);
	int (*depth_read)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			  const unsigned[3], float *, uint8_t *, char *, size_t);
	void (*texture_destroy)(struct rwa_texture *);
	struct rwa_texture *(*texture_view_create)(struct rwa_texture *,
						   const struct rwa_texture_view_shape *, char *,
						   size_t);
	/* M5c: a library without these has no specialized function (FUNCTION CONSTANTS) */
	int (*module_function_constants)(struct rwa_device *, const void *, size_t,
					 struct rwa_function_constant_decl *, unsigned, unsigned *,
					 char *, size_t);
	struct rwa_compute *(*compute_create_stitched)(struct rwa_device *, const char *, const struct rwa_stitch_node *, unsigned,
						       struct rwa_compute_info *, char *, size_t);
	struct rwa_compute *(*compute_create_constants)(struct rwa_device *, const void *, size_t,
							const char *,
							const struct rwa_function_constant *,
							unsigned, struct rwa_compute_info *, char *,
							size_t);
	struct rwa_sampler *(*sampler_create)(struct rwa_device *,
					      const struct rwa_sampler_state *, char *, size_t);
	void (*sampler_destroy)(struct rwa_sampler *);
	int (*dispatch)(struct rwa_compute *, const struct rwa_bindings *,
			const unsigned[3], const unsigned[3], char *, size_t);
	int (*dispatch_buffers)(struct rwa_compute *, const struct rwa_bindings *,
				const struct rwa_buffer_bind *, unsigned, const unsigned[3],
				const unsigned[3], char *, size_t);
	int (*module_argbufs)(struct rwa_device *, const void *, size_t, const char *,
			      struct rwa_argbuf_layout *, unsigned, unsigned *, char *,
			      size_t);
	int (*argbuf_encode_buffer)(struct rwa_buffer *, size_t, unsigned,
				    struct rwa_buffer *, size_t, char *, size_t);
	int (*argbuf_encode_texture)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_texture *, char *, size_t);
	int (*argbuf_encode_sampler)(struct rwa_buffer *, size_t, unsigned,
				     struct rwa_sampler *, char *, size_t);
	unsigned (*argbuf_encoded_members)(const struct rwa_buffer *, size_t);
	struct rwa_render *(*render_create)(struct rwa_device *, const void *, size_t,
					    const char *, const void *, size_t, const char *,
					    const struct rwa_render_shape *,
					    struct rwa_render_info *, char *, size_t);
	void (*render_destroy)(struct rwa_render *);
	int (*render_draw)(struct rwa_render *, const struct rwa_draw *, char *, size_t);
	struct rwa_depth_stencil *(*depth_stencil_create)(struct rwa_device *,
							  const struct rwa_depth_stencil_state *,
							  char *, size_t);
	void (*depth_stencil_destroy)(struct rwa_depth_stencil *);
	int (*finish)(struct rwa_device *, char *, size_t);
	void (*stats_read)(uint64_t *, unsigned);
	int (*texture_prepare)(struct rwa_texture *, struct rwa_texture *const *, unsigned, struct rwa_texture *,
			       char *, size_t);
	/* one submission and one fence per command buffer; optional, and without them every
	 * draw and dispatch waits for itself, as an older library does */
	void (*defer_waits)(struct rwa_device *, int);
	int (*flush)(struct rwa_device *, struct rwa_fence **, char *, size_t);
	int (*fence_wait)(struct rwa_device *, struct rwa_fence *, char *, size_t);
	void (*fence_release)(struct rwa_device *, struct rwa_fence *);
	/* the blits (air_metal.h); looked up but not required, like drawing */
	int (*copy_buffer)(struct rwa_buffer *, size_t, struct rwa_buffer *, size_t, size_t, char *,
			   size_t);
	int (*fill_buffer)(struct rwa_buffer *, size_t, size_t, uint8_t, char *, size_t);
	int (*copy_texture)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
			    const unsigned[3], struct rwa_texture *, unsigned, unsigned,
			    const unsigned[3], char *, size_t);
	int (*copy_buffer_to_texture)(struct rwa_buffer *, size_t, size_t, size_t, const unsigned[3],
				      struct rwa_texture *, unsigned, unsigned, const unsigned[3],
				      char *, size_t);
	int (*copy_texture_to_buffer)(struct rwa_texture *, unsigned, unsigned, const unsigned[3],
				      const unsigned[3], struct rwa_buffer *, size_t, size_t, size_t,
				      char *, size_t);
	int (*generate_mipmaps)(struct rwa_texture *, char *, size_t);
	int (*clear_color)(struct rwa_texture *, const double[4], char *, size_t);
	int (*clear_depth_stencil)(struct rwa_texture *, const float *, const unsigned *, char *,
				   size_t);
	int (*resolve_texture)(struct rwa_texture *, struct rwa_texture *, char *, size_t);
} mesa;

/* Loaded once, and its absence is an answer rather than a crash: without the GPU there is
 * no library worth loading, and the device object still has to exist. */
/* Which of the two libraries mesa_available chose. It decides one thing in this file, what is
 * said about a viewport's rectangle: the Vulkan library honours it (M5a) and Gallium's draws the
 * whole attachment, and says so as it always has. */
static bool mesa_is_vulkan;

static bool
mesa_available(void)
{
	static bool tried;

	if (tried) {
		return mesa.handle != NULL;
	}
	tried = true;
	/*
	 * WHICH LIBRARY: the contract (tools/mesa/air/air_metal.h) has two implementations
	 * with the same rwa_* names -- libhackiosair.dylib over freedreno's Gallium driver,
	 * and libvulkan_freedreno.dylib, Mesa's Vulkan driver with tools/mesa/air/air_vk.c
	 * linked into it (Metal over Vulkan, milestone M3a). The boot-arg metal_backend=vk
	 * picks the second for the whole boot. HK_METAL_BACKEND=vk|gallium in the environment
	 * wins over the boot-arg, and is for test programs: one boot can then run the same
	 * test once per back end (metal_drv_test --backend sets it before it makes a device).
	 * Anything else in the variable is said and ignored, rather than read as one of them.
	 */
	char bootargs[1024] = "";
	size_t bootargs_size = sizeof(bootargs) - 1;
	const char *env = getenv("HK_METAL_BACKEND");
	bool vk;
	const char *chosen_by;

	if (env != NULL && strcmp(env, "vk") == 0) {
		vk = true;
		chosen_by = "HK_METAL_BACKEND=vk";
	} else if (env != NULL && strcmp(env, "gallium") == 0) {
		vk = false;
		chosen_by = "HK_METAL_BACKEND=gallium";
	} else {
		if (env != NULL && *env != '\0') {
			rw_print("HK_METAL_BACKEND=%s is neither vk nor gallium, so the boot-args "
			         "decide\n", env);
		}
		vk = sysctlbyname("kern.bootargs", bootargs, &bootargs_size, NULL, 0) == 0 &&
		     strstr(bootargs, "metal_backend=vk") != NULL;
		chosen_by = vk ? "the boot-arg metal_backend=vk" : "the default";
	}
	const char *library = vk ? "/hackios/lib/libvulkan_freedreno.dylib"
	                         : "/hackios/lib/libhackiosair.dylib";

	mesa_is_vulkan = vk;

	rw_print("the compiler library is %s (%s, by %s)\n", library,
	         vk ? "Metal over Vulkan" : "Metal over Gallium", chosen_by);
	mesa.handle = dlopen(library, RTLD_NOW);
	if (mesa.handle == NULL) {
		rw_print("no compiler library: %s\n", dlerror());
		return false;
	}
	*(void **)&mesa.device_open = dlsym(mesa.handle, "rwa_device_open");
	*(void **)&mesa.device_close = dlsym(mesa.handle, "rwa_device_close");
	*(void **)&mesa.device_name = dlsym(mesa.handle, "rwa_device_name");
	*(void **)&mesa.buffer_create = dlsym(mesa.handle, "rwa_buffer_create");
	*(void **)&mesa.texture_over_buffer = dlsym(mesa.handle, "rwa_texture_over_buffer");
	*(void **)&mesa.buffer_map = dlsym(mesa.handle, "rwa_buffer_map");
	*(void **)&mesa.buffer_size = dlsym(mesa.handle, "rwa_buffer_size");
	*(void **)&mesa.buffer_destroy = dlsym(mesa.handle, "rwa_buffer_destroy");
	*(void **)&mesa.buffer_busy = dlsym(mesa.handle, "rwa_buffer_busy");
	*(void **)&mesa.vk_memory = dlsym(mesa.handle, "rwa_vk_memory");
	*(void **)&mesa.compute_create = dlsym(mesa.handle, "rwa_compute_create");
	*(void **)&mesa.compute_destroy = dlsym(mesa.handle, "rwa_compute_destroy");
	*(void **)&mesa.texture_create = dlsym(mesa.handle, "rwa_texture_create");
	*(void **)&mesa.texture_write = dlsym(mesa.handle, "rwa_texture_write");
	*(void **)&mesa.texture_read = dlsym(mesa.handle, "rwa_texture_read");
	*(void **)&mesa.depth_read = dlsym(mesa.handle, "rwa_depth_read");
	*(void **)&mesa.texture_destroy = dlsym(mesa.handle, "rwa_texture_destroy");
	*(void **)&mesa.sampler_create = dlsym(mesa.handle, "rwa_sampler_create");
	*(void **)&mesa.sampler_destroy = dlsym(mesa.handle, "rwa_sampler_destroy");
	*(void **)&mesa.dispatch = dlsym(mesa.handle, "rwa_dispatch");
	/* The argument-buffer calls, looked up but NOT required, for the reason drawing is
	 * not required: a library without them is a real state of this work, and the
	 * encoder says so when an application asks for one rather than the driver refusing
	 * to load. What must not happen is answering an encoder and then binding nothing,
	 * so -newArgumentEncoder... checks for these before it makes one. */
	*(void **)&mesa.dispatch_buffers = dlsym(mesa.handle, "rwa_dispatch_buffers");
	*(void **)&mesa.module_argbufs = dlsym(mesa.handle, "rwa_module_argbufs");
	*(void **)&mesa.argbuf_encode_buffer = dlsym(mesa.handle, "rwa_argbuf_encode_buffer");
	*(void **)&mesa.argbuf_encode_texture = dlsym(mesa.handle, "rwa_argbuf_encode_texture");
	*(void **)&mesa.argbuf_encode_sampler = dlsym(mesa.handle, "rwa_argbuf_encode_sampler");
	*(void **)&mesa.argbuf_encoded_members = dlsym(mesa.handle,
						       "rwa_argbuf_encoded_members");
	/* Drawing is looked up but not required: a compiler library that can compute and not
	 * yet draw is a real state of this work, and the driver says so when asked to draw
	 * rather than refusing to exist. */
	*(void **)&mesa.render_create = dlsym(mesa.handle, "rwa_render_create");
	*(void **)&mesa.render_destroy = dlsym(mesa.handle, "rwa_render_destroy");
	*(void **)&mesa.render_draw = dlsym(mesa.handle, "rwa_render_draw");
	/* Depth likewise: a library that can draw and not test depth is a real state of
	 * this work, and -newDepthStencilStateWithDescriptor: says so when it is asked
	 * rather than the driver refusing to load. */
	*(void **)&mesa.depth_stencil_create = dlsym(mesa.handle, "rwa_depth_stencil_create");
	*(void **)&mesa.depth_stencil_destroy = dlsym(mesa.handle, "rwa_depth_stencil_destroy");
	*(void **)&mesa.stats_read = dlsym(mesa.handle, "rwa_stats_read");
	*(void **)&mesa.texture_prepare = dlsym(mesa.handle, "rwa_texture_prepare");
	*(void **)&mesa.finish = dlsym(mesa.handle, "rwa_finish");
	*(void **)&mesa.defer_waits = dlsym(mesa.handle, "rwa_device_defer_waits");
	*(void **)&mesa.flush = dlsym(mesa.handle, "rwa_flush");
	*(void **)&mesa.fence_wait = dlsym(mesa.handle, "rwa_fence_wait");
	*(void **)&mesa.fence_release = dlsym(mesa.handle, "rwa_fence_release");
	if (mesa.flush == NULL || mesa.fence_wait == NULL || mesa.fence_release == NULL) {
		mesa.defer_waits = NULL;   /* all four or none */
	}
	*(void **)&mesa.copy_buffer = dlsym(mesa.handle, "rwa_copy_buffer");
	*(void **)&mesa.fill_buffer = dlsym(mesa.handle, "rwa_fill_buffer");
	*(void **)&mesa.copy_texture = dlsym(mesa.handle, "rwa_copy_texture");
	*(void **)&mesa.copy_buffer_to_texture = dlsym(mesa.handle, "rwa_copy_buffer_to_texture");
	*(void **)&mesa.copy_texture_to_buffer = dlsym(mesa.handle, "rwa_copy_texture_to_buffer");
	*(void **)&mesa.generate_mipmaps = dlsym(mesa.handle, "rwa_generate_mipmaps");
	*(void **)&mesa.clear_color = dlsym(mesa.handle, "rwa_clear_color");
	*(void **)&mesa.clear_depth_stencil = dlsym(mesa.handle, "rwa_clear_depth_stencil");
	*(void **)&mesa.resolve_texture = dlsym(mesa.handle, "rwa_resolve_texture");
	/* not among what makes a library usable: a library without it has no views (M5b) */
	*(void **)&mesa.texture_view_create = dlsym(mesa.handle, "rwa_texture_view_create");
	/* and without these it has no specialized function, and Metal's own answer stands (M5c) */
	*(void **)&mesa.module_function_constants =
	    dlsym(mesa.handle, "rwa_module_function_constants");
	*(void **)&mesa.compute_create_constants =
	    dlsym(mesa.handle, "rwa_compute_create_constants");
	*(void **)&mesa.compute_create_stitched = dlsym(mesa.handle, "rwa_compute_create_stitched");
	if (mesa.device_open == NULL || mesa.compute_create == NULL || mesa.dispatch == NULL ||
	    mesa.texture_create == NULL || mesa.sampler_create == NULL) {
		/* rwa_dispatch's arguments changed when textures were added to the contract, so
		 * an older library would be called with the wrong shape of argument and do
		 * something arbitrary. Refusing it outright is the only safe reading of a
		 * missing rwa_texture_create. */
		rw_print("the compiler library is missing calls the contract names, "
		         "so it is older than this driver and will not be used\n");
		dlclose(mesa.handle);
		mesa.handle = NULL;
		return false;
	}
	/* A drawable's import (src/hkdrawable.m) is looked up in THIS library, the one the
	 * device is made with: both libraries export rwa_texture_from_surface (Metal over
	 * Vulkan has it since M3e). On Gallium it is the handle hkdrawable.m found by itself
	 * before, so nothing there changes. */
	rwd_use_library(mesa.handle);
	hk_say_traps();
	if (mesa.module_function_constants != NULL && mesa.compute_create_constants != NULL) {
		hk_serve_function_constants();
	}
	return true;
}

/*
 * A process that dies of a brk in Swift or in RenderBox leaves nothing but launchd's "exited due to SIGTRAP"
 * in a live log (chronod, run 142910, 27 lives). So where it was, in this process's images, and then
 * the same death. Only the stack of frame pointers: this runs in a signal handler.
 */
static void
hk_trap_handler(int number, siginfo_t *info, void *context)
{
	const ucontext_t *uc = context;
	uint64_t pc = uc->uc_mcontext->__ss.__pc, fp = uc->uc_mcontext->__ss.__fp;
	Dl_info where;

	(void)info;
	for (unsigned i = 0; i < 16; i++) {
		if (dladdr((const void *)pc, &where) && where.dli_fname != NULL) {
			const char *leaf = strrchr(where.dli_fname, '/');

			fprintf(stderr, "hk-trap[%s %d]: signal %d #%u %s+%#llx %s\n", getprogname(), getpid(), number, i,
				leaf != NULL ? leaf + 1 : where.dli_fname,
				(unsigned long long)(pc - (uint64_t)where.dli_fbase),
				where.dli_sname != NULL ? where.dli_sname : "");
		}
		if (fp == 0 || (fp & 7) != 0) {
			break;
		}
		pc = ((const uint64_t *)fp)[1] & 0x0000000fffffffffull;
		fp = ((const uint64_t *)fp)[0];
	}
	signal(number, SIG_DFL);
	raise(number);
}

/*
 * Every thread of this process, some seconds on: what a process that has stopped getting on with something
 * is waiting for. Only with /hackios/share/probes/sample in the root; it only reads.
 */
extern kern_return_t task_threads(task_t, thread_act_array_t *, mach_msg_type_number_t *);
extern kern_return_t thread_get_state(thread_act_t, thread_state_flavor_t, thread_state_t,
				      mach_msg_type_number_t *);

static void
hk_sample_threads(void)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	for (mach_msg_type_number_t t = 0; t < count; t++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		char line[1500];
		size_t at = 0;

		if (threads[t] == mach_thread_self() ||
		    thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &n) != KERN_SUCCESS) {
			continue;
		}
		uint64_t pc = __darwin_arm_thread_state64_get_pc(state) & 0x0000000fffffffffull;
		uint64_t fp = __darwin_arm_thread_state64_get_fp(state);

		for (unsigned i = 0; i < 24 && at + 80 < sizeof(line); i++) {
			Dl_info where;

			if (dladdr((const void *)pc, &where) && where.dli_fname != NULL) {
				const char *leaf = strrchr(where.dli_fname, '/');

				at += (size_t)snprintf(line + at, sizeof(line) - at, " %s:%s",
						       leaf != NULL ? leaf + 1 : where.dli_fname,
						       where.dli_sname != NULL ? where.dli_sname : "?");
			}
			if (fp == 0 || (fp & 7) != 0 || fp < 0x100000000ull) {
				break;
			}
			pc = ((const uint64_t *)fp)[1] & 0x0000000fffffffffull;
			fp = ((const uint64_t *)fp)[0];
		}
		fprintf(stderr, "hk-sample[%s] #%u%s\n", getprogname(), t, line);
	}
}

/*
 * (The owner: 「地圖伸縮卡頓」; Maps' pinch, 76 frames against hk-gl's 182, runs 005452 and 010021.) With a program's name in the file sample_of: on the Darwin notification com.hackios.sample,
 * for 12 s, every 250 ms, the threads of that program that are running, their CPU, and where; at the end the CPU each thread used in those 12 s. To the console.
 */
extern kern_return_t thread_info(thread_act_t, thread_flavor_t, thread_info_t, mach_msg_type_number_t *);
extern kern_return_t mach_port_deallocate(ipc_space_t, mach_port_name_t);
extern kern_return_t vm_deallocate(vm_map_t, vm_address_t, vm_size_t);
extern kern_return_t vm_read_overwrite(vm_map_t, vm_address_t, vm_size_t, vm_address_t, vm_size_t *);
extern kern_return_t thread_suspend(thread_act_t);
extern kern_return_t thread_resume(thread_act_t);
static void hk_console_line(const char *line, size_t length);

static void
hk_sample_tick(unsigned tick)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	static struct { thread_act_t thread; uint64_t us; } was[128];
	char line[1200];

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	for (mach_msg_type_number_t t = 0; t < count && t < 128; t++) {
		thread_basic_info_data_t info;
		mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
		arm_thread_state64_t state;
		mach_msg_type_number_t m = ARM_THREAD_STATE64_COUNT;
		char name[48] = "";

		if (threads[t] == mach_thread_self() || thread_info(threads[t], THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) {
			continue;
		}
		pthread_t pt = pthread_from_mach_thread_np(threads[t]);

		if (pt != NULL) {
			pthread_getname_np(pt, name, sizeof(name));
		}
		const uint64_t us = (uint64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds + (uint64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
		size_t at;

		if (tick == 0 || tick == 48) {
			unsigned k;

			for (k = 0; k < 128 && was[k].thread != 0 && was[k].thread != threads[t]; k++) {
			}
			if (k < 128 && tick == 48 && us - was[k].us >= 20000) {
				at = (size_t)snprintf(line, sizeof(line), "hk-sample[%s] thread %#x '%s' used %llu ms\n", getprogname(), threads[t], name, (unsigned long long)(us - was[k].us) / 1000);
				hk_console_line(line, MIN(at, sizeof(line) - 1));
			}
			if (k < 128) {
				was[k].thread = threads[t];
				was[k].us = us;
			}
		}
		/* (stopped first, and its stack read by a call that can fail: a thread that runs on makes a frame pointer of a moment ago a fault here, Maps' SIGSEGV of run 015038) */
		/* (a waiting thread every other tick: what a frame waits on is on the stack of a thread that does not run) */
		const bool running = info.run_state == TH_STATE_RUNNING && !(info.flags & TH_FLAGS_IDLE);

		if ((!running && (tick & 1) != 0) || thread_suspend(threads[t]) != KERN_SUCCESS) {
			continue;
		}
		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &m) != KERN_SUCCESS) {
			thread_resume(threads[t]);
			continue;
		}
		uint64_t pc = __darwin_arm_thread_state64_get_pc(state) & 0x0000000fffffffffull;
		uint64_t fp = __darwin_arm_thread_state64_get_fp(state);
		uint64_t pcs[24];
		unsigned depth = 0;

		for (; depth < 24; depth++) {
			uint64_t frame[2];
			vm_size_t got = 0;

			pcs[depth] = pc;
			if (fp == 0 || (fp & 7) != 0 || fp < 0x100000000ull ||
			    vm_read_overwrite(mach_task_self(), fp, sizeof(frame), (vm_address_t)frame, &got) != KERN_SUCCESS || got != sizeof(frame)) {
				depth++;
				break;
			}
			pc = frame[1] & 0x0000000fffffffffull;
			fp = frame[0];
		}
		thread_resume(threads[t]);
		if (!running && depth <= 3) {
			continue;       /* a worker with nothing to do */
		}

		at = (size_t)snprintf(line, sizeof(line), "hk-sample[%s] %u %#x %c '%s' %u%%:", getprogname(), tick, threads[t], running ? 'R' : 'W', name, info.cpu_usage / 10);
		for (unsigned i = 0; i < depth && at + 70 < sizeof(line); i++) {
			Dl_info where;

			if (dladdr((const void *)pcs[i], &where) && where.dli_fname != NULL) {
				const char *leaf = strrchr(where.dli_fname, '/');

				/* (the shared cache has no local names: such a frame by its offset, for ipsw dyld a2s) */
				if (where.dli_sname != NULL && strcmp(where.dli_sname, "<redacted>") != 0) {
					at += (size_t)snprintf(line + at, sizeof(line) - at, " %.14s:%.34s", leaf != NULL ? leaf + 1 : where.dli_fname, where.dli_sname);
				} else {
					at += (size_t)snprintf(line + at, sizeof(line) - at, " %.14s+%#llx", leaf != NULL ? leaf + 1 : where.dli_fname, (unsigned long long)(pcs[i] - (uint64_t)where.dli_fbase));
				}
			}
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
		hk_console_line(line, MIN(at, sizeof(line) - 1));
	}
	for (mach_msg_type_number_t t = 0; t < count; t++) {
		mach_port_deallocate(mach_task_self(), threads[t]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
}

static void
hk_sample_on_notice(void)
{
	char wanted[64] = "";
	int file = open("/hackios/share/probes/sample_of", O_RDONLY);

	if (file < 0) {
		return;
	}
	ssize_t got = read(file, wanted, sizeof(wanted) - 1);

	close(file);
	while (got > 0 && (wanted[got - 1] == '\n' || wanted[got - 1] == ' ')) {
		got--;
	}
	wanted[got > 0 ? got : 0] = 0;
	if (strcmp(getprogname(), wanted) != 0) {
		return;
	}
	int token;

	notify_register_dispatch("com.hackios.sample", &token, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(int unused) {
		(void)unused;
		for (unsigned tick = 0; tick <= 48; tick++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, tick * 250 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
				hk_sample_tick(tick);
			});
		}
	});
}

static void
hk_say_traps(void)
{
	struct sigaction action = { 0 };

	action.sa_sigaction = hk_trap_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGTRAP, &action, NULL);
	hk_sample_on_notice();
	if (access("/hackios/share/probes/sample", F_OK) == 0 && strcmp(getprogname(), "SpringBoard") == 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC),
			       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			hk_sample_threads();
		});
	}
}

/*
 * Say a thing once, per place that says it.
 *
 * Every method of this driver that declines to do something is called by a daemon that will
 * call it again on the next frame, so a line per call buries the log that matters and a
 * silent return tells us nothing at all. One line the first time a site is reached is what
 * names the feature the system actually wanted -- which is the whole value of answering
 * "no" instead of being absent: an absent selector aborts the daemon and says nothing, an
 * answer that refuses keeps it running AND leaves its name in the log.
 *
 * And it is flushed, because the measurement says it has to be: searching every log in
 * `logs/` for four different RW_ONCE strings -- the GPU priority, the one ring, the texture
 * view, the resource ID -- finds them **zero** times, while plain printf lines from these
 * same files are in every boot. The processes that reach these sites are the ones that then
 * abort (backboardd aborted 43 times in logs/214841-run.log), and stdio to a console is
 * block-buffered, so a line written and not flushed dies with the process. That is a
 * suspicion with evidence rather than a proof -- the sites may also simply never have been
 * reached -- and the flush is what makes the next boot able to tell the two apart: a line
 * that is now still missing was never reached. One flush per site per process costs nothing;
 * a diagnostic that has never once been seen costs the whole point of writing it.
 */
#define RW_ONCE(...)							\
	do {								\
		static BOOL rw_said_;					\
									\
		if (!rw_said_) {					\
			rw_said_ = YES;					\
			rw_print(__VA_ARGS__);				\
		}							\
	} while (0)

/*
 * A sentence that is true every time it is reached and says nothing new after the first: print
 * it, then only on powers of two, carrying the count. The window server asks this driver for a
 * two-plane YCbCr drawable 862 times in a boot, and 862 identical lines pushed the rest of that
 * boot out of the loader's log, which keeps the first 256 KB and the last megabyte. A refusal
 * that drowns the evidence of what it refused is a worse instrument than a quiet one.
 */
#define RW_AGAIN(...)							\
	do {								\
		static unsigned long rw_times_;				\
									\
		if ((++rw_times_ & (rw_times_ - 1)) == 0) {		\
			rw_print(__VA_ARGS__);				\
			if (rw_times_ > 1) {				\
				rw_print("  ... and that is the %lu'th "\
					 "time\n", rw_times_);		\
			}						\
		}							\
	} while (0)

/*
 * A BLEND LINE, deduplicated the way a repeated refusal is -- but on the CONTENT and not on
 * the call site, which is the difference between this and RW_AGAIN above.
 *
 * These lines are the measurement this driver exists to make, and they repeat enormously:
 * the window server built 127 pipelines in logs/080121-run.log and printed 240 attachment
 * lines, and there were THIRTEEN DISTINCT ONES among them. One boot of Resident Evil 4
 * would be a different order of magnitude -- 7 879 modules in its library -- and the
 * loader keeps the first 256 KB of a log and the last megabyte and drops the middle, which
 * is how 862 identical drawable refusals once pushed out the lines saying whether the game
 * had launched at all.
 *
 * So a line is printed the first time its exact content is seen and then on powers of two
 * of that content's own count, carrying the count. RW_AGAIN cannot do this: its counter is
 * per call site, so it would have suppressed twelve of those thirteen as if they were
 * repetitions of the first.
 *
 * Returns whether it printed, so the caller can attach the pipeline's LABEL to a line that
 * is new -- the label is what names the blend mode, and one per distinct configuration is
 * the whole of what it has to say.
 *
 * 32 entries is more than twice the thirteen a whole boot of the compositor produced, and
 * past it every further distinct line shares one counter rather than being dropped: a table
 * that silently stopped reporting would be the same instrument failure as the flood.
 */
static bool
rw_blend_line(unsigned attachment, unsigned format, const struct rwa_color_blend *b)
{
	static struct {
		unsigned attachment, format;
		struct rwa_color_blend b;
		unsigned long times;
	} seen[32];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].attachment == attachment && seen[i].format == format &&
		    memcmp(&seen[i].b, b, sizeof(*b)) == 0) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].attachment = attachment;
		seen[nseen].format = format;
		seen[nseen].b = *b;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return false;
	}
	rw_print("blend: attachment %u format %u blend %u rgb %u/%u op %u alpha %u/%u op %u "
	         "writeMask 0x%x\n", attachment, format, b->blending_enabled,
	         b->source_rgb_blend_factor, b->destination_rgb_blend_factor,
	         b->rgb_blend_operation, b->source_alpha_blend_factor,
	         b->destination_alpha_blend_factor, b->alpha_blend_operation, b->write_mask);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline asking for exactly that%s\n",
		         *times, times == &beyond ? ", counting every configuration past the "
		                                    "thirty-two this keeps apart" : "");
	}
	return true;
}

/* The same, for what the LIBRARY made of it: four distinct lines in that whole boot. */
static void
rw_blend_result(unsigned enabled_mask, unsigned write_masks)
{
	static struct { unsigned enabled, masks; unsigned long times; } seen[16];
	static unsigned nseen;
	static unsigned long beyond;
	unsigned long *times = NULL;

	for (unsigned i = 0; i < nseen; i++) {
		if (seen[i].enabled == enabled_mask && seen[i].masks == write_masks) {
			times = &seen[i].times;
			break;
		}
	}
	if (times == NULL && nseen < sizeof(seen) / sizeof(seen[0])) {
		seen[nseen].enabled = enabled_mask;
		seen[nseen].masks = write_masks;
		times = &seen[nseen].times;
		nseen++;
	}
	if (times == NULL) {
		times = &beyond;
	}
	if ((++*times & (*times - 1)) != 0) {
		return;
	}
	rw_print("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium "
	         "bits per attachment, R 0x1 .. A 0x8)\n", enabled_mask, write_masks);
	if (*times > 1) {
		rw_print("  ... and that is the %lu'th pipeline it made that of\n", *times);
	}
}

/* The private base class. Only what is used of it is declared, and nothing of its layout.
 * -init is its own (its method list has it); -initWithAcceleratorPort: is abstract there,
 * which is precisely the invitation this file accepts. */
@interface _MTLDevice : NSObject
- (instancetype)init;
@end

/* The private bases this driver stands on. Only what is used of each is declared, and
 * nothing of any layout. The designated initialisers are the ones Apple's own driver sends
 * to super (read out of IOGPUMetalCommandQueue 0x1ec16da90, IOGPUMetalCommandEncoder
 * 0x1ec171d14 and AGXA11FamilyComputePipeline 0x2092477e0), which is a better authority
 * than a guess at what an init of this kind ought to be called. */
@interface _MTLObjectWithLabel : NSObject
- (instancetype)init;
@end

@interface _MTLResource : _MTLObjectWithLabel
@end

@interface _MTLCommandQueue : _MTLObjectWithLabel
- (instancetype)initWithDevice:(id)device descriptor:(id)descriptor;
@end

@interface _MTLCommandBuffer : _MTLObjectWithLabel
- (instancetype)initWithQueue:(id)queue
	   retainedReferences:(BOOL)retained
	 synchronousDebugMode:(BOOL)synchronous;
@end

@interface _MTLCommandEncoder : _MTLObjectWithLabel
- (instancetype)initWithCommandBuffer:(id)commandBuffer;
/* Declared because it is CALLED: that initialiser retains the command buffer and this is the
 * only thing that gives it back (see -[HKMetalRenderEncoder endEncoding]). */
- (void)endEncoding;
@end

@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)device pipelineStateDescriptor:(id)descriptor;
@end

/*
 * The depth/stencil state's base class, and it is one of the good ones: everything but four
 * methods is Apple's and works for us. Read out of the cache the way the rest of this driver
 * was (tools/metal/absmeth.py over `_MTLDepthStencilState`, 10 selectors):
 *
 *   - -initWithDevice:depthStencilDescriptor: (0x181a29b38) is REAL and is the designated
 *     initialiser. It validates the descriptor field by field -- depthCompareFunction and
 *     each face's stencilCompareFunction, stencilFailureOperation, depthFailureOperation and
 *     depthStencilPassOperation are each compared against 8 and reported through
 *     MTLReportFailure when out of range -- then sends `init` to super
 *     (objc_msgSendSuper2), retains the device into _device and copies the descriptor's
 *     label into _label. It also asserts isKindOfClass: on both arguments (_MTLDevice for
 *     the device, MTLDepthStencilDescriptorInternal for the descriptor) and REPORTS rather
 *     than refuses: the failure paths branch back into the chain and the object is built
 *     either way.
 *   - -device and -label are ivar reads, so they work once that initialiser has run;
 *     -description, -formattedDescription: and -dealloc are Apple's.
 *   - -readsDepth, -writesDepth, -readsStencil, -writesStencil (0x181b1b5e0 and its three
 *     neighbours) are two-instruction NIL-STUBS: they answer NO. Those four are the whole of
 *     <MTLDepthStencilStateSPI> beyond the public protocol, and for a state that does write
 *     depth, NO is a false statement about this object -- so they are the four this driver
 *     overrides, out of the descriptor it was given.
 */
@interface _MTLDepthStencilState : NSObject
- (instancetype)initWithDevice:(id)device depthStencilDescriptor:(id)descriptor;
@end

/* The sampler has a real base class that owes its protocol nothing, and the initialiser
 * AGXA11FamilySampler sends to super (0x20930f0f0 -> 0x181a2d414). */
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id)device samplerDescriptor:(id)descriptor;
@end

/*
 * The one struct Metal reads out of a device by raw pointer, and the one answer on this
 * device that cannot be a refusal. Nothing here is guessed: the name and all three field
 * types are Apple's own, out of the type encoding MTLDeviceSPI records for the method
 * (`tools/metal/protometh.py`, joined to `absmeth.py` by implementation address):
 *
 *     -targetDeviceInfo    r^{MTLTargetDeviceArch=QI*}16@0:8
 *
 * so this declaration makes our method's recorded encoding the same string as Apple's,
 * rather than a `^v` that happens to pass in the same register. Which field means what was
 * read separately, off the readers -- see -targetDeviceInfo.
 */
struct MTLTargetDeviceArch {
	uint64_t target;      /* +0x00: nothing on this path reads it; Apple's own branch writes 0 */
	uint32_t version;     /* +0x08: the second half of MTLCompilerCache::Version{1, version} */
	char *name;           /* +0x10: formatted into "<pipelinelib>/pipelines.%s", never nil */
};

/* The offsets are the point of the struct, so they are checked rather than trusted: 0x10 is
 * the offset in the instruction that crashed without this answer. */
_Static_assert(sizeof(struct MTLTargetDeviceArch) == 24 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, version) == 0x8 &&
	       __builtin_offsetof(struct MTLTargetDeviceArch, name) == 0x10,
	       "Metal reads the name at +0x10 (0x181a3ca3c) and the version at +0x8");

/* M5g: -setVertexBytes: and its kind (-[HKMetalCommandBuffer hkBytes:...]). Metal's limit for them is
 * 4 KB; more goes to a buffer of its own, as all of them did. */
#define RW_BYTES_CHUNK       (64u * 1024)
#define RW_BYTES_MOST        (8u * 1024)
#define RW_BYTES_ALIGN       64u
#define RW_BYTES_CHUNKS_KEPT 8u

@interface HKMetalDevice : _MTLDevice {
	io_service_t _acceleratorService;   /* the IOAcceleratorES service Metal handed us */
	io_connect_t _gpu;                  /* this port's own gpu client, or IO_OBJECT_NULL */
	struct rwa_device *_mesa;           /* Mesa's screen and context, or NULL */
	BOOL _assertions;                   /* Metal's own assertion switch, stored for it */
	struct MTLTargetDeviceArch _target;     /* answered by pointer; see -targetDeviceInfo */
	char _targetName[96];               /* the bytes _target.name points at, inside the object */
	NSMutableArray *_bytesChunks;       /* M5g: chunks of -setVertexBytes: and friends the GPU has finished with */
}
/* Not part of any Metal protocol: how this driver's own objects reach the compiler. */
- (struct rwa_device *)mesaDevice;
/* M5g: a chunk for -[HKMetalCommandBuffer hkBytes:...], retained; and these back, once the GPU is done */
- (id)hkTakeBytesChunk;
- (void)hkGiveBackBytesChunks:(NSArray *)chunks;
/* and the argument encoder of one function's `[[buffer(n)]]` set, which the method this
 * driver installs into Metal's own _MTLFunction forwards to (the end of this file says
 * why it has to): the layout is read out of the function's own bitcode, which is the one
 * thing Metal has here and its reflection has not. */
- (id)hkArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index;
@end

/* Declaring the protocol is the gate both assertions in MTLAddDevice read. Done at +load
 * for the bundle case (Metal loads us, so Metal is already there) and again before the
 * first use, because a library can also be brought in by hand, before Metal. */
static void
ensure_spi(Class cls)
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");

	if (spi == NULL) {
		rw_print("MTLDeviceSPI is not in this process yet; "
		         "MTLAddDevice would abort\n");
		return;
	}
	if (!class_conformsToProtocol(cls, spi) && !class_addProtocol(cls, spi)) {
		rw_print("class_addProtocol(MTLDeviceSPI) refused\n");
	}
	/* and the one method of Metal's OWN function class this driver has to answer: it is
	 * installed here for the same reason the protocol is declared here, and the whole of
	 * why it is installed at all is at the end of this file. */
	ensure_function_encoder();
}

/* One request of this port's own gpu client, shaped like the msm ioctl it stands for
 * (iokit/Kernel/HKGPU.cpp: kMsmGetParam = 0x40, struct msm_param). The parameters
 * worth asking for are 0x03 CHIP_ID (with the speed bin in the high half), 0x02 GMEM_SIZE
 * and 0x04 MAX_FREQ. Mesa reaches the same client the same way. */
static bool
gpu_param(io_connect_t gpu, uint32_t which, uint64_t *value)
{
	struct {
		uint32_t pipe, param;
		uint64_t value;
		uint32_t len, pad;
	} request = { .pipe = 0, .param = which, .value = 0, .len = 0, .pad = 0 };
	size_t size = sizeof(request);

	if (gpu == IO_OBJECT_NULL) {
		return false;
	}
	if (IOConnectCallStructMethod(gpu, 0x40, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS) {
		return false;
	}
	*value = request.value;
	return true;
}

/* The kernel's gpu interface, by the numbers its own header gives
 * (iokit/Kernel/HKGPU.cpp). */
enum {
	kDrmGemClose  = 0x09,
	kMsmGetParam  = 0x40,
	kMsmGemNew    = 0x42,
	kMsmGemInfo   = 0x43,
};
#define MSM_INFO_GET_IOVA  1

struct gem_new   { uint64_t size; uint32_t flags, handle; };
struct gem_info  { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct gem_close { uint32_t handle, pad; };

#define RW_MAX_BUFFERS 32

@class HKMetalCommandQueue;
@class HKMetalWork;

@interface HKMetalComputePipeline : _MTLComputePipelineState {
	HKMetalDevice *_owner;
	struct rwa_compute *_compute;
	struct rwa_compute_info _info;
}
@end

/* One recorded dispatch. Metal's rule is that nothing runs until the command buffer is
 * committed, so an encoder records and the command buffer runs; dispatching at encode time
 * would be less code and would quietly reorder work. */
@interface HKMetalComputePipeline (HKMetalInternal)
- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor;
- (struct rwa_compute *)compute;
@end

@interface HKMetalCommandQueue : _MTLCommandQueue
- (instancetype)initWithHKDevice:(HKMetalDevice *)device descriptor:(id)descriptor;
@end

@interface HKMetalCommandBuffer : _MTLCommandBuffer
- (instancetype)initWithHKQueue:(HKMetalCommandQueue *)queue;
- (void)recordWork:(HKMetalWork *)work;
- (id)device;
/* M5g: these bytes in memory the GPU can read for as long as this buffer runs; the buffer they are
 * in, not retained, and where. A buffer of its own for each -setVertexBytes: was memory from the
 * kernel for each draw. */
- (id)hkBytes:(const void *)bytes length:(NSUInteger)length offset:(NSUInteger *)offset;
/* runs what was recorded from where it stopped: at -commit, and again when an event it waits for is signalled */
- (void)hkRun;
@end

@interface HKMetalComputeEncoder : _MTLCommandEncoder
- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer;
@end

@interface HKMetalRenderEncoder : _MTLCommandEncoder
- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer
				  descriptor:(id)descriptor;
/* Every form of every draw method records through this one, indexed or not: the thirteen
 * lines that copy the encoder's state into the work item are the same either way, and two
 * copies of them are how one form ends up carrying state the other does not. */
- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance;
@end

@interface HKMetalRenderPipeline : _MTLRenderPipelineState {
	HKMetalDevice *_owner;
	struct rwa_render *_render;
	struct rwa_render_info _info;
}
- (struct rwa_render *)render;
/* Not part of any Metal protocol: whether this pipeline's fragment shader stores to
 * [[depth]], which only whoever read the shader knows. A pass without a depth attachment
 * drops those writes -- Metal's own behaviour -- and a draw that says so is the difference
 * between a measurement and a colour attachment holding its clear value. */
- (BOOL)writesDepth;
/* Which buffer indices this pipeline's vertex stage FETCHES ATTRIBUTES from, one bit
 * each, and how many slots that covers. Metal has ONE table behind
 * -setVertexBuffer:offset:atIndex: and both the vertex descriptor's layouts and the
 * stage's [[buffer(n)]] arguments read out of it, so the encoder cannot know which an
 * index is until it knows the pipeline -- and this is what the draw splits the table
 * with. Zero for a stage that fetches nothing, which is every pipeline this driver drew
 * before the layout existed. */
- (unsigned)vertexStreamMask;
- (unsigned)vertexStreamCount;
/* And the element array the compiler library built out of the descriptor and the module's
 * own interface: how many, and which Metal [[attribute(n)]] each one is. The order is the
 * shader's driver_location order and not Metal's attribute order (docs/metal.md 10.44), so
 * these two are the only way a caller can read that mapping back instead of trusting it --
 * which is what metal_drv_test --draw-attrib prints beside its pixels. Not part of any
 * Metal protocol; Metal's own pipeline reflection would report the same thing. */
- (unsigned)vertexElementCount;
- (unsigned)vertexElementAttribute:(unsigned)index;
@end

/*
 * WHAT A 16-BIT TEXTURE ASKS FOR (M5b), for a later short metal=2 boot on Gallium to read.
 *
 * The Gallium library's table for MTLPixelFormat 40-43 is the mirror of real Metal's, red and blue
 * exchanged, and that mirror is what makes the wallpaper blue through Apple's compositor. Real
 * Metal, on an M1 Max and not A11, has the first-named channel in the low bits, and the Vulkan
 * library now does too (tools/mesa/air/tex_model.h). The picture does not show who did the
 * exchange: a B,G,R,A swizzle on the compositor's texture or view would look exactly like it, and
 * this file ignored every swizzle until this build. That is a hypothesis, unverified, and this line
 * is what can test it: one per distinct (format, route, swizzle, fourcc, usage), at most 32, so a
 * later texture that asks differently is not hidden by the first. Routes: "descriptor"
 * (-newTextureWithDescriptor:), "iosurface" (-newTextureWithDescriptor:iosurface:plane:) and "view"
 * (the three view calls, whose descriptor column is the parent's swizzle).
 *
 * WHAT THIS BUILD MAY DO TO THE PICTURE ON VK, because its table and its swizzles have both moved. It
 * could exchange red and blue twice if the compositor does swizzle and the bytes it reads were
 * written for a reading that already has the exchange in it: the swizzle goes through on this
 * library's "descriptor" and "view" routes, and the table no longer mirrors. And it could exchange
 * them once where Gallium does not, drawing the (255, 12, 12) wallpaper, if the compositor asks for
 * no swizzle at all, or asks on the "iosurface" route (the drawable's own path, which only this line
 * will show matters; that route honours one too since the build after run 112410). So a picture on vk
 * of formats 40-43 is a question for this line and not yet a result.
 */
/*
 * WHAT A SYSTEM PIPELINE WAS GIVEN (M5d), for reading a picture that is wrong: the fragment constants
 * of each CoreAnimation specification once, the ones that are not zero, which is what air2nir
 * --constants needs to show what that specification compiled to; and each pipeline's first draws
 * with what was bound, and any draw that samples something the size of the screen.
 */
static bool
hk_tracing(void)
{
	static int on = -1;

	if (on < 0) {
		on = access("/hackios/share/probes/metal_trace", F_OK) == 0;
	}
	return on != 0;
}

/*
 * (WHAT A FRAME PUT WHERE) the file frame_dump, for the window server only: one frame in every 300 committed, each draw
 * with where its vertices are, its scissor, and for each texture its surface. Says why a part of a page is not on the
 * screen, which no count of frames can.
 */
static bool
hk_frame_dumping(void)
{
	static int answer = -1;

	if (answer < 0) {
		answer = access("/hackios/share/probes/frame_dump", F_OK) == 0 && strcmp(getprogname(), "backboardd") == 0;
	}
	return answer != 0;
}

static void
hk_note_pipeline_constants(id descriptor, const struct rwa_function_constant *constants, unsigned count)
{
	NSString *label = [descriptor label];

	if (!hk_tracing() || constants == NULL || label == nil || ![label hasPrefix:@"com.apple.coreanimation"]) {
		return;
	}
	char line[900];
	size_t at = (size_t)snprintf(line, sizeof(line), "spec %s %s:", [[label substringFromIndex:29] UTF8String],
				     [[[descriptor fragmentFunction] name] UTF8String]);

	for (unsigned i = 0; i < count && at + 32 < sizeof(line); i++) {
		if (constants[i].bits != 0 || constants[i].bits_hi != 0) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %u=%llx", constants[i].index,
					       (unsigned long long)constants[i].bits);
		}
	}
	rw_print("%s\n", line);
}

static void
hk_note_packed16(const char *route, unsigned format, unsigned usage,
		 MTLTextureSwizzleChannels descriptor, const MTLTextureSwizzleChannels *view,
		 void *surface)
{
	static struct {
		unsigned format, usage;
		uint32_t fourcc, swizzles;
		const char *route;
	} seen[32];
	static unsigned count;

	if (format < 40 || format > 43) {
		return;
	}
	uint32_t fourcc = 0;

	if (surface != NULL) {
		uint32_t (*pixel_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");

		fourcc = pixel_format != NULL ? pixel_format(surface) : 0;
	}
	const uint32_t swizzles = (uint32_t)descriptor.red | (uint32_t)descriptor.green << 3 |
				  (uint32_t)descriptor.blue << 6 | (uint32_t)descriptor.alpha << 9 |
				  (view != NULL ? (uint32_t)view->red << 12 | (uint32_t)view->green << 15 |
						      (uint32_t)view->blue << 18 |
						      (uint32_t)view->alpha << 21 | 1u << 24 : 0);

	@synchronized([NSObject class]) {
		for (unsigned i = 0; i < count; i++) {
			if (seen[i].format == format && seen[i].usage == usage &&
			    seen[i].fourcc == fourcc && seen[i].swizzles == swizzles &&
			    seen[i].route == route) {
				return;
			}
		}
		if (count == sizeof(seen) / sizeof(seen[0])) {
			return;
		}
		seen[count].format = format;
		seen[count].usage = usage;
		seen[count].fourcc = fourcc;
		seen[count].swizzles = swizzles;
		seen[count].route = route;
		count++;
	}
	char v[32] = "none", f[16] = "none";

	if (view != NULL) {
		snprintf(v, sizeof(v), "%u %u %u %u", view->red, view->green, view->blue, view->alpha);
	}
	if (fourcc != 0) {
		snprintf(f, sizeof(f), "%c%c%c%c", (char)(fourcc >> 24), (char)(fourcc >> 16),
			 (char)(fourcc >> 8), (char)fourcc);
	}
	rw_print("packed16: format %u route %s descriptor-swizzle %u %u %u %u view-swizzle %s "
		 "surface-fourcc %s usage 0x%x (MTLTextureSwizzle: 0 zero, 1 one, 2 r, 3 g, 4 b, 5 a)\n",
		 format, route, descriptor.red, descriptor.green, descriptor.blue, descriptor.alpha, v,
		 f, usage);
}

@class HKMetalEvent;
@interface HKMetalWork : NSObject {
@public
	HKMetalComputePipeline *pipeline;
	HKMetalRenderPipeline *render;      /* set instead for a draw */
	id color[8];
	unsigned colorCount;
	float clearColor[4];
	BOOL clears;
	/* M5g: rwa_draw's fresh_pass and dont_care_mask */
	BOOL freshPass;
	unsigned dontCareMask;
	/* M5b: the level, and the slice or depth plane, each attachment is rendered at */
	unsigned colorLevel[8], colorSlice[8];
	unsigned depthLevel, depthSlice;
	/* the depth/stencil attachment of the pass, its two load actions and the state
	 * bound when the draw was encoded. Separate flags per aspect because Metal keeps
	 * two attachment descriptors over one texture and a pass that clears the depth and
	 * keeps the stencil is ordinary. */
	id depth;
	id depthStencilState;
	float clearDepth;
	unsigned clearStencil;
	BOOL clearsDepth, clearsStencil;
	unsigned stencilReference[2];
	float depthRange[2];
	BOOL hasDepthRange;
	/* the encoder's own state, as it stood when this draw was encoded (M5a, air_metal.h):
	 * the blend colour, the face of a triangle, the rectangles and each attachment's own
	 * clear, which the library takes from here where it can (the Vulkan one) and which
	 * Gallium's never looks at. */
	float blendColor[4];
	BOOL hasBlendColor;
	unsigned cullMode, frontWinding;
	float viewportRect[4];
	BOOL hasViewportRect;
	unsigned scissorRect[4];
	BOOL hasScissorRect;
	float attachmentClears[8][4];
	unsigned attachmentClearMask;
	unsigned primitive, vertexStart, vertexCount;
	/* the index buffer of an indexed draw and the three numbers that come with it in
	 * the same Metal call; indexBuffer nil is -drawPrimitives:, and then vertexCount
	 * counts vertices rather than indices */
	id indexBuffer;
	unsigned indexType;
	size_t indexBufferOffset;
	int baseVertex;
	unsigned instanceCount, baseInstance;
	id vertexBuffers[RW_MAX_BUFFERS];
	size_t vertexBufferOffsets[RW_MAX_BUFFERS];
	unsigned vertexBufferCount;
	id buffers[RW_MAX_BUFFERS];
	/* Metal's own `offset:` for each of them, which until argument buffers existed was
	 * refused at the encoder as unimplemented. It is RECORDED here and judged at the
	 * draw instead, because which indices may carry one is the pipeline's answer: a
	 * member of an argument buffer has its own offset and a buffer argument still has
	 * none. */
	size_t bufferOffsets[RW_MAX_BUFFERS];
	id textures[RW_MAX_BUFFERS];
	id samplers[RW_MAX_BUFFERS];
	unsigned count, textureCount, samplerCount;
	/* a draw's vertex stage has its own, at its own indices (-setVertexTexture:atIndex:) */
	id vertexTextures[RW_MAX_BUFFERS];
	id vertexSamplers[RW_MAX_BUFFERS];
	unsigned vertexTextureCount, vertexSamplerCount;
	unsigned groups[3], size[3];
	/* A BLIT instead of a dispatch or a draw (RW_BLIT_*, 0: not one), from an
	 * MTLBlitCommandEncoder: the two resources RETAINED (the blit's own -dealloc gives
	 * them back) and Metal's numbers as the call gave them. */
	unsigned blit;
	id blitFrom, blitTo;
	size_t fromOffset, toOffset, length, rowBytes, imageBytes;
	unsigned fromSlice, fromLevel, toSlice, toLevel;
	unsigned fromOrigin[3], toOrigin[3], blitSize[3];
	uint8_t fillValue;
	/* An event's signal or wait instead (RW_EVENT_*, 0: not one), at its place among the rest:
	 * -encodeSignalEvent:value: and -encodeWaitForEvent:value: of the command buffer. */
	unsigned eventOp;
	HKMetalEvent *event;
	uint64_t eventValue;
}
@end

enum { RW_EVENT_SIGNAL = 1, RW_EVENT_WAIT };

enum {
	RW_BLIT_BUFFER = 1, RW_BLIT_FILL, RW_BLIT_TEXTURE, RW_BLIT_BUFFER_TO_TEXTURE,
	RW_BLIT_TEXTURE_TO_BUFFER, RW_BLIT_MIPMAPS,
	RW_BLIT_CLEAR,          /* a render pass's load action with no draw to carry it */
	RW_BLIT_RESOLVE,        /* a render pass's multisample resolve, after its draws */
};

@interface HKMetalBlitEncoder : _MTLCommandEncoder
- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer;
@end

/*
 * What a buffer and a texture share: whether a heap made them. Nothing here suballocates --
 * every resource is its own allocation, a heap's one included -- so a heap is an account kept
 * of its resources' sizes against its own (HKMetalHeap below), and "made by a heap" is this.
 */
@class HKMetalHeap;
@interface HKMetalResource : _MTLResource {
@protected
	HKMetalHeap *_heap;       /* retained: Metal keeps a heap for as long as its resources */
	NSUInteger _heapSize;     /* what it counts against the heap (0: a placement heap's) */
	NSUInteger _heapOffset;
	BOOL _aliasable;
}
- (void)hkPlaceInHeap:(HKMetalHeap *)heap size:(NSUInteger)size offset:(NSUInteger)offset;
@end

static _Atomic(unsigned) hk_draw_backs;   /* how many textures stand for a surface (DRAWN AND PUT BACK) */
static struct { void *surface; id texture; } hk_stand_ins[64];   /* this process's one texture for each surface it cannot have in place, under @synchronized of the class: not retained, each takes itself out */

@interface HKMetalTexture : HKMetalResource {
	HKMetalDevice *_owner;
	struct rwa_texture *_texture;
	struct rwa_texture_shape _shape;
	int _responsibleProcess;   /* as on the buffer: accounting, not behaviour */
	/* M5b, a view: what it is a view of, retained as Metal's is, where in it, and its swizzle */
	id _parent;
	NSUInteger _parentLevel, _parentSlice;
	MTLTextureSwizzleChannels _swizzle;
	/* M5e, a copy of a surface (hkFollowSurface): the surface, what it added up to, and when it was last looked at */
	void *_followed;
	uint64_t _followedSum, _followedLooked;
	uint64_t _followedSummedAt;   /* M5g: when it was last added up, and the surface's seed then */
	uint32_t _followedSeed;
	BOOL _followedInPlace;   /* (frame_dump: the surface is only remembered, the GPU has its memory itself) */
	int64_t _liveBytes;      /* (hk_live) */
	BOOL _liveSurface;
	void *_biplanar;         /* format 500's copy (hkBiplanarLook): the surface, its seed and a sample of it when copied, the RGB */
	uint32_t _biplanarSeed;
	uint64_t _biplanarSum;
	uint8_t *_biplanarBytes;
	id _overBuffer;          /* M5g: the buffer whose memory this is (-[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]) */
	NSUInteger _rowGiven;    /* (census) the last bytesPerRow it was written with, or laid over a buffer with */
	BOOL _drawsBack;         /* M5g: hkDrawnBackTexture */
	BOOL _planeStandIn;      /* M5g: hkPlaneTexture */
	NSUInteger _plane;
	BOOL _putBackDue;
	_Atomic(uint64_t) _lastDrawn;
}
/* M5g: this texture's memory is that buffer's: kept for as long as the texture */
- (void)hkOverBuffer:(id)buffer row:(NSUInteger)row;
/* M5g, DRAWN AND PUT BACK (hkDrawnBackTexture): this texture stands for that surface as a target, and the surface gets what it holds */
- (void)hkDrawBackTo:(void *)surface;
- (void)hkStandInFor:(void *)surface;
- (void)hkStandInForPlane:(NSUInteger)plane of:(void *)surface;
- (BOOL)hkWritesPlaneBack;
- (void)hkPutBack;
- (id)hkOverBuffer;
- (void)hkStartDrawingBack;
- (BOOL)hkDrawsBack;
- (void)hkPutBackSoon;
/* format 500: this texture is that surface in RGB from now on; and the look that keeps it so */
- (void)hkBiplanarOf:(void *)surface seed:(uint32_t)seed sum:(uint64_t)sum;
- (void)hkBiplanarLook;
/* M5e: this texture is a copy of plane 0 of that surface from now on; and the look that keeps it one */
- (void)hkFollow:(void *)surface;
- (void)hkFollowSurface;
/* frame_dump: remember the surface of a texture over it; and what is known of either kind, for a line */
- (void)hkRememberSurface:(void *)surface;
- (size_t)hkDescribeSurface:(char *)out size:(size_t)size;
/* census (hkCensus): how it was made, and what it holds at its centre as R G B A, or byte by byte if it is not BGRA */
- (size_t)hkCensus:(char *)out size:(size_t)size centre:(uint8_t *)centre;
/* M5b: the slices Metal counts in this texture, a cube's six in them; and one level and one slice
 * of it as a texture of its own, or nil on a library without views: how a pass's clear and resolve,
 * whose calls take no level, say which (air_metal.h). A depth plane is a slice of a 3D texture. */
- (NSUInteger)hkSlices;
- (id)hkWindowLevel:(NSUInteger)level slice:(NSUInteger)slice;
@property (readonly) struct rwa_texture *mesaTexture;
/* Not part of MTLTexture: the depth or stencil aspect of this texture, one float and one
 * byte per texel. Metal's own -getBytes: has no aspect to name and the compiler library
 * refuses a depth texture through it by name, so a caller that wants to know what a depth
 * buffer holds asks for an aspect here instead. Either pointer may be NULL. */
- (BOOL)hkReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region;
@end

@interface HKMetalSampler : _MTLSamplerState {
	struct rwa_sampler *_sampler;
}
@property (readonly) struct rwa_sampler *mesaSampler;
@end

@interface HKMetalDepthStencilState : _MTLDepthStencilState {
	struct rwa_depth_stencil *_state;
	BOOL _readsDepth, _writesDepth, _readsStencil, _writesStencil;
}
@property (readonly) struct rwa_depth_stencil *mesaDepthStencil;
- (instancetype)initWithHKDevice:(HKMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor;
@end

/*
 * MTLArgumentEncoder. Not a writer: the records live in the compiler library with the
 * destination buffer, because the frontend has already resolved every member of every set
 * to a binding and Metal's 8-byte handles are an Apple GPU's descriptors (air_metal.h).
 * The implementation below carries the whole of why.
 */
@interface HKMetalArgumentEncoder : NSObject
- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule;
/* Not part of MTLArgumentEncoder: the mapping between Metal's [[id(n)]] and the index the
 * GPU is programmed with, so a test can read it back instead of trusting it. */
- (NSUInteger)hkMemberCount;
- (NSUInteger)hkMetalIndex;
- (BOOL)hkMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index;
@end

@interface HKMetalBuffer : HKMetalResource {
	void *_wrapped;          /* newBufferWithBytesNoCopy: the caller's pages, and what gives them back */
	void (^_wrappedGone)(void *, NSUInteger);
	HKMetalDevice *_owner;
	io_connect_t _gpu;        /* not owned: the device holds it */
	uint32_t _handle;         /* set on the kernel path only */
	uint64_t _iova;
	void *_contents;
	NSUInteger _length;
	BOOL _neverKept;         /* has been an argument buffer (the pool of buffers) */
	struct rwa_buffer *_mesa; /* set instead when the memory came from Mesa */
	int _responsibleProcess;  /* what <MTLResourceSPI> calls the owner, for accounting */
}
@property (readonly) struct rwa_buffer *mesaBuffer;
- (instancetype)initWithDevice:(HKMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length;
- (void)hkWrap:(void *)bytes deallocator:(void (^)(void *, NSUInteger))deallocator;
- (void)hkWrappedBack;
@end

@interface HKMetalFence : _MTLObjectWithLabel
- (instancetype)initWithHKDevice:(HKMetalDevice *)device;
@end

@interface HKMetalEvent : _MTLObjectWithLabel
- (instancetype)initWithHKDevice:(HKMetalDevice *)device shared:(BOOL)shared;
/* YES: not reached yet in the order of the GPU's work, and `buffer` is resumed when it is */
- (BOOL)hkWait:(HKMetalCommandBuffer *)buffer value:(uint64_t)value;
/* the value in the order of the GPU's work; returns the command buffers that may go on now */
- (NSArray *)hkSignalOrdered:(uint64_t)value;
/* the value as the processor sees it (signaledValue, listeners): once the work before it is done */
- (void)hkSignalVisible:(uint64_t)value;
@end

@interface HKMetalHeap : _MTLObjectWithLabel
- (instancetype)initWithHKDevice:(HKMetalDevice *)device descriptor:(MTLHeapDescriptor *)descriptor;
- (void)hkGiveBack:(NSUInteger)size;
@end

/*
 * What a heap counts for a resource (HKMetalHeap): a buffer's length and a texture's texels at
 * the bytes their format takes, rounded up to the GPU's pages. A compressed format is counted
 * at a byte a texel, the most any of them takes (ASTC 4x4).
 */
#define RW_HEAP_ALIGN           16384u

static NSUInteger
rw_heap_round(NSUInteger size)
{
	return (size + RW_HEAP_ALIGN - 1) & ~(NSUInteger)(RW_HEAP_ALIGN - 1);
}

static NSUInteger
rw_format_bytes(NSUInteger format)
{
	if (format < 20) {
		return 1;       /* A8, R8 */
	} else if (format < 53) {
		return 2;       /* R16, RG8, the packed 16-bit ones */
	} else if (format < 103) {
		return 4;       /* R32, RG16, RGBA8, BGRA8, the 10-bit ones */
	} else if (format < 123) {
		return 8;       /* RG32, RGBA16 */
	} else if (format < 130) {
		return 16;      /* RGBA32 */
	} else if (format < 250) {
		return 1;       /* block-compressed, 4:2:2 */
	}
	return format == 250 ? 2 : format == 253 ? 1 : format == 260 ? 8 : format >= 550 && format < 554 ? 8 : 4;
}

static NSUInteger
rw_texture_bytes(MTLTextureDescriptor *d)
{
	NSUInteger layers = MAX([d arrayLength], 1) * ([d textureType] == 5 || [d textureType] == 6 ? 6 : 1);
	NSUInteger bytes = MAX([d width], 1) * MAX([d height], 1) * MAX([d depth], 1) * layers *
	    MAX([d sampleCount], 1) * rw_format_bytes([d pixelFormat]);

	return [d mipmapLevelCount] > 1 ? bytes + bytes / 3 : bytes;       /* (a full chain is a third more) */
}

/*
 * FUNCTION CONSTANTS (M5c).
 *
 * -[MTLLibrary newFunctionWithName:constantValues:error:] and its three siblings run inside Metal's
 * own classes and through -[_MTLDevice compiler], which is nil here, so they answer nil, and most
 * applications ask them for every pipeline. Metal's classes are the ones an application holds, so
 * the methods are replaced on the classes when a library that can serve them is loaded:
 *
 *   MTLFunctionConstantValues  each setter still does what it did, and what it was told is kept
 *                              beside it, because nothing public reads it back. +alloc answers an
 *                              instance of MTLFunctionConstantValuesInternal, which has its own
 *                              setters, so those are the ones replaced (run 125308: on the public
 *                              class nothing was told anything, and every constant of every
 *                              specialized function read as not defined). A copy keeps what the
 *                              original was told.
 *   _MTLLibrary                the four that make a specialized function make one of ours.
 *
 * WHY A PROXY. -newFunctionWithName: answers the SAME object every time (measured on real Metal, on
 * an M1: two calls, one pointer), so values kept on it would be the last specialization's for every
 * pipeline made from it. HKSpecializedFunction is that object as far as anyone can tell, with the
 * values beside it; hk_function_constants is the one reader. A function nothing specialized has no
 * values and takes every constant as defined and zero, as every pipeline did until this.
 *
 * Unset and not defined: real Metal makes the function when a value is left out, on the M1, so
 * this does too, and the constant reads as not defined.
 */
@interface HKSpecializedFunction : NSProxy {
@public
	id _function;
	NSData *_constants;   /* struct rwa_function_constant, however many */
}
@end

@implementation HKSpecializedFunction
- (id)forwardingTargetForSelector:(SEL)selector { (void)selector; return _function; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	return [_function methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation *)invocation { [invocation invokeWithTarget:_function]; }
- (BOOL)isKindOfClass:(Class)c { return [_function isKindOfClass:c]; }
- (BOOL)isMemberOfClass:(Class)c { return [_function isMemberOfClass:c]; }
- (BOOL)conformsToProtocol:(Protocol *)p { return [_function conformsToProtocol:p]; }
- (BOOL)respondsToSelector:(SEL)selector { return [_function respondsToSelector:selector]; }
- (NSString *)description { return [_function description]; }
- (void)dealloc
{
	[_function release];
	[_constants release];
	[super dealloc];
}
@end

/* What a function was specialized with, or NULL with *given 0 for one nothing specialized. */
static const struct rwa_function_constant *
hk_function_constants(id function, unsigned *count, unsigned *given)
{
	*count = 0;
	*given = 0;
	if (function == nil || object_getClass(function) != [HKSpecializedFunction class]) {
		return NULL;
	}
	NSData *constants = ((HKSpecializedFunction *)function)->_constants;

	*given = 1;
	*count = (unsigned)([constants length] / sizeof(struct rwa_function_constant));
	return [constants bytes];
}

static char hk_constant_values_key;

/* MTLDataType's numbers, public and stable: the first of each run of one to four components. */
static bool
hk_constant_type(NSUInteger type, unsigned *bytes, unsigned *stride)
{
	static const struct { unsigned first, element; } runs[] = {
		{ 3, 4 } /* float */, { 16, 2 } /* half */, { 29, 4 } /* int */, { 33, 4 } /* uint */,
		{ 37, 2 } /* short */, { 41, 2 } /* ushort */, { 45, 1 } /* char */,
		{ 49, 1 } /* uchar */, { 53, 1 } /* bool */, { 81, 8 } /* long */, { 85, 8 } /* ulong */,
	};
	for (unsigned i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
		if (type >= runs[i].first && type < runs[i].first + 4) {
			unsigned comps = (unsigned)type - runs[i].first + 1;

			*bytes = comps * runs[i].element;
			*stride = (comps == 3 ? 4 : comps) * runs[i].element;
			return *bytes <= 16;
		}
	}
	return false;
}

static void
hk_constant_told(id values, id key, const void *value, unsigned bytes)
{
	NSMutableDictionary *told = objc_getAssociatedObject(values, &hk_constant_values_key);

	if (told == nil) {
		told = [[NSMutableDictionary alloc] init];
		objc_setAssociatedObject(values, &hk_constant_values_key, told,
					 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[told release];
	}
	[told setObject:[NSData dataWithBytes:value length:bytes] forKey:key];
}

static void (*hk_real_set_at_index)(id, SEL, const void *, NSUInteger, NSUInteger);
static void (*hk_real_set_with_name)(id, SEL, const void *, NSUInteger, id);
static void (*hk_real_set_range)(id, SEL, const void *, NSUInteger, NSRange);
static void (*hk_real_reset)(id, SEL);

static void
hk_set_at_index(id self, SEL cmd, const void *value, NSUInteger type, NSUInteger index)
{
	unsigned bytes, stride;

	hk_real_set_at_index(self, cmd, value, type, index);
	if (hk_constant_type(type, &bytes, &stride)) {
		hk_constant_told(self, @(index), value, bytes);
	} else {
		RW_ONCE("a function constant of MTLDataType %lu, which this driver has no size for\n",
			(unsigned long)type);
	}
}

static void
hk_set_with_name(id self, SEL cmd, const void *value, NSUInteger type, id name)
{
	unsigned bytes, stride;

	hk_real_set_with_name(self, cmd, value, type, name);
	if (hk_constant_type(type, &bytes, &stride)) {
		hk_constant_told(self, name, value, bytes);
	} else {
		RW_ONCE("a function constant of MTLDataType %lu, which this driver has no size for\n",
			(unsigned long)type);
	}
}

static void
hk_set_range(id self, SEL cmd, const void *values, NSUInteger type, NSRange range)
{
	unsigned bytes, stride;

	hk_real_set_range(self, cmd, values, type, range);
	if (hk_constant_type(type, &bytes, &stride)) {
		for (NSUInteger i = 0; i < range.length; i++) {
			hk_constant_told(self, @(range.location + i),
					 (const char *)values + i * stride, bytes);
		}
	}
}

static void
hk_values_reset(id self, SEL cmd)
{
	hk_real_reset(self, cmd);
	objc_setAssociatedObject(self, &hk_constant_values_key, nil,
				 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static id (*hk_real_values_copy)(id, SEL, void *);

static id
hk_values_copy(id self, SEL cmd, void *zone)
{
	id made = hk_real_values_copy(self, cmd, zone);
	NSDictionary *told = objc_getAssociatedObject(self, &hk_constant_values_key);

	if (made != nil && told != nil) {
		NSMutableDictionary *kept = [told mutableCopy];

		objc_setAssociatedObject(made, &hk_constant_values_key, kept,
					 OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		[kept release];
	}
	return made;
}

/* One specialization: the function, and the values the module declares a place for. */
/* (a module's or a set of constants' identity, within this process) */
static uint64_t
hk_bytes_hash(const void *bytes, size_t size)
{
	uint64_t h = 0xcbf29ce484222325ull ^ size;
	const uint8_t *p = bytes;

	for (; size >= 8; size -= 8, p += 8) {
		uint64_t word;

		memcpy(&word, p, 8);
		h = (h ^ word) * 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; size > 0; size--, p++) {
		h = (h ^ *p) * 0x100000001b3ull;
	}
	return h;
}

static id
hk_specialize(id library, id name, id values, NSError **error)
{
	/*
	 * VideoToolbox's Metal scaler, served (M5e). It was declined for some hours (M5d) because the wallpaper stayed
	 * black when it succeeded: its pipeline took 2-3 s to make, and the poster extension that decodes the wallpaper is
	 * killed by launchd at 5 s for hanging on launch. The file vt_decline answers nil again, for a comparison.
	 */
	/*
	 * [M5g: the file vt_decline_sb declines it in SpringBoard alone, a probe: its one use there scales the screen for a
	 * snapshot, 1125x2436 to 787x1705, and that one dispatch holds the GPU's one ring for 170 to 230 ms, once per app opened
	 * or closed (run 212454). Served is what HEIC needs; the hitch needs the shader's own speed found (TODO, M5g).]
	 */
	if ([name isEqualToString:@"VTMTSComputeFunction"] && (access("/hackios/share/probes/vt_decline", F_OK) == 0 ||
							       (strcmp(getprogname(), "SpringBoard") == 0 &&
								access("/hackios/share/probes/vt_decline_sb", F_OK) == 0))) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:3 userInfo:@{
				NSLocalizedDescriptionKey: @"VideoToolbox's scaler is declined here" }];
		}
		return nil;
	}
	id function = [library newFunctionWithName:name];

	if (function == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:5 /* FunctionNotFound */
						 userInfo:@{ NSLocalizedDescriptionKey:
							     [NSString stringWithFormat:@"no function named %@", name] }];
		}
		return nil;
	}
	NSDictionary *told = values != nil ? objc_getAssociatedObject(values, &hk_constant_values_key)
					   : nil;
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, sel_getUid("bitcodeData"));
	/* VideoToolbox's VTMTSComputeFunction declares 202 (run 125612) */
	enum { MOST_DECLS = 1024 };
	struct rwa_function_constant_decl *decls = calloc(MOST_DECLS, sizeof(*decls));
	unsigned declared = 0;
	char why[256] = { 0 };

	/*
	 * What a module declares does not change: read once per module. VideoToolbox asks for its function again for every
	 * transfer, and reading 464 KB of bitcode each time was most of the 10 s a thread of Photos' spent turning an HDR photo
	 * (run 051210: 10238 ms of 12 s on one thread, air_module_read under this call).
	 */
	static struct { uint64_t hash; size_t size; struct rwa_function_constant_decl *decls; unsigned declared; } known[8];
	static unsigned known_count;
	const uint64_t air_hash = air != nil ? hk_bytes_hash([air bytes], [air length]) : 0;
	bool remembered = false;

	@synchronized([NSObject class]) {
		for (unsigned k = 0; air != nil && decls != NULL && k < known_count && !remembered; k++) {
			if (known[k].hash == air_hash && known[k].size == [air length]) {
				memcpy(decls, known[k].decls, known[k].declared * sizeof(*decls));
				declared = known[k].declared;
				remembered = true;
			}
		}
	}
	if (!remembered) {
		if (air == nil || decls == NULL ||
		    mesa.module_function_constants(NULL, [air bytes], [air length], decls, MOST_DECLS,
						   &declared, why, sizeof(why)) != 0) {
			rw_print("the function constants of %s cannot be read: %s\n", [name UTF8String], why);
			declared = 0;
		} else {
			@synchronized([NSObject class]) {
				if (known_count < 8) {
					known[known_count].hash = air_hash;
					known[known_count].size = [air length];
					known[known_count].decls = malloc(MAX(declared, 1) * sizeof(*decls));
					memcpy(known[known_count].decls, decls, declared * sizeof(*decls));
					known[known_count++].declared = declared;
				}
			}
		}
	}
	NSMutableData *constants = [NSMutableData data];

	for (unsigned i = 0; i < declared; i++) {
		NSData *value = [told objectForKey:@(decls[i].index)];

		if (value == nil) {
			value = [told objectForKey:[NSString stringWithUTF8String:decls[i].name]];
		}
		if (value == nil) {
			continue;   /* not defined: the header comment says what real Metal does */
		}
		struct rwa_function_constant c = { .index = decls[i].index };
		uint64_t words[2] = { 0, 0 };

		memcpy(words, [value bytes], MIN([value length], sizeof(words)));
		c.bits = words[0];
		c.bits_hi = words[1];
		[constants appendBytes:&c length:sizeof(c)];
	}
	free(decls);
	HKSpecializedFunction *made = [HKSpecializedFunction alloc];

	made->_function = function;   /* -new..., which is +1 */
	made->_constants = [constants retain];
	return (id)made;
}

static id
hk_new_function_values_error(id self, SEL cmd, id name, id values, NSError **error)
{
	(void)cmd;
	return hk_specialize(self, name, values, error);
}

static void
hk_new_function_values_handler(id self, SEL cmd, id name, id values, void (^handler)(id, NSError *))
{
	NSError *error = nil;
	id made = hk_specialize(self, name, values, &error);

	(void)cmd;
	handler(made, error);
	[made release];
}

static id
hk_new_function_descriptor_error(id self, SEL cmd, id descriptor, NSError **error)
{
	(void)cmd;
	return hk_specialize(self, [descriptor name], [descriptor constantValues], error);
}

static void
hk_new_function_descriptor_handler(id self, SEL cmd, id descriptor, void (^handler)(id, NSError *))
{
	NSError *error = nil;
	id made = hk_specialize(self, [descriptor name], [descriptor constantValues], &error);

	(void)cmd;
	handler(made, error);
	[made release];
}

static void
hk_replace(Class c, const char *selector, IMP with, void **real)
{
	Method m = c != Nil ? class_getInstanceMethod(c, sel_getUid(selector)) : NULL;

	if (m == NULL) {
		rw_print("function constants: %s has no %s\n", c != Nil ? class_getName(c) : "(no class)",
			 selector);
		return;
	}
	IMP was = method_setImplementation(m, with);

	if (real != NULL) {
		*real = (void *)was;
	}
}

static void
hk_serve_function_constants(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	Class values = objc_getClass("MTLFunctionConstantValuesInternal");
	Class library = objc_getClass("_MTLLibrary");

	if (values == Nil) {
		values = objc_getClass("MTLFunctionConstantValues");
	}

	rw_print("function constants: what a specialized function was given is kept from %s\n",
		 values != Nil ? class_getName(values) : "(no class)");
	hk_replace(values, "setConstantValue:type:atIndex:", (IMP)hk_set_at_index,
		   (void **)&hk_real_set_at_index);
	hk_replace(values, "setConstantValue:type:withName:", (IMP)hk_set_with_name,
		   (void **)&hk_real_set_with_name);
	hk_replace(values, "setConstantValues:type:withRange:", (IMP)hk_set_range,
		   (void **)&hk_real_set_range);
	hk_replace(values, "reset", (IMP)hk_values_reset, (void **)&hk_real_reset);
	hk_replace(values, "copyWithZone:", (IMP)hk_values_copy, (void **)&hk_real_values_copy);
	hk_replace(library, "newFunctionWithName:constantValues:error:",
		   (IMP)hk_new_function_values_error, NULL);
	hk_replace(library, "newFunctionWithName:constantValues:completionHandler:",
		   (IMP)hk_new_function_values_handler, NULL);
	hk_replace(library, "newFunctionWithDescriptor:error:",
		   (IMP)hk_new_function_descriptor_error, NULL);
	hk_replace(library, "newFunctionWithDescriptor:completionHandler:",
		   (IMP)hk_new_function_descriptor_handler, NULL);
}

/*
 * MTLTextureDescriptor.swizzle (M5b). A swizzle is a view's business in the contract and every other
 * thing about the texture is the texture's, so a descriptor that has one is a texture and a view of
 * all of it, the view being what the application holds. The Gallium library has no views: it gets the
 * texture as it always did, and says so. Both routes that take a descriptor come here,
 * -newTextureWithDescriptor: and -newTextureWithDescriptor:iosurface:plane:.
 */
static id
hk_with_descriptor_swizzle(HKMetalTexture *made, MTLTextureDescriptor *descriptor)
{
	MTLTextureSwizzleChannels sw = [descriptor swizzle];

	if (made != nil && (sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen ||
			    sw.blue != MTLTextureSwizzleBlue || sw.alpha != MTLTextureSwizzleAlpha)) {
		id view = [made newTextureViewWithPixelFormat:(NSUInteger)[descriptor pixelFormat]
						 textureType:(NSUInteger)[descriptor textureType]
						      levels:NSMakeRange(0, [made mipmapLevelCount])
						      slices:NSMakeRange(0, [made hkSlices])
						     swizzle:sw];
		if (view == nil) {
			RW_ONCE("a texture descriptor asks for a swizzle and this compiler library "
				"has no views: it reads unswizzled\n");
			return made;
		}
		[made release];
		return view;
	}
	return made;
}

/*
 * M5g, WHERE A FRAME'S TIME GOES. With the file metal_stats, each process that commits says every 5 s
 * what it did in that time, here and in the library (rwa_stats_read). It costs a few additions a
 * command buffer, so that the numbers are the numbers of a run with the trace off.
 */
static struct {
	_Atomic(uint64_t) commits, run_us, run_longest;
	_Atomic(uint64_t) done, done_us, done_longest;
	_Atomic(uint64_t) looks, look_bytes, look_us;
	_Atomic(uint64_t) bytes_calls, imports, import_us, copied;
	_Atomic(uint64_t) made, made_us;
	_Atomic(uint64_t) biplanar, biplanar_us;   /* format 500 and 540 turned into RGB on the CPU (hkBiplanarTexture) */
} hk_stat;

/*
 * With metal_stats, what this process holds of this driver's objects at this moment, beside its footprint (K40: the compositor
 * growing to 1.5 GB, run 091520): flat here while the footprint climbs says the memory is held below this file. A texture's
 * bytes are its pixels at 4 bytes each, mip levels and block sizes aside: a figure to watch, not an account.
 */
static struct {
	_Atomic(int64_t) textures, texture_bytes, surfaces, surface_bytes, buffers, buffer_bytes;
	_Atomic(int64_t) asked, refused_again, made_again;   /* render pipelines asked for, in all; see the memo there */
	_Atomic(int64_t) pool_taken, pool_kept, pool_busy;              /* buffers that came from the pool below, and went into it */
} hk_live;

static uint64_t
hk_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static void
hk_stat_most(_Atomic(uint64_t) *most, uint64_t value)
{
	uint64_t was = atomic_load(most);

	while (value > was && !atomic_compare_exchange_weak(most, &was, value)) {
	}
}

static bool
hk_stats_on(void)
{
	static int on = -1;

	if (on < 0) {
		on = access("/hackios/share/probes/metal_stats", F_OK) == 0;
	}
	return on != 0;
}

/* (an application's stdout goes nowhere) */
static void
hk_console_line(const char *line, size_t length)
{
	static int console = -2;

	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (console >= 0) {
		(void)!write(console, line, length);
	}
}

/*
 * THE MEMORY'S ACCOUNT (K40): backboardd reaches hundreds of MB of footprint with far less than that in live textures and buffers (the
 * counts in hk-live). With the file mem_account in share/probes, every sixth stats line, what the rest is: the footprint and the
 * heap as the process has them; what the library and turnip hold (rwa_vk_memory: every BO by its name, as MB); and the process's
 * dirty pages by VM tag, the biggest first. It walks the address space, so it is not for a row.
 */
static struct rwa_device *hk_memory_device;
extern kern_return_t mach_vm_region_recurse(vm_map_read_t, mach_vm_address_t *, mach_vm_size_t *, natural_t *, vm_region_recurse_info_t,
					    mach_msg_type_number_t *);

static void
hk_memory_say(void)
{
	static int on = -1;
	static _Atomic(unsigned) every;
	char line[700];

	if (on < 0) {
		on = access("/hackios/share/probes/mem_account", F_OK) == 0;
	}
	if (!on || hk_memory_device == NULL || mesa.vk_memory == NULL || atomic_fetch_add(&every, 1) % 6 != 0) {
		return;
	}

	task_vm_info_data_t vm = { 0 };
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	malloc_statistics_t heap = { 0 };

	(void)task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
	malloc_zone_statistics(NULL, &heap);

	int said = snprintf(line, sizeof(line), "hk-bo[%s %d]: footprint %llu MB, heap %llu MB in use of %llu; ", getprogname(), getpid(),
			    (unsigned long long)(vm.phys_footprint >> 20), (unsigned long long)(heap.size_in_use >> 20),
			    (unsigned long long)(heap.size_allocated >> 20));

	said += mesa.vk_memory(hk_memory_device, line + said, sizeof(line) - (size_t)said);
	hk_console_line(line, MIN((size_t)said, sizeof(line) - 1));

	static uint64_t pages[256];
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;
	natural_t depth = 0;

	memset(pages, 0, sizeof(pages));
	for (;;) {
		vm_region_submap_info_data_64_t info;

		count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS) {
			break;
		}
		if (info.is_submap) {
			depth++;
			continue;
		}
		pages[info.user_tag & 255] += info.pages_dirtied + info.pages_swapped_out;
		address += size;
	}
	said = snprintf(line, sizeof(line), "hk-vm[%s %d]: dirty MB by tag:", getprogname(), getpid());
	for (int shown = 0; shown < 8; shown++) {
		int top = 0;

		for (int i = 1; i < 256; i++) {
			if (pages[i] > pages[top]) {
				top = i;
			}
		}
		if (pages[top] == 0 || (size_t)said >= sizeof(line)) {
			break;
		}
		said += snprintf(line + said, sizeof(line) - (size_t)said, " %d:%llu", top, (unsigned long long)(pages[top] * vm_page_size >> 20));
		pages[top] = 0;
	}
	hk_console_line(line, MIN((size_t)said, sizeof(line) - 1));
}

static void
hk_stats_say(void)
{
	static _Atomic(uint64_t) last_at;
	static uint64_t was[32], was_lib[RWA_STAT_COUNT];

	if (!hk_stats_on()) {
		return;
	}
	const uint64_t now = hk_now_us();
	uint64_t at = atomic_load(&last_at);

	if (at == 0) {
		atomic_compare_exchange_strong(&last_at, &at, now);
		return;
	}
	if (now - at < 5000000 || !atomic_compare_exchange_strong(&last_at, &at, now)) {
		return;
	}
	uint64_t lib[RWA_STAT_COUNT] = { 0 }, d[RWA_STAT_COUNT];

	if (mesa.stats_read != NULL) {
		mesa.stats_read(lib, RWA_STAT_COUNT);
	}
	for (unsigned i = 0; i < RWA_STAT_COUNT; i++) {
		d[i] = lib[i] - was_lib[i];
		was_lib[i] = lib[i];
	}
#define HK_STAT_TAKE(index, field) ({ const uint64_t v_ = atomic_load(&hk_stat.field), r_ = v_ - was[index]; was[index] = v_; r_; })
	const uint64_t commits = HK_STAT_TAKE(0, commits), run_us = HK_STAT_TAKE(1, run_us);
	const uint64_t done = HK_STAT_TAKE(2, done), done_us = HK_STAT_TAKE(3, done_us);
	const uint64_t looks = HK_STAT_TAKE(4, looks), look_bytes = HK_STAT_TAKE(5, look_bytes), look_us = HK_STAT_TAKE(6, look_us);
	const uint64_t bytes_calls = HK_STAT_TAKE(7, bytes_calls), imports = HK_STAT_TAKE(8, imports);
	const uint64_t import_us = HK_STAT_TAKE(9, import_us), copied = HK_STAT_TAKE(10, copied);
	const uint64_t made = HK_STAT_TAKE(11, made), made_us = HK_STAT_TAKE(12, made_us);
#undef HK_STAT_TAKE
	const uint64_t run_most = atomic_exchange(&hk_stat.run_longest, 0), done_most = atomic_exchange(&hk_stat.done_longest, 0);

	char line[640];
	const int length = snprintf(line, sizeof(line), "hkmetal[%s %d]: %llu s in %llu ms: %llu commits, recording %llu us each (longest %llu), done %llu us after commit (longest %llu); "
		 "%llu draws in %llu passes, %llu dispatches, %llu submits %llu us; passes ended by target %llu clear %llu layout %llu "
		 "apart %llu (fetch %llu writes %llu shares %llu) copy %llu barrier %llu; %llu buffer copies %llu KB; %llu setBytes; %llu buffers %llu us; "
		 "%llu pipelines %llu us, %llu functions made %llu us; %llu surfaces %llu us, %llu as copies; "
		 "%llu looks at copies, %llu MB, %llu us; textures %llu made %llu us, %llu kept; %llu 420 on the CPU %llu us\n", getprogname(), getpid(), (unsigned long long)(now / 1000000),
		 (unsigned long long)((now - at) / 1000), (unsigned long long)commits,
		 (unsigned long long)(commits ? run_us / commits : 0), (unsigned long long)run_most,
		 (unsigned long long)(done ? done_us / done : 0), (unsigned long long)done_most,
		 (unsigned long long)d[RWA_STAT_DRAWS], (unsigned long long)d[RWA_STAT_PASSES],
		 (unsigned long long)d[RWA_STAT_DISPATCHES], (unsigned long long)d[RWA_STAT_SUBMITS],
		 (unsigned long long)(d[RWA_STAT_SUBMITS] ? d[RWA_STAT_SUBMIT_US] / d[RWA_STAT_SUBMITS] : 0),
		 (unsigned long long)d[RWA_STAT_BREAK_TARGET], (unsigned long long)d[RWA_STAT_BREAK_CLEAR],
		 (unsigned long long)d[RWA_STAT_BREAK_LAYOUT], (unsigned long long)d[RWA_STAT_BREAK_SHARED],
		 (unsigned long long)d[RWA_STAT_APART_FETCH], (unsigned long long)d[RWA_STAT_APART_WRITES], (unsigned long long)d[RWA_STAT_APART_SHARES],
		 (unsigned long long)d[RWA_STAT_BREAK_COPY], (unsigned long long)d[RWA_STAT_BREAK_BARRIER],
		 (unsigned long long)d[RWA_STAT_COPIES], (unsigned long long)(d[RWA_STAT_COPY_BYTES] / 1024),
		 (unsigned long long)bytes_calls, (unsigned long long)d[RWA_STAT_BUFFERS],
		 (unsigned long long)d[RWA_STAT_BUFFER_US], (unsigned long long)d[RWA_STAT_PIPELINES],
		 (unsigned long long)d[RWA_STAT_PIPELINE_US], (unsigned long long)made, (unsigned long long)made_us,
		 (unsigned long long)imports, (unsigned long long)import_us, (unsigned long long)copied,
		 (unsigned long long)looks, (unsigned long long)(look_bytes >> 20), (unsigned long long)look_us,
		 (unsigned long long)d[RWA_STAT_TEXTURES], (unsigned long long)d[RWA_STAT_TEXTURE_US], (unsigned long long)d[RWA_STAT_TEXTURES_KEPT], (unsigned long long)atomic_exchange(&hk_stat.biplanar, 0), (unsigned long long)atomic_exchange(&hk_stat.biplanar_us, 0));

	if (length > 0) {
		hk_console_line(line, MIN((size_t)length, sizeof(line) - 1));
		{
			char live[260];
			const int said = snprintf(live, sizeof(live), "hk-live[%s %d]: pool took %lld kept %lld busy %lld; pipelines asked %lld, refused again %lld, made again %lld; textures %lld, %lld MB; of surfaces %lld, %lld MB; buffers %lld, %lld MB\n",
						  getprogname(), getpid(), (long long)atomic_load(&hk_live.pool_taken), (long long)atomic_load(&hk_live.pool_kept), (long long)atomic_load(&hk_live.pool_busy),
						  (long long)atomic_load(&hk_live.asked),
						  (long long)atomic_load(&hk_live.refused_again), (long long)atomic_load(&hk_live.made_again),
						  (long long)atomic_load(&hk_live.textures),
						  (long long)(atomic_load(&hk_live.texture_bytes) >> 20), (long long)atomic_load(&hk_live.surfaces),
						  (long long)(atomic_load(&hk_live.surface_bytes) >> 20), (long long)atomic_load(&hk_live.buffers),
						  (long long)(atomic_load(&hk_live.buffer_bytes) >> 20));

			hk_console_line(live, MIN((size_t)said, sizeof(live) - 1));
		}
		hk_memory_say();
	}
}

/*
 * FUNCTION STITCHING (K37). CoreImage on Metal makes its kernels with -newLibraryWithStitchedDescriptor:error: out of library functions,
 * ci_stdlib_stitchable's and its filters'; without it Metal refuses the descriptor, CoreImage logs that and every image it makes is empty,
 * status fine (ci_gain, runs 033712-034446). A graph is compiled when a pipeline is asked of it (air_stitch_to_nir): a function of this
 * library has no bitcode of its own to answer with. The classes are as measured on real Metal (a spy on this Mac, 2026-11-06): nodes in
 * order, an argument one of MTLFunctionStitchingBuiltinThreadPositionInGrid, ...InputBufferAddress, ...InputTexture, ...InputSampler or a
 * node of the same array, and an early return a node of its own (a class here, a flag on iOS 16.5's FunctionNodeSPI).
 */
@interface HKStitchedFunction : NSObject {
@public
	NSString *_name, *_label;
	id _graph, _device;
	NSArray *_functions;
}
@end

@implementation HKStitchedFunction
+ (void)initialize
{
	Protocol *p = objc_getProtocol("MTLFunction");

	if (p != NULL && !class_conformsToProtocol(self, p)) {
		class_addProtocol(self, p);
	}
}
- (void)dealloc
{
	[_name release];
	[_label release];
	[_graph release];
	[_device release];
	[_functions release];
	[super dealloc];
}
- (NSString *)name { return _name; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [_label release]; _label = [label copy]; }
- (id)device { return _device; }
- (NSUInteger)functionType { return 3; }   /* MTLFunctionTypeKernel */
- (NSInteger)patchControlPointCount { return -1; }
- (NSDictionary *)functionConstantsDictionary { return @{}; }
/* (what else Metal or CoreImage asks of a function is said and answered with nothing, rather than an exception that ends the process) */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	return [super methodSignatureForSelector:selector] ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
	rw_print("a stitched function was asked %s: nothing\n", sel_getName([invocation selector]));
	id none = nil;

	[invocation setReturnValue:&none];
}
@end

@interface HKStitchedLibrary : NSObject {
@public
	NSArray *_graphs, *_functions;
	id _device;
	NSString *_label;
}
@end

@implementation HKStitchedLibrary
+ (void)initialize
{
	Protocol *p = objc_getProtocol("MTLLibrary");

	if (p != NULL && !class_conformsToProtocol(self, p)) {
		class_addProtocol(self, p);
	}
}
- (void)dealloc
{
	[_graphs release];
	[_functions release];
	[_device release];
	[_label release];
	[super dealloc];
}
- (id)newFunctionWithName:(NSString *)name
{
	for (id graph in _graphs) {
		if ([[graph functionName] isEqualToString:name]) {
			HKStitchedFunction *function = [[HKStitchedFunction alloc] init];

			function->_name = [name copy];
			function->_graph = [graph retain];
			function->_device = [_device retain];
			function->_functions = [_functions retain];
			return function;
		}
	}
	return nil;
}
- (id)newFunctionWithName:(NSString *)name constantValues:(id)values error:(NSError **)error
{
	(void)values;
	id function = [self newFunctionWithName:name];

	if (function == nil && error != NULL) {
		*error = [NSError errorWithDomain:@"HKMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey: @"no such function in this stitched library" }];
	}
	return function;
}
- (id)newFunctionWithDescriptor:(id)descriptor error:(NSError **)error
{
	return [self newFunctionWithName:[descriptor name] constantValues:nil error:error];
}
- (NSArray *)functionNames
{
	NSMutableArray *names = [NSMutableArray array];

	for (id graph in _graphs) {
		[names addObject:[graph functionName]];
	}
	return names;
}
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { [_label release]; _label = [label copy]; }
- (id)device { return _device; }
- (NSInteger)type { return 0; }   /* MTLLibraryTypeExecutable */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	return [super methodSignatureForSelector:selector] ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
	rw_print("a stitched library was asked %s: nothing\n", sel_getName([invocation selector]));
	id none = nil;

	[invocation setReturnValue:&none];
}
@end

static NSUInteger hk_uint_of(id object, const char *name) { return ((NSUInteger (*)(id, SEL))objc_msgSend)(object, sel_getUid(name)); }
static BOOL hk_bool_of(id object, const char *name) { return ((BOOL (*)(id, SEL))objc_msgSend)(object, sel_getUid(name)); }
static id hk_object_of(id object, const char *name) { return ((id (*)(id, SEL))objc_msgSend)(object, sel_getUid(name)); }

@implementation HKMetalDevice

- (id)newLibraryWithStitchedDescriptor:(id)descriptor error:(NSError **)error
{
	if (mesa.compute_create_stitched == NULL) {
		return [self fail:error with:@"the compiler library here has no rwa_compute_create_stitched: stitching is the Vulkan back end's"];
	}
	HKStitchedLibrary *library = [[HKStitchedLibrary alloc] init];

	library->_graphs = [hk_object_of(descriptor, "functionGraphs") retain];
	library->_functions = [hk_object_of(descriptor, "functions") retain];
	library->_device = [self retain];
	return library;
}

- (void)newLibraryWithStitchedDescriptor:(id)descriptor completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id library = [self newLibraryWithStitchedDescriptor:descriptor error:&error];

	handler(library, error);
	[library release];
}

- (id)newLibraryWithStitchedDescriptor:(id)descriptor destinationBinaryArchive:(id)archive error:(NSError **)error
{
	(void)archive;
	return [self newLibraryWithStitchedDescriptor:descriptor error:error];
}

- (id)newLibraryWithStitchedDescriptorSPI:(id)descriptor error:(NSError **)error
{
	return [self newLibraryWithStitchedDescriptor:descriptor error:error];
}

/* The graph as rwa_compute_create_stitched wants it, by its classes, and the pipeline. */
- (id)pipelineForStitched:(HKStitchedFunction *)function descriptor:(id)descriptor error:(NSError **)error
{
	NSArray *nodes = hk_object_of(function->_graph, "nodes");
	const unsigned count = (unsigned)[nodes count];
	struct rwa_stitch_node *out = calloc(MAX(count, 1), sizeof(*out));
	NSMutableArray *keep = [NSMutableArray array];
	NSString *refused = nil;
	Class early = objc_getClass("MTLFunctionStitchingEarlyReturnNode"), address = objc_getClass("MTLFunctionStitchingInputBufferAddress");
	Class buffer = objc_getClass("MTLFunctionStitchingInputBuffer"), texture = objc_getClass("MTLFunctionStitchingInputTexture");
	Class sampler = objc_getClass("MTLFunctionStitchingInputSampler"), grid = objc_getClass("MTLFunctionStitchingBuiltinThreadPositionInGrid");
	Class group = objc_getClass("MTLFunctionStitchingBuiltinThreadPositionInThreadgroup"), call = objc_getClass("MTLFunctionStitchingFunctionNode");

	for (unsigned n = 0; n < count && refused == nil; n++) {
		id node = nodes[n];
		NSArray *arguments = hk_object_of(node, "arguments");
		const unsigned given = (unsigned)[arguments count];
		struct rwa_stitch_input *inputs = calloc(MAX(given, 1), sizeof(*inputs));

		[keep addObject:[NSData dataWithBytesNoCopy:inputs length:MAX(given, 1) * sizeof(*inputs) freeWhenDone:YES]];
		out[n].inputs = inputs;
		out[n].num_inputs = given;
		out[n].early_return = (early != Nil && [node isKindOfClass:early]) ||
				      ([node respondsToSelector:sel_getUid("isEarlyReturn")] && hk_bool_of(node, "isEarlyReturn"));
		NSString *name = hk_object_of(node, "name");

		/* (iOS 16.5: an early return is a function's node with a flag, and its own call; a Mac today has it as a node of no name over another) */
		if ([name length] != 0) {
			out[n].name = [name UTF8String];
			[keep addObject:name];
			for (id candidate in function->_functions) {
				if ([[candidate name] isEqualToString:name] && [candidate respondsToSelector:sel_getUid("bitcodeData")]) {
					NSData *air = hk_object_of(candidate, "bitcodeData");

					[keep addObject:air];
					out[n].bitcode = [air bytes];
					out[n].size = [air length];
					break;
				}
			}
			if (out[n].bitcode == NULL) {
				refused = [NSString stringWithFormat:@"the stitched graph calls %@, which the descriptor's functions have no bitcode of", name];
			}
		}
		for (unsigned a = 0; a < given && refused == nil; a++) {
			id argument = arguments[a];

			if (grid != Nil && [argument isKindOfClass:grid]) {
				inputs[a].kind = 0;
			} else if (group != Nil && [argument isKindOfClass:group]) {
				inputs[a].kind = 1;
			} else if (address != Nil && [argument isKindOfClass:address]) {
				inputs[a].kind = hk_bool_of(argument, "dereference") ? 2 : 3;
				inputs[a].bind = (unsigned)hk_uint_of(argument, "bindIndex");
				inputs[a].offset = (unsigned)hk_uint_of(argument, "byteOffset");
			} else if (buffer != Nil && [argument isKindOfClass:buffer]) {
				inputs[a].kind = 3;
				inputs[a].bind = (unsigned)hk_uint_of(argument, "bindIndex");
			} else if (texture != Nil && [argument isKindOfClass:texture]) {
				inputs[a].kind = 4;
				inputs[a].bind = (unsigned)hk_uint_of(argument, "bindIndex");
			} else if (sampler != Nil && [argument isKindOfClass:sampler]) {
				inputs[a].kind = 5;
				inputs[a].bind = (unsigned)hk_uint_of(argument, "bindIndex");
			} else if (call != Nil && [argument isKindOfClass:call]) {
				const NSUInteger at = [nodes indexOfObjectIdenticalTo:argument];

				inputs[a].kind = 6;
				inputs[a].node = (unsigned)(at != NSNotFound ? at : [nodes indexOfObject:argument]);
				if (inputs[a].node >= n) {
					refused = [NSString stringWithFormat:@"argument %u of stitched node %u is not an earlier node", a, n];
				}
			} else {
				refused = [NSString stringWithFormat:@"argument %u of stitched node %u is a %s, which this does not read", a, n, class_getName([argument class])];
			}
		}
	}
	id state = nil;

	if (refused != nil) {
		state = [self fail:error with:refused];
	} else {
		char why[512] = { 0 };
		struct rwa_compute_info info = { 0 };
		const CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
		struct rwa_compute *compute = mesa.compute_create_stitched(_mesa, [function->_name UTF8String], out, count, &info, why, sizeof(why));

		if (CFAbsoluteTimeGetCurrent() - began > 0.25 || compute == NULL) {
			rw_print("stitched compute %s took %.1f s to make: %s\n", [function->_name UTF8String], CFAbsoluteTimeGetCurrent() - began, compute != NULL ? "made" : why);
		}
		/* (a refused graph as this read it, the first few: what the frontend's sentence is about) */
		static unsigned graphs_told;

		if (compute == NULL && graphs_told++ < 3) {
			for (unsigned n = 0; n < count; n++) {
				char line[300];
				int at = snprintf(line, sizeof(line), "  node %u %s %s%s:", n, class_getName([nodes[n] class]), out[n].name != NULL ? out[n].name : "(none)", out[n].early_return ? " early return" : "");

				for (unsigned a = 0; a < out[n].num_inputs && at < 240; a++) {
					at += snprintf(line + at, sizeof(line) - (size_t)at, " %u/%u/%u/%u", out[n].inputs[a].kind, out[n].inputs[a].bind, out[n].inputs[a].offset, out[n].inputs[a].node);
				}
				rw_print("%s\n", line);
			}
		}
		state = compute == NULL ? [self fail:error with:[NSString stringWithUTF8String:why]]
					: [[HKMetalComputePipeline alloc] initWithHKDevice:self compute:compute info:&info descriptor:descriptor];
	}
	free(out);
	return state;
}


+ (void)load
{
	ensure_spi(self);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	ensure_spi([self class]);

	self = [super init];
	if (self == nil) {
		return nil;
	}
	_acceleratorService = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}

	/* The gpu this driver would actually drive. Not fatal if it is absent: the port
	 * starts the Adreno only with the boot argument gpu=1, and a device object that
	 * cannot allocate is still the thing under test until the resource layer exists. */
	io_service_t gpu = IOServiceGetMatchingService(0, IOServiceMatching("HKGPU"));

	_gpu = IO_OBJECT_NULL;
	if (gpu == IO_OBJECT_NULL) {
		rw_print("no HKGPU service (the gpu was not started)\n");
	} else {
		kern_return_t kr = IOServiceOpen(gpu, mach_task_self(), 0, &_gpu);

		rw_print("HKGPU client %s (%#x)\n",
		         kr == KERN_SUCCESS ? "opened" : "refused", kr);
		IOObjectRelease(gpu);
	}

	/* And the compiler, if it is there. Its absence is not fatal: the device still has to
	 * exist and answer, it just cannot make a pipeline (and says so when asked). */
	if (mesa_available()) {
		char why[256] = { 0 };

		_mesa = mesa.device_open(why, sizeof(why));
		hk_memory_device = _mesa;
		rw_print("the compiler library %s\n",
		         _mesa != NULL ? mesa.device_name(_mesa) : why);
		/* A command buffer is this driver's unit of completion (-commit), so a draw does
		 * not wait for itself: the GPU's per-draw round trip was most of a frame. */
		/* metal_sync=1 keeps the old wait per draw: slower, but a failure names its draw */
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		bool sync = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
			    strstr(args, "metal_sync=1") != NULL;

		if (_mesa != NULL && mesa.defer_waits != NULL && !sync) {
			mesa.defer_waits(_mesa, 1);
		}
		rw_print("a fence per %s\n", _mesa != NULL && mesa.defer_waits != NULL && !sync
			 ? "command buffer" : "draw and dispatch (metal_sync=1 or an older library)");
	}

	/* The struct Metal reads out of this device by raw pointer, filled here rather than on
	 * demand: the pointer is handed out without a lock, from whichever thread a renderer
	 * happens to be on, and here is the one moment when nobody can have asked yet. The
	 * name is Apple's own rule for a device that declines -- its -name with the spaces
	 * taken out (0x181a3ca0c) -- but the bytes are COPIED, which Apple's branch does not
	 * need to do: it keeps the autoreleased -UTF8String in a struct on its own stack and is
	 * done with it nine instructions later, while this one is answered to callers for as
	 * long as the device lives. See -targetDeviceInfo. */
	NSString *plain = [[self name] stringByReplacingOccurrencesOfString:@" "
								 withString:@""];
	const char *bytes = [plain UTF8String];

	snprintf(_targetName, sizeof(_targetName), "%s", bytes != NULL ? bytes : "");
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;
	rw_print("targetDeviceInfo answers {0, 0, \"%s\"} -- _MTLDevice's own entry "
	         "answers NULL and Metal reads it without looking, so the precompiled pipelines "
	         "this device will be asked for are pipelines.%s\n", _target.name, _target.name);
	return self;
}

- (struct rwa_device *)mesaDevice
{
	return _mesa;
}

- (void)dealloc
{
	if (_mesa != NULL) {
		mesa.device_close(_mesa);
	}
	if (_gpu != IO_OBJECT_NULL) {
		IOServiceClose(_gpu);
	}
	if (_acceleratorService != IO_OBJECT_NULL) {
		IOObjectRelease(_acceleratorService);
	}
	[super dealloc];
}

/* The profile is what -[_MTLDevice initLimits] switches on, and it was swept on the device:
 * 0, 1 and 4 to 9 fill the limits table, while 2, 3 and 10 to 12 abort inside Apple on
 * `_limits.maxColorAttachments > 0`. 0 is taken because it is the lowest that works and
 * because its table (31 buffers, 16 samplers, 512 threads per threadgroup, 16 KB of
 * threadgroup memory, 8192 textures) is a plausible floor for this Adreno.
 *
 * "Which profile the hardware deserves is a separate question, to be answered when something
 * is drawn" -- and something has now been drawn, so here is the answer so far. This number is
 * the ONLY input Apple has for what this device supports: -[_MTLDevice initGPUFamilySupport]
 * switches on it and pushes a list of MTLGPUFamily into the vector that -supportsFamily:
 * searches. Profile 0 pushes Apple1 and Common1 and nothing else.
 *
 * That is why SpringBoard cannot start. Siri's orb prewarms an MPSImageGaussianBlur, and
 * MPSDevice::GetMPSDevice is a descending ladder of -supportsFamily: from Apple8 (1008) down
 * to Apple2 (1002) which returns NULL when none answers. It never asks about Apple1 -- the
 * constant 0x3e9 is nowhere in that function -- so the one family we claim is the one family
 * it does not want, and the process aborts in MPSKernel with "Failed to get MPSDevice"
 * (docs/metal.md 10.47). Profile 1 is the lowest that pushes Apple2.
 *
 * So the profile is a boot-arg (metal_profile=N), not because it should stay configurable but
 * because the cost of each step up has to be measured rather than argued: every family claimed
 * is a promise, and some of those promises this driver cannot keep. Answering LESS is not the
 * safe direction either -- an empty family vector makes
 * +[MTLFXSpatialScalerDescriptor supportsDevice:] take this for a discrete GPU and say yes
 * unconditionally -- so there is no answer here that is safe by default, only one that is
 * measured. Apple's own A11 driver answers 5.
 */
- (NSUInteger)featureProfile
{
	static NSUInteger profile = (NSUInteger)-1;

	if (profile == (NSUInteger)-1) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		profile = 0;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "metal_profile=")) != NULL && (at == args || at[-1] == ' ')) {
			profile = (NSUInteger)strtoul(at + strlen("metal_profile="), NULL, 10);
		}
		rw_print("feature profile %lu, which is what Metal reads this device's "
		         "whole capability surface out of (metal_profile= to change it; 0 claims Apple1 "
		         "alone, 1 adds Apple2, which is the lowest rung MPS asks about)\n",
		         (unsigned long)profile);
	}
	return profile;
}

/* A placeholder, and named as one: nothing has yet measured what the kernel's gemNew will
 * give in one allocation. It only has to be non-zero for initLimits to accept it. */
- (uint64_t)maxBufferLength
{
	return 256ULL << 20;
}

/*
 * The render pipeline, by the same rule as the compute one: both of _MTLDevice's entry points
 * go through its nil -compiler, so a driver that leaves them alone gets nil and no reason.
 *
 * One asymmetry is deliberate and is the measured road rather than a shortcut. When the
 * descriptor names no vertex function, the compiler library supplies a pass-through vertex
 * stage over the whole attachment -- which is exactly how air_run has drawn every fragment
 * shader of the corpus, 255 of them compared pixel by pixel (docs/air.md). A descriptor that
 * does name one passes both modules and nothing is generated.
 */
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
				     error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (descriptor == nil) {
		return [self fail:error with:@"no render pipeline descriptor"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libhackiosair.dylib is "
					     @"missing or older than this driver"];
	}
	if (mesa.render_create == NULL) {
		/* The order of these two matters: the first version asked about render_create
		 * first and would have told a caller that a library which never loaded "can
		 * compute but not yet draw", which is a sentence about a library that is not
		 * there. */
		return [self fail:error with:@"this compiler library can compute but not yet "
					     @"draw"];
	}

	id vertex = [descriptor vertexFunction];
	id fragment = [descriptor fragmentFunction];

	if (fragment == nil) {
		return [self fail:error with:@"the descriptor has no fragment function, and this "
					     @"driver has nothing to draw without one"];
	}
	if (![fragment respondsToSelector:bitcode] ||
	    (vertex != nil && ![vertex respondsToSelector:bitcode])) {
		return [self fail:error with:@"a function in this descriptor does not answer "
					     @"bitcodeData, so its AIR cannot be read"];
	}
	NSData *fragment_air = ((NSData *(*)(id, SEL))objc_msgSend)(fragment, bitcode);
	NSData *vertex_air = vertex != nil ?
	    ((NSData *(*)(id, SEL))objc_msgSend)(vertex, bitcode) : nil;

	if (fragment_air == nil || [fragment_air length] == 0) {
		return [self fail:error with:@"the fragment function has no bitcode"];
	}

	struct rwa_render_shape shape = { .sample_count = (unsigned)[descriptor sampleCount] };

	/*
	 * THE COLOUR ATTACHMENTS, each one's format AND ITS BLEND STATE.
	 *
	 * The blend half is what the home screen was missing: with no blend state in the
	 * contract the library made every pipeline opaque, and CoreAnimation composites with
	 * PREMULTIPLIED ALPHA -- so a label layer whose background is transparent black was
	 * drawn as opaque black, which is the black box behind every icon name in the
	 * photographs.
	 *
	 * All eight are read, as the formats are, and for the same reason: Apple's array
	 * answers -objectAtIndexedSubscript: for every index in range and an attachment an
	 * application never touched holds Apple's own defaults -- blendingEnabled NO,
	 * writeMask All, the factors One and Zero, both operations Add (MTLRenderPipeline.h).
	 * Nothing is interpreted here. Every number goes across as Metal's own and the
	 * library maps the three enumerations by table, because a cast between them is wrong:
	 * MTLBlendFactor and pipe_blendfactor agree on two of nineteen values and
	 * MTLColorWriteMask's bits run in the opposite order to Gallium's colormask.
	 *
	 * `color_blend_count` is set to eight -- all eight entries filled in -- and NOT to
	 * the attachment count: a zero there means "this shape carries no blend state at
	 * all", which is the old opaque behaviour, and that is a thing this driver must never
	 * say by accident.
	 */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		unsigned format = attachment != nil ? (unsigned)[attachment pixelFormat] : 0;

		shape.color_format[i] = format;
		if (format != 0) {
			shape.color_count = i + 1;
		}
		if (attachment == nil) {
			/* an array that answered nil has no defaults to pass on, so this
			 * entry says what Metal's own untouched descriptor says: no
			 * blending, every channel written */
			shape.color_blend[i].write_mask = 0xf;   /* MTLColorWriteMaskAll */
			continue;
		}
		shape.color_blend[i].blending_enabled =
		    [attachment isBlendingEnabled] ? 1 : 0;
		shape.color_blend[i].rgb_blend_operation =
		    (unsigned)[attachment rgbBlendOperation];
		shape.color_blend[i].alpha_blend_operation =
		    (unsigned)[attachment alphaBlendOperation];
		shape.color_blend[i].source_rgb_blend_factor =
		    (unsigned)[attachment sourceRGBBlendFactor];
		shape.color_blend[i].destination_rgb_blend_factor =
		    (unsigned)[attachment destinationRGBBlendFactor];
		shape.color_blend[i].source_alpha_blend_factor =
		    (unsigned)[attachment sourceAlphaBlendFactor];
		shape.color_blend[i].destination_alpha_blend_factor =
		    (unsigned)[attachment destinationAlphaBlendFactor];
		shape.color_blend[i].write_mask = (unsigned)[attachment writeMask];
	}
	shape.color_blend_count = 8;
	/*
	 * And the measurement this was built to make: what the window server actually asks
	 * for, one line per attachment it has, printed BEFORE the library can refuse any of
	 * it. These are Metal's own numbers, so `blend 1 rgb 1/5 op 0 alpha 1/5 op 0` is
	 * premultiplied source-over (One, OneMinusSourceAlpha, Add) and `blend 1 rgb 4/5` is
	 * straight alpha instead -- the one distinction a boot log has to be able to make.
	 *
	 * THE LABEL IS HERE BECAUSE IT IS THE ANSWER. CoreAnimation builds a pipeline's
	 * label out of the specification it compiled: `encode_segment` emits a `B<mnemonic>`
	 * segment IF AND ONLY IF the fixed-function blend mode is not its "none" value
	 * (0x36), so `com.apple.coreanimation.draw.PBGRABsovXm_...` is a pipeline whose
	 * blending this descriptor carries and `com.apple.coreanimation.draw.PBGRAXm_...` is
	 * one that blends INSIDE ITS FRAGMENT SHADER and leaves this descriptor at Apple's
	 * defaults. One grep over a boot log then says which of the two the home screen is
	 * made of, which no amount of reading the enumerations can.
	 */
	bool blend_is_new = false;

	for (unsigned i = 0; i < shape.color_count; i++) {
		blend_is_new |= rw_blend_line(i, shape.color_format[i],
		                              &shape.color_blend[i]);
	}
	/* and the label, on a pipeline whose blend configuration has not been seen before:
	 * one per distinct configuration is the whole of what the label has to say, and the
	 * compositor has over a hundred of them per boot */
	if (blend_is_new) {
		NSString *label = [descriptor label];

		rw_print("blend: ... and the first such pipeline's label is %s\n",
		         label != nil ? [label UTF8String] : "(none)");
	}
	/* Both aspect formats, as Metal's descriptor has them, and the library requires
	 * them to name one texture: an application with a combined format sets the same
	 * MTLPixelFormat on both, because there is one attachment behind the two
	 * properties. Passing them through separately rather than folding them here is
	 * what lets the library refuse a descriptor that names two different formats by
	 * name instead of this driver picking one of them. */
	shape.depth_format = (unsigned)[descriptor depthAttachmentPixelFormat];
	shape.stencil_format = (unsigned)[descriptor stencilAttachmentPixelFormat];

	/*
	 * THE VERTEX DESCRIPTOR, which is what the window server asked this driver for and
	 * did not get: one pipeline specification was refused 80 times with "reads vertex
	 * attributes ([[stage_in]]), and this contract carries no vertex attribute layout"
	 * (docs/metal.md 10.44, 10.45), because the layout lives here and nowhere in the AIR.
	 *
	 * Both of Metal's arrays are read whole, index by index, and passed through as
	 * Metal's own numbers -- the library decides what it can honour and refuses the
	 * rest BY NAME, which is the half this driver must not do for it: a format a6xx
	 * cannot fetch and an offset past twelve bits are both things freedreno would take
	 * silently (air_metal.h has the register fields).
	 *
	 * There is no count to read on either side. Apple's arrays answer
	 * -objectAtIndexedSubscript: for every index in range and a descriptor an
	 * application never touched holds MTLVertexFormatInvalid in every attribute, which
	 * is what the library reads as "not configured" -- so the loop is the whole array
	 * and the formats decide. The range is Apple's 31 of each, from the feature tables,
	 * and asking past it would raise.
	 */
	MTLVertexDescriptor *vd = [descriptor vertexDescriptor];

	if (vd != nil) {
		unsigned configured = 0, highest = 0;

		for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
			MTLVertexAttributeDescriptor *a =
			    [[vd attributes] objectAtIndexedSubscript:i];
			unsigned format = a != nil ? (unsigned)[a format] : 0;

			if (format == 0) {
				continue;
			}
			shape.vertex_attribute[i].format = format;
			shape.vertex_attribute[i].offset = (unsigned)[a offset];
			shape.vertex_attribute[i].buffer_index = (unsigned)[a bufferIndex];
			configured++;
			if ((unsigned)[a bufferIndex] > highest) {
				highest = (unsigned)[a bufferIndex];
			}
		}
		for (unsigned k = 0; k < RWA_MAX_VERTEX_BUFFERS; k++) {
			MTLVertexBufferLayoutDescriptor *l =
			    [[vd layouts] objectAtIndexedSubscript:k];

			if (l == nil) {
				continue;
			}
			shape.vertex_layout[k].stride = (unsigned)[l stride];
			shape.vertex_layout[k].step_function = (unsigned)[l stepFunction];
			shape.vertex_layout[k].step_rate = (unsigned)[l stepRate];
		}
		if (configured > 0) {
			rw_print("this pipeline's vertex descriptor configures %u "
			         "attribute%s, the highest from buffer index %u\n", configured,
			         configured == 1 ? "" : "s", highest);
		}
	}

	/* M5c: what each function was specialized with, if it was (FUNCTION CONSTANTS) */
	shape.vertex_constants = hk_function_constants(vertex, &shape.vertex_constant_count,
						       &shape.vertex_constants_given);
	shape.fragment_constants = hk_function_constants(fragment, &shape.fragment_constant_count,
							 &shape.fragment_constants_given);

	hk_note_pipeline_constants(descriptor, shape.fragment_constants, shape.fragment_constant_count);
	if (hk_tracing() && [[descriptor label] hasPrefix:@"com.apple.coreanimation"]) {
		char line[500];
		size_t at = (size_t)snprintf(line, sizeof(line), "layout %s:", [[[descriptor label] substringFromIndex:29] UTF8String]);

		for (unsigned i = 0; i < 8 && at + 40 < sizeof(line); i++) {
			if (shape.vertex_attribute[i].format != 0) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " a%u fmt %u +%u buf %u", i,
						       shape.vertex_attribute[i].format, shape.vertex_attribute[i].offset,
						       shape.vertex_attribute[i].buffer_index);
			}
		}
		for (unsigned k = 0; k < 4 && at + 24 < sizeof(line); k++) {
			if (shape.vertex_layout[k].stride != 0) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " stride[%u] %u", k, shape.vertex_layout[k].stride);
			}
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " vertex constants:");
		for (unsigned i = 0; i < shape.vertex_constant_count && at + 16 < sizeof(line); i++) {
			if (shape.vertex_constants[i].bits != 0) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %u=%llx", shape.vertex_constants[i].index,
						       (unsigned long long)shape.vertex_constants[i].bits);
			}
		}
		rw_print("%s\n", line);
	}

	/*
	 * What is asked for again. A specification refused once is refused again, and whoever asked asks at every draw: VectorKit's
	 * GlowAlpha and PatternedRibbon, framebuffer fetch at 4 samples, about 15 tries a frame at 0.6 ms each, on Maps' main thread,
	 * up to 1.4 s in 5 s while a finger moves it (run 093635, the owner: 「不跟手」). So the sentence is kept and given again.
	 * The same count for what was made says whether keeping those too is worth doing.
	 */
	static NSMutableDictionary *asked_before;
	static int no_memo = -1;   /* (the file pipeline_no_memo in share/probes: the sentence not kept, only counted, to compare) */
	NSString *memo;
	{
		struct rwa_render_shape plain = shape;

		plain.vertex_constants = NULL;
		plain.fragment_constants = NULL;
		memo = [NSString stringWithFormat:@"%@ %@ %zu %zu %llx %llx %llx", [vertex name], [fragment name], (size_t)[vertex_air length],
			(size_t)[fragment_air length], (unsigned long long)hk_bytes_hash(&plain, sizeof(plain)),
			(unsigned long long)(shape.vertex_constants != NULL ? hk_bytes_hash(shape.vertex_constants, shape.vertex_constant_count * sizeof(*shape.vertex_constants)) : 0),
			(unsigned long long)(shape.fragment_constants != NULL ? hk_bytes_hash(shape.fragment_constants, shape.fragment_constant_count * sizeof(*shape.fragment_constants)) : 0)];
	}
	atomic_fetch_add(&hk_live.asked, 1);
	@synchronized([HKMetalDevice class]) {
		NSString *before = [asked_before objectForKey:memo];

		if (no_memo < 0) {
			no_memo = access("/hackios/share/probes/pipeline_no_memo", F_OK) == 0;
		}
		if (before != nil && [before length] > 0) {
			atomic_fetch_add(&hk_live.refused_again, 1);
			if (!no_memo) {
				return [self fail:error with:before];
			}
		}
		if (before != nil && [before length] == 0) {
			atomic_fetch_add(&hk_live.made_again, 1);
		}
	}

	char why[512] = { 0 };
	struct rwa_render_info info = { 0 };
	const uint64_t made_began = hk_now_us();
	struct rwa_render *render = mesa.render_create(_mesa,
	    vertex_air != nil ? [vertex_air bytes] : NULL,
	    vertex_air != nil ? [vertex_air length] : 0,
	    vertex != nil ? [[vertex name] UTF8String] : NULL,
	    [fragment_air bytes], [fragment_air length], [[fragment name] UTF8String],
	    &shape, &info, why, sizeof(why));

	const uint64_t made_took = hk_now_us() - made_began;

	atomic_fetch_add(&hk_stat.made, 1);
	atomic_fetch_add(&hk_stat.made_us, made_took);
	/* which ones a window's "functions made" is (a stutter is one of these on whoever asked), with the file metal_stats */
	if (made_took >= 20000 && hk_stats_on()) {
		char slow[300];
		const int said = snprintf(slow, sizeof(slow), "hk-slow[%s %d]: %llu ms for the render pipeline of %s and %s (%s)", getprogname(), getpid(),
					  (unsigned long long)(made_took / 1000), vertex != nil ? [[vertex name] UTF8String] : "no vertex stage",
					  [[fragment name] UTF8String], render != NULL ? "made" : "refused");

		hk_console_line(slow, MIN((size_t)said, sizeof(slow) - 1));
	}

	if (render == NULL) {
		/* PRINTED as well as returned, and RW_AGAIN because the same specification
		 * is asked for over and over: -fail: puts the sentence in an NSError and
		 * nothing more, and whether anyone ever sees it is then the caller's
		 * business -- CoreAnimation discards this error and retries. A refusal that
		 * reaches no log is a refusal nobody can grep for, which makes the whole
		 * "refused by name" discipline unobservable from a boot: one pipeline
		 * specification was declined 80 times over 42 lives of backboardd before the
		 * vertex layout existed, and that number had to be counted by hand. */
		RW_AGAIN("this render pipeline is refused (%s, %s, %u fragment constants): %s\n",
			 [[vertex name] UTF8String] ?: "no vertex function", [[fragment name] UTF8String] ?: "?",
			 shape.fragment_constant_count, why);
		@synchronized([HKMetalDevice class]) {
			if (asked_before == nil) {
				asked_before = [[NSMutableDictionary alloc] init];
			}
			if ([asked_before count] < 1024) {
				[asked_before setObject:[NSString stringWithUTF8String:why] forKey:memo];
			}
		}
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	@synchronized([HKMetalDevice class]) {
		if (asked_before == nil) {
			asked_before = [[NSMutableDictionary alloc] init];
		}
		if ([asked_before count] < 1024) {
			[asked_before setObject:@"" forKey:memo];
		}
	}
	/* and what the library made of it, in GALLIUM's bits: for any mask but All, None,
	 * green|blue and red|alpha these nibbles must DIFFER from the writeMask printed
	 * above, because the two enumerations number their channels in opposite directions.
	 * A log where 0xe went in and 0xe came back is a log saying nothing reversed them. */
	if (shape.color_count > 0) {
		rw_blend_result(info.blend_enabled_mask, info.color_write_masks);
	}
	return [[HKMetalRenderPipeline alloc] initWithHKDevice:self
								 render:render
								   info:&info
							     descriptor:descriptor];
}

- (id)newRenderPipelineStateWithDescriptor:(id)descriptor
				   options:(NSUInteger)options
				reflection:(id *)reflection
				     error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection is Metal's compiler's, and ours is nil */
	}
	return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

/*
 * The asynchronous forms, and they are not a convenience: without them Metal's own versions
 * send the pipeline request to the nil compiler, return, and NEVER CALL THE HANDLER -- so an
 * application that builds a pipeline asynchronously waits for ever instead of being told no.
 * Reversing the four entry points is what turned that up (0x181aabc48 and 0x181aabccc both
 * end in a stack block handed to the compiler), and a hang is the one failure mode that
 * cannot be read out of a log.
 *
 * Building synchronously and calling the handler at once is allowed: Metal does not promise
 * the handler runs later, only that it runs.
 */
- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
			   completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newRenderPipelineStateWithDescriptor:(id)descriptor
				     options:(NSUInteger)options
			   completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newRenderPipelineStateWithDescriptor:descriptor
						     options:options
						  reflection:&reflection
						       error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

/*
 * The mesh forms are the opposite hazard: they are NOT routed through the compiler, they ask
 * the mesh descriptor for a plain render pipeline descriptor and re-enter one of the four
 * forms above on self (0x181aabf60 and its three neighbours). Inherited, they would hand a
 * mesh pipeline to a vertex-and-fragment builder and produce something built wrong instead of
 * a refusal. Mesh shaders are an A14-and-later feature and nothing here implements them.
 */
- (id)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
				       options:(NSUInteger)options
				    reflection:(id *)reflection
					 error:(NSError **)error
{
	(void)descriptor; (void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	return [self fail:error with:@"mesh pipelines are not implemented, and the inherited "
				     @"path would have built this one as a vertex pipeline"];
}

- (void)newRenderPipelineStateWithMeshDescriptor:(id)descriptor
					 options:(NSUInteger)options
			       completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;

	(void)[self newRenderPipelineStateWithMeshDescriptor:descriptor options:options
						 reflection:NULL error:&error];
	if (handler != NULL) {
		handler(nil, nil, error);
	}
}

/* A texture is Metal's descriptor handed through to the compiler library in Metal's own
 * terms; a pixel format it cannot translate is refused by name, because a format that is
 * almost right computes a wrong picture instead of failing. */
/*
 * THE SELECTORS THE PROTOCOLS DO NOT LIST, and this is the second time one of them has killed a
 * process. owes.py counts what the protocols require and found nothing missing; these are
 * declared on Apple's counterpart CLASSES -- AGXA11FamilyDevice and IOGPUMetalDevice -- which our
 * device does not descend from, so nothing in the chain answers them and no protocol says so.
 * tools/metal/owesclass.py counts that other list: 343 such selectors, 72 of which something in
 * this image actually sends, and NONE of the 72 behind a -respondsToSelector:. QuartzCore does not
 * ask, it sends. So every one of them is terminate(), not a composite that merely looks wrong --
 * which is what setProtectionOptions: did to the window server and what the first of these just
 * did to mediaserverd.
 *
 * A texture LAYOUT answers bytes-per-row, image stride, size and alignment for a descriptor
 * without allocating anything, so CoreAnimation can size an IOSurface before it has a texture.
 * nil is the honest answer rather than a measured one: CA::Render::new_mtl_texture_layout hands
 * our result straight back to its own caller, and that caller is in no other image of this cache,
 * so whether a nil is tolerated is unknown. If the log says it is not, the next step is a small
 * object over the arithmetic -newTextureWithDescriptor: below already does.
 */
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
			isHeapOrBufferBacked:(BOOL)backed
{
	(void)descriptor;
	(void)backed;
	RW_ONCE("hkmetal: a texture layout was asked for and this driver has none to give; "
		"CoreAnimation sizes a surface with it before any texture exists, and nil is the "
		"answer until a boot says nil is not tolerated\n");
	return nil;
}

/*
 * A resource group names resources whose cache lines the GPU may drop at the end of a pass -- an
 * AGX cache-management optimisation this port has no control over. nil here is not a guess: the
 * caller tests it (cbz x0 in CA::OGL::MetalContext::mark_cache_drop_hints) and composites without
 * it, and a nil also leaves the counter at zero, which makes -encodeCacheHintTag:... and
 * -encodeCacheHintFinalize:... unreachable. One nil answers three selectors, which is the right
 * shape: refuse where the decision is made rather than at each leaf.
 */
- (id)newResourceGroupFromResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources;
	(void)count;
	return nil;
}

/*
 * Overriding this keeps _MTLDevice's own body from running on our device, and that body is what
 * sends -copyIOSurfaceSharedTextureProperties: and -sparseSurfaceDefaultValue, neither of which
 * we answer. So one override closes three. It agrees with -newSharedTextureHandle below, which
 * answers nil for the same reason: nothing here is shared between processes.
 */
- (id)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	(void)descriptor;
	return nil;
}

/*
 * A texture over memory the CALLER allocated, which CoreAnimation's update_texture stores and
 * uses (docs/metal.md 10.48, selector 5 of the class-declared list). It is answered by name
 * rather than left unrecognized, because an unrecognized selector is terminate() and a named nil
 * is a line in the log -- mediaserverd died 43 times on the one above this before it was
 * answered at all.
 *
 * nil and not a texture, because the contract has no call for it. rwa_texture_from_surface takes
 * an IOSurface, which the kernel hands over as a buffer (kHKSurface); a bare pointer into
 * the caller's address space is a different import, and a texture made by copying instead would
 * be wrong in a way nothing would report: the caller writes to its own pages and expects the
 * texture to change.
 */
- (id)newTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length
		     descriptor:(MTLTextureDescriptor *)descriptor
		    deallocator:(void (^)(void *, NSUInteger))deallocator
{
	(void)bytes;
	(void)length;
	(void)descriptor;
	(void)deallocator;
	RW_ONCE("hkmetal: a texture over the caller's own memory was asked for, and the "
		"contract has no import for a bare pointer -- only for an IOSurface. Copying "
		"instead would be worse than nil: the caller writes to its pages and expects the "
		"texture to follow\n");
	return nil;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a texture needs the compiler library\n");
		return nil;
	}
	struct rwa_texture_shape shape = {
		.pixel_format = (unsigned)[descriptor pixelFormat],
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	static unsigned multisampled_told;

	if (shape.sample_count > 1 && multisampled_told < 6) {
		multisampled_told++;
		char told[160];     /* (to the console: an application's stdout goes nowhere, and Maps' is the one wanted) */
		const int length = snprintf(told, sizeof(told), "hkmetal[%s]: a multisampled texture: %ux%u format %u, %u samples, storage mode %u, usage %#x\n", getprogname(), shape.width, shape.height, shape.pixel_format, shape.sample_count, (unsigned)[descriptor storageMode], shape.usage);

		hk_console_line(told, MIN((size_t)length, sizeof(told) - 1));
	}
	hk_note_packed16("descriptor", shape.pixel_format, shape.usage, [descriptor swizzle], NULL, NULL);
	struct rwa_texture *texture = mesa.texture_create(_mesa, &shape, why, sizeof(why));

	if (texture == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	HKMetalTexture *made = [[HKMetalTexture alloc] initWithHKDevice:self
								texture:texture
								  shape:&shape];

	return hk_with_descriptor_swizzle(made, descriptor);
}

/* The sampler's own base class exists and owes its protocol nothing, so this is the cheapest
 * object in the driver: the state goes to the compiler library, which maps it the same way it
 * maps a shader's own `constexpr sampler` word -- one vocabulary, so the two can be checked
 * against each other (docs/air.md 3.3). */
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (_mesa == NULL) {
		rw_print("a sampler needs the compiler library\n");
		return nil;
	}
	struct rwa_sampler_state state = {
		.address = { (unsigned)[descriptor sAddressMode],
			     (unsigned)[descriptor tAddressMode],
			     (unsigned)[descriptor rAddressMode] },
		.min_filter = (unsigned)[descriptor minFilter],
		.mag_filter = (unsigned)[descriptor magFilter],
		.mip_filter = (unsigned)[descriptor mipFilter],
		.compare_function = (unsigned)[descriptor compareFunction],
		.max_anisotropy = (unsigned)[descriptor maxAnisotropy],
		.normalized_coordinates = [descriptor normalizedCoordinates] ? 1 : 0,
		.lod_min = [descriptor lodMinClamp],
		.lod_max = [descriptor lodMaxClamp],
		/* M5b. lodAverage has no field: "This option is a performance hint. An
		 * implementation is free to ignore this property" (Apple's MTLSampler.h), and this
		 * one does. */
		.border_color = (unsigned)[descriptor borderColor],
	};
	if (hk_tracing()) {
		static unsigned lines;

		if (__atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < 16) {
			rw_print("sampler: address %u %u %u, filter %u %u %u, normalized %u, border %u\n", state.address[0],
				 state.address[1], state.address[2], state.min_filter, state.mag_filter, state.mip_filter,
				 state.normalized_coordinates, state.border_color);
		}
	}
	/* The Gallium library reads the shorter struct and has one border, transparent black
	 * (air_metal.c, sampler_state_from_metal), so a sampler asking for another would read as
	 * right and be wrong: said, once. The Vulkan library binds it. */
	if (!mesa_is_vulkan && state.border_color != 0 &&
	    (state.address[0] == MTLSamplerAddressModeClampToBorderColor ||
	     state.address[1] == MTLSamplerAddressModeClampToBorderColor ||
	     state.address[2] == MTLSamplerAddressModeClampToBorderColor)) {
		RW_ONCE("a sampler clamps to a border colour other than transparent black, which "
			"the Gallium library has no way to bind: it reads transparent black\n");
	}
	struct rwa_sampler *sampler = mesa.sampler_create(_mesa, &state, why, sizeof(why));

	if (sampler == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[HKMetalSampler alloc] initWithHKDevice:self
							 sampler:sampler
						      descriptor:descriptor];
}

/*
 * The depth/stencil state, which _MTLDevice does not implement at all: unlike -compiler,
 * whose body is `mov x0, #0; ret`, this selector is simply absent from the base class's
 * method list, so the whole of it is a driver's. (Apple's own is AGXA11FamilyDepthStencilState
 * over the same _MTLDepthStencilState this one inherits.)
 *
 * MTLDepthStencilDescriptor is read here, in Metal's words, and translated by the compiler
 * library -- which is where the two enumerations that do NOT line up are turned over:
 * MTLStencilOperationInvert is 5 and Gallium's INVERT is 7, so a cast would turn "invert the
 * stencil" into "increment it and wrap", and that is a different picture rather than a
 * failure. This side only reads properties and names them.
 */
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	char why[256] = { 0 };

	if (descriptor == nil) {
		rw_print("a depth/stencil state needs a descriptor\n");
		return nil;
	}
	if (_mesa == NULL) {
		rw_print("a depth/stencil state needs the compiler library\n");
		return nil;
	}
	if (mesa.depth_stencil_create == NULL) {
		rw_print("this compiler library has no depth/stencil state, so it "
		         "is older than this driver and cannot test depth\n");
		return nil;
	}

	/* frontFaceStencil and backFaceStencil are null_resettable: the setter takes nil
	 * and the getter hands back Metal's default descriptor, so both are read without a
	 * nil check -- and when one is nil anyway, the zeroed face left behind is compare
	 * Always with every operation Keep, which is that same default. */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };
	struct rwa_depth_stencil_state state = {
		.depth_compare_function = (unsigned)[descriptor depthCompareFunction],
		.depth_write_enabled = [descriptor isDepthWriteEnabled] ? 1 : 0,
	};
	struct rwa_stencil_face *out[2] = { &state.front, &state.back };

	for (unsigned i = 0; i < 2; i++) {
		if (faces[i] == nil) {
			*out[i] = (struct rwa_stencil_face){
				.compare_function = MTLCompareFunctionAlways,
				.read_mask = 0xffffffff, .write_mask = 0xffffffff,
			};
			continue;
		}
		*out[i] = (struct rwa_stencil_face){
			.compare_function = (unsigned)[faces[i] stencilCompareFunction],
			.stencil_fail_operation = (unsigned)[faces[i] stencilFailureOperation],
			.depth_fail_operation = (unsigned)[faces[i] depthFailureOperation],
			.depth_stencil_pass_operation =
			    (unsigned)[faces[i] depthStencilPassOperation],
			.read_mask = (unsigned)[faces[i] readMask],
			.write_mask = (unsigned)[faces[i] writeMask],
		};
	}

	struct rwa_depth_stencil *made = mesa.depth_stencil_create(_mesa, &state, why,
								   sizeof(why));

	if (made == NULL) {
		rw_print("%s\n", why);
		return nil;
	}
	return [[HKMetalDepthStencilState alloc] initWithHKDevice:self
								     state:made
								descriptor:descriptor];
}

/* _MTLDevice's own newCommandQueue and newCommandQueueWithMaxCommandBufferCount: are real
 * and both end here, where its body is `mov x0, #0; ret` (0x181aac688) -- a refusal that
 * announces itself as nothing at all. */
/*
 * -[MTLDevice newArgumentEncoderWithArguments:], the path an application takes when it
 * describes the set itself instead of taking it from a shader.
 *
 * Everything this encoder needs is in the descriptors -- how many members, which kind
 * each is, and therefore the length and the alignment -- and that is the whole of the
 * layout, because the records are keyed by Metal's `[[id(n)]]` and the FLAT index each
 * member is bound at comes from the pipeline's own shader at the draw. So an encoder made
 * this way records exactly as one made from a module does.
 *
 * Three shapes are refused BY NAME rather than encoded into something, each because the
 * frontend refuses the matching shader when it translates it (docs/air.md 8.2.1), so a
 * record of ours could never be asked for:
 *
 *   - a member that is not a pointer, a texture or a sampler: a VALUE inside the set,
 *     which this frontend binds nothing for;
 *   - an array of descriptors (arrayLength above 1), which no module in reach declares
 *     and which a shader could index at run time;
 *   - indices with a gap in them. Metal's own encodedLength is over the highest index, so
 *     a set declaring [[id(0)]] and [[id(5)]] is 48 bytes and not 16 -- and this library
 *     answers 8 bytes per member. Saying so is better than handing back a length the
 *     application would allocate too little memory from.
 */
- (id)newArgumentEncoderWithArguments:(NSArray *)arguments
{
	struct rwa_argbuf_layout layout = { 0 };
	NSUInteger count = [arguments count];

	if (mesa.argbuf_encode_buffer == NULL || mesa.module_argbufs == NULL) {
		rw_print("this compiler library has no argument buffers in it, so "
		         "an encoder would record what no draw could read\n");
		return nil;
	}
	if (count == 0 || count > RWA_MAX_ARGBUF_MEMBERS) {
		rw_print("an argument encoder of %lu members: this driver records "
		         "1 to %u\n", (unsigned long)count, RWA_MAX_ARGBUF_MEMBERS);
		return nil;
	}
	layout.metal_index = ~0u;      /* the application binds it where it likes */
	layout.alignment = 8;
	layout.member_count = (unsigned)count;
	layout.encoded_length = 8 * (unsigned)count;

	uint64_t seen = 0;
	for (NSUInteger i = 0; i < count; i++) {
		MTLArgumentDescriptor *d = [arguments objectAtIndex:i];
		NSUInteger index = [d index];
		NSUInteger type = [d dataType];
		NSUInteger array = [d arrayLength];
		unsigned kind;

		switch (type) {
		case MTLDataTypePointer: kind = RWA_ARGBUF_BUFFER; break;
		case MTLDataTypeTexture: kind = RWA_ARGBUF_TEXTURE; break;
		case MTLDataTypeSampler: kind = RWA_ARGBUF_SAMPLER; break;
		default:
			rw_print("member %lu of this argument encoder is "
			         "MTLDataType %lu, and this port binds a buffer, a texture or a "
			         "sampler member: a value inside a set is refused when the "
			         "shader is translated\n", (unsigned long)index,
			         (unsigned long)type);
			return nil;
		}
		if (array > 1) {
			rw_print("member %lu of this argument encoder is an array "
			         "of %lu descriptors, and this port's frontend refuses a member "
			         "spanning more than one descriptor index\n",
			         (unsigned long)index, (unsigned long)array);
			return nil;
		}
		if (index >= count || (seen & (1ull << index)) != 0) {
			rw_print("this argument encoder has %lu members and one of "
			         "them is index %lu: Metal's encodedLength is over the highest "
			         "index, so this driver will not answer a length for indices "
			         "with a gap or a repeat in them\n", (unsigned long)count,
			         (unsigned long)index);
			return nil;
		}
		seen |= 1ull << index;
		layout.members[i].id = (unsigned)index;
		layout.members[i].kind = kind;
		/* The flat index is the SHADER's to allocate, and there is no shader here:
		 * the draw takes it from the pipeline's own resource list, where the member
		 * is named by (set, id) exactly as the application named it. */
		layout.members[i].index = ~0u;
	}
	return [[HKMetalArgumentEncoder alloc] initWithHKDevice:self layout:&layout
							      fromModule:NO];
}

/* The iOS 16 form, which takes a reflection object (MTLBufferBinding) instead of
 * descriptors. Declined by name: every reflection object in this process comes from
 * Metal's own compiler, which is nil here (docs/metal.md 10.7), so an application that
 * has one did not get it from us and this driver has nothing to read out of it. */
- (id)newArgumentEncoderWithBufferBinding:(id)binding
{
	(void)binding;
	rw_print("-newArgumentEncoderWithBufferBinding: takes a reflection "
	         "object, and reflection on this port needs Metal's own compiler, which is "
	         "nil; ask a function for an encoder (-newArgumentEncoderWithBufferIndex:) or "
	         "describe the set with MTLArgumentDescriptors\n");
	return nil;
}

/*
 * And the function path, which the method installed into Metal's own _MTLFunction
 * forwards here.
 * The layout is read out of the function's bitcode -- the one thing this port has that
 * Metal's reflection has not -- so the numbers are the module's own.
 */
- (id)hkArgumentEncoderForFunction:(id)function bufferIndex:(NSUInteger)index
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (_mesa == NULL || mesa.module_argbufs == NULL) {
		rw_print("no compiler library, so the argument buffer layout of "
		         "this function cannot be read\n");
		return nil;
	}
	if (function == nil || ![function respondsToSelector:bitcode]) {
		rw_print("this function does not answer bitcodeData, so its "
		         "argument buffers cannot be read\n");
		return nil;
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		rw_print("this function has no bitcode\n");
		return nil;
	}

	char why[512] = { 0 };
	struct rwa_argbuf_layout sets[8];
	unsigned count = 0;

	if (mesa.module_argbufs(_mesa, [air bytes], [air length],
				[[function name] UTF8String], sets,
				(unsigned)(sizeof(sets) / sizeof(sets[0])), &count, why,
				sizeof(why)) != 0) {
		/* the frontend's own sentence, passed through: it names what it refused */
		rw_print("%s\n", why);
		return nil;
	}
	for (unsigned i = 0; i < count; i++) {
		if (sets[i].metal_index != (unsigned)index) {
			continue;
		}
		return [[HKMetalArgumentEncoder alloc] initWithHKDevice:self
									  layout:&sets[i]
								      fromModule:YES];
	}
	/* Metal's own words for this, and its own reason: there is no argument buffer at
	 * that index. Ours says which indices there ARE, because an application that asked
	 * for the wrong one has no other way to find out. */
	char which[96] = "";

	for (unsigned i = 0; i < count; i++) {
		char one[16];

		snprintf(one, sizeof(one), "%s%u", i ? ", " : " at ", sets[i].metal_index);
		strlcat(which, one, sizeof(which));
	}
	rw_print("bufferIndex %lu does not identify an argument buffer: this function "
	         "declares %u%s\n", (unsigned long)index, count, which);
	return nil;
}

- (id)newCommandQueueWithDescriptor:(id)descriptor
{
	/* Apple's own -newCommandQueue and -newCommandQueueWithMaxCommandBufferCount: build a
	 * descriptor and end here, so one usually arrives; when this is called directly with
	 * nil, one is made, because the base class validates it and refuses nil. */
	id use = descriptor != nil ? [[descriptor retain] autorelease] :
	    [[[MTLCommandQueueDescriptor alloc] init] autorelease];

	return [[HKMetalCommandQueue alloc] initWithHKDevice:self descriptor:use];
}

/*
 * Where subsystem (1) is finally asked to do its work.
 *
 * Both pipeline entry points of _MTLDevice route through -[_MTLDevice compiler], which is
 * `mov x0, #0; ret`, so a driver that leaves them alone gets nil and no error -- which is
 * exactly what this device did before this method existed. Overriding them, as AGX does, is
 * what keeps us out of Apple's compiler: the AIR comes from the function object and goes to
 * our own front end.
 *
 * The cost of that road, named in docs/metal.md 10.7: with -compiler still nil,
 * -newFunctionWithName:constantValues: and -[MTLFunction reflectionWithOptions:] return nil,
 * because those go through the compiler inside Metal's own classes where we cannot reach.
 */
- (id)newComputePipelineStateWithDescriptor:(id)descriptor
				    options:(NSUInteger)options
				 reflection:(id *)reflection
				      error:(NSError **)error
{
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;   /* reflection needs Metal's compiler; saying nil is honest */
	}
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

- (id)newComputePipelineStateWithDescriptor:(id)descriptor error:(NSError **)error
{
	return [self pipelineForFunction:[descriptor computeFunction] descriptor:descriptor
				  error:error];
}

/* _MTLDevice's own version of this builds an MTLComputePipelineDescriptorInternal and
 * re-sends to self, so overriding it is not strictly needed -- but a descriptor is what the
 * pipeline state's base class demands, and making one here keeps the three entry points
 * reaching the same code with the same arguments. */
/* The same hole on the compute side, and the same reason. */
- (void)newComputePipelineStateWithFunction:(id)function
			  completionHandler:(void (^)(id, NSError *))handler
{
	NSError *error = nil;
	id state = [self newComputePipelineStateWithFunction:function error:&error];

	if (handler != NULL) {
		handler(state, error);
	}
	[state release];
}

- (void)newComputePipelineStateWithDescriptor:(id)descriptor
				      options:(NSUInteger)options
			    completionHandler:(void (^)(id, id, NSError *))handler
{
	NSError *error = nil;
	id reflection = nil;
	id state = [self newComputePipelineStateWithDescriptor:descriptor
						      options:options
						   reflection:&reflection
							error:&error];

	if (handler != NULL) {
		handler(state, reflection, error);
	}
	[state release];
}

- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error
{
	Class internal = objc_getClass("MTLComputePipelineDescriptorInternal");
	id descriptor = internal != Nil ? [[internal alloc] init] :
	    (id)[[MTLComputePipelineDescriptor alloc] init];

	if (descriptor == nil) {
		return [self fail:error with:@"no pipeline descriptor class to build one from"];
	}
	[descriptor setComputeFunction:function];
	id state = [self pipelineForFunction:function descriptor:descriptor error:error];

	[descriptor release];
	return state;
}

/* One place where a function becomes a pipeline, so that the three entry points above
 * cannot drift apart. */
- (id)pipelineForFunction:(id)function descriptor:(id)descriptor error:(NSError **)error
{
	static SEL bitcode;

	if (bitcode == NULL) {
		bitcode = sel_getUid("bitcodeData");
	}
	if (function == nil) {
		return [self fail:error with:@"the descriptor has no compute function"];
	}
	if (_mesa == NULL) {
		return [self fail:error with:@"there is no GPU compiler here: either the Adreno "
					     @"was not started (gpu=1) or libhackiosair.dylib "
					     @"is missing"];
	}
	if ([function isKindOfClass:[HKStitchedFunction class]]) {
		return [self pipelineForStitched:function descriptor:descriptor error:error];
	}
	if (![function respondsToSelector:bitcode]) {
		return [self fail:error with:@"this function does not answer bitcodeData, so its "
					     @"AIR cannot be read"];
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(function, bitcode);

	if (air == nil || [air length] == 0) {
		return [self fail:error with:@"the function has no bitcode"];
	}

	char why[512] = { 0 };
	struct rwa_compute_info info = { 0 };
	/* M5c: FUNCTION CONSTANTS */
	unsigned constant_count = 0, constants_given = 0;
	const struct rwa_function_constant *constants =
	    hk_function_constants(function, &constant_count, &constants_given);
	{
		/* (WHAT A SYSTEM PIPELINE WAS GIVEN) */
		static unsigned lines;

		if (hk_tracing() && __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < 60) {
			char line[900];
			size_t at = (size_t)snprintf(line, sizeof(line), "compute %s, %u constants:",
						     [[function name] UTF8String], constant_count);

			for (unsigned i = 0; i < constant_count && at + 32 < sizeof(line); i++) {
				if (constants[i].bits != 0 || constants[i].bits_hi != 0) {
					at += (size_t)snprintf(line + at, sizeof(line) - at, " %u=%llx", constants[i].index,
							       (unsigned long long)constants[i].bits);
				}
			}
			rw_print("%s\n", line);
		}
	}
	/*
	 * The same function with the same constants is the same pipeline: VideoToolbox makes one per transfer and frees it after, where real
	 * Metal's own cache answers (run 051210: 39-83 dispatches and as many pipelines in every 5 s). Kept for the process's life, and only these.
	 */
	static NSMutableDictionary *kept_pipelines;
	NSString *kept_key = constants_given
	    ? [NSString stringWithFormat:@"%@ %zu %llx %u %llx", [function name], (size_t)[air length], (unsigned long long)hk_bytes_hash([air bytes], [air length]), constant_count,
				(unsigned long long)hk_bytes_hash(constants, constant_count * sizeof(*constants))] : nil;

	if (kept_key != nil) {
		@synchronized([NSObject class]) {
			id kept = [kept_pipelines objectForKey:kept_key];

			if (kept != nil) {
				return [kept retain];
			}
		}
	}
	const CFAbsoluteTime compute_began = CFAbsoluteTimeGetCurrent();
	struct rwa_compute *compute = constants_given
	    ? mesa.compute_create_constants(_mesa, [air bytes], [air length],
					    [[function name] UTF8String], constants,
					    constant_count, &info, why, sizeof(why))
	    : mesa.compute_create(_mesa, [air bytes], [air length],
				  [[function name] UTF8String], &info, why, sizeof(why));
	if (CFAbsoluteTimeGetCurrent() - compute_began > 0.25) {
		rw_print("compute %s took %.1f s to make, %zu bytes of AIR: %s\n", [[function name] UTF8String],
			 CFAbsoluteTimeGetCurrent() - compute_began, (size_t)[air length], compute != NULL ? "made" : why);
	}
	if (compute == NULL) {
		/* The front end's own sentence, passed through rather than replaced: it says
		 * which construct it refused, and an application can act on that. */
		return [self fail:error with:[NSString stringWithUTF8String:why]];
	}
	id made = [[HKMetalComputePipeline alloc] initWithHKDevice:self
							   compute:compute
							      info:&info
							descriptor:descriptor];

	if (kept_key != nil && made != nil) {
		@synchronized([NSObject class]) {
			if (kept_pipelines == nil) {
				kept_pipelines = [[NSMutableDictionary alloc] init];
			}
			if ([kept_pipelines count] < 256) {
				[kept_pipelines setObject:made forKey:kept_key];
			}
		}
	}
	return made;
}

- (id)fail:(NSError **)error with:(NSString *)reason
{
	if (error != NULL) {
		*error = [NSError errorWithDomain:@"HKMetal" code:1
					 userInfo:@{ NSLocalizedDescriptionKey: reason }];
	}
	return nil;
}

/*
 * The rest of what MTLDevice and MTLDeviceSPI require.
 *
 * Measured on the phone: 44 of the methods those two protocols require are answered by
 * nothing in the _MTLDevice chain (docs/metal.md 10.8). Until they are answered, any
 * application that asks one gets "unrecognized selector" and dies -- and that matters more
 * than it looks, because the gates inside Metal are assertions, so a process that asks a
 * question this device cannot answer does not degrade, it aborts, and launchd restarts it.
 *
 * So each of these answers, and each answers HONESTLY: a capability we have not got is
 * declined rather than claimed, a quantity we do not track says so in its comment rather
 * than inventing a number that something would then rely on. Where a number is a placeholder
 * it is marked as one; where the right authority is the compiler library rather than this
 * file, that is written down, because the next person to touch it will want to know.
 */

- (uint64_t)registryID
{
	/* Not invented: the registry's own identifier for the service Metal handed us. */
	uint64_t id = 0;

	if (_acceleratorService != IO_OBJECT_NULL &&
	    IORegistryEntryGetRegistryEntryID(_acceleratorService, &id) == KERN_SUCCESS) {
		return id;
	}
	return 0;
}

/* The a635 runs up to 1024 threads in a threadgroup, which is also what every pipeline the
 * compiler library has compiled reports back (10.15). The per-dimension limits are Metal's
 * usual shape for that; the authority should become the library itself once the contract has
 * a call for it. */
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }

/* The number _MTLDevice's own limits table was filled with for this feature profile. Also a
 * placeholder in the same sense: the hardware's figure belongs to Mesa, not here. */
- (NSUInteger)maxThreadgroupMemoryLength { return 16384; }

- (BOOL)hasUnifiedMemory { return YES; }   /* one memory on this SoC, shared with the CPU */

/* Nothing here tracks allocation yet, and two of these are read by applications deciding how
 * much to allocate, so they answer a figure that is plausible for this phone rather than 0,
 * which would read as "allocate nothing". That they are not measured is the point of this
 * comment. */
- (uint64_t)recommendedMaxWorkingSetSize { return 512ULL << 20; }
- (uint64_t)currentAllocatedSize         { return 0; }
- (uint64_t)sharedMemorySize             { return 512ULL << 20; }
- (uint64_t)dedicatedMemorySize          { return 0; }   /* none: the memory is shared */
- (uint64_t)maxBufferLengthInternal      { return 256ULL << 20; }

- (BOOL)isLowPower      { return NO; }
- (BOOL)isHeadless      { return NO; }
- (BOOL)isRemovable     { return NO; }
- (BOOL)supportPriorityBand { return NO; }

/* Declined rather than claimed: no sparse textures, and a yes here would be acted on.
 *
 * Depth24Unorm_Stencil8 stays NO now that there IS a depth path, and it is not a leftover:
 * Metal's own header marks that format API_UNAVAILABLE(ios), the compiler library refuses it
 * by name for the same reason, and iOS depth is Depth16Unorm, Depth32Float or
 * Depth32Float_Stencil8 -- which this device does answer for. A yes here would offer a
 * format no texture could be made of. */
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }

/*
 * One sample, STILL -- and this one needs its reason written down, because it is now the
 * narrower statement of the two this driver could make.
 *
 * The compiler library does make multisample textures since docs/metal.md 10.34: the
 * Adreno gives 2 and 4 samples, a render pass of that count fills one, and a shader reads
 * one with read(coord, sample). So "count == 1" is no longer what this device CAN do with
 * a multisample texture.
 *
 * It is what this device can do with one for an APPLICATION, and that is what the
 * selector asks. An application that is told four samples makes a four-sample attachment
 * and then asks for its result in a one-sample texture -- MTLStoreActionMultisampleResolve,
 * or MTLRenderPassColorAttachmentDescriptor.resolveTexture -- and this driver has neither:
 * the contract stores what it has and resolves nothing. AppleMetalGLRenderer is exactly
 * such an application (its own six multisample fragment shaders are the ones 10.34 is
 * about), and a GL context given an MSAA config it cannot resolve is worse off than one
 * given none. So the yes waits for the resolve, and the measurement of the texture does
 * not wait for the yes: a test binds one without asking.
 *
 * Note also that -supportsTextureSampleCount: is not a second answer to keep in step. It
 * is one instruction in _MTLDevice (0x181a17890) that tail-calls -supportsSampleCount:,
 * which _MTLDevice itself does not implement at all -- so this method is the whole of
 * both, and without it that forward would die on an unrecognized selector naming OUR
 * class (docs/metal.md 10.1's trap).
 */
/* (and now the resolve exists: -endEncoding carries out MTLStoreActionMultisampleResolve, so 2
 * and 4 samples -- what the Adreno gives, docs/metal.md 10.34 -- are what an application is told) */
- (BOOL)supportsSampleCount:(NSUInteger)count { return count == 1 || count == 2 || count == 4; }
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
{
	(void)type; (void)format; (void)samples;
	return MTLSizeMake(0, 0, 0);
}
- (MTLSize)sparseTileSizeWithTextureType:(NSUInteger)type
			     pixelFormat:(NSUInteger)format
			     sampleCount:(NSUInteger)samples
			  sparsePageSize:(NSInteger)page
{
	(void)page;
	return [self sparseTileSizeWithTextureType:type pixelFormat:format sampleCount:samples];
}

/* These are bitfields of floating-point behaviours a device claims. Claiming none is the
 * honest answer while nothing has verified any of them on this hardware -- and docs/air.md
 * is precisely the document that would have to grow the evidence first. */
- (uint64_t)singleFPConfig { return 0; }
- (uint64_t)halfFPConfig   { return 0; }
- (uint64_t)doubleFPConfig { return 0; }

- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(NSUInteger)format
{
	(void)format;
	return 16;   /* a placeholder: the real figure is per format and belongs to Mesa */
}

/* Heaps: an account of sizes, not a suballocator (HKMetalHeap). The sizes these two answer
 * are exactly what the heap counts, which is all Metal promises of them. */
- (id)newHeapWithDescriptor:(id)descriptor
{
	return [[HKMetalHeap alloc] initWithHKDevice:self descriptor:descriptor];
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;
	return (MTLSizeAndAlign){ rw_heap_round(length), RW_HEAP_ALIGN };
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(id)descriptor
{
	return (MTLSizeAndAlign){ rw_heap_round(rw_texture_bytes(descriptor)), RW_HEAP_ALIGN };
}

/* Everything else the two protocols require and this driver has not built. Each returns the
 * API's own failure value, so a caller finds out by the rule Metal already has, rather than
 * by a selector that does not exist. */
/* Fences order encoders of one queue on the GPU. Here every command buffer of the process runs
 * through one Gallium context in commit order, and freedreno orders the hazards inside it, so a
 * fence has nothing left to add: the object is real and its update and wait are the no-ops the
 * encoders already answer. */
- (id)newFence                                      { return [[HKMetalFence alloc] initWithHKDevice:self]; }
- (id)newEvent                                      { return [[HKMetalEvent alloc] initWithHKDevice:self shared:NO]; }
- (id)newSharedEvent                                { return [[HKMetalEvent alloc] initWithHKDevice:self shared:YES]; }
/* an event of another process's: nothing here is shared between processes (see -newSharedTextureWithDescriptor:) */
- (id)newSharedEventWithHandle:(id)handle
{
	(void)handle;
	RW_ONCE("a shared event from another process was asked for; nothing of this driver's "
		"crosses processes, so there is none\n");
	return nil;
}
/*
 * The one method -[CAMetalLayer nextDrawable] asks of a device (docs/metal.md 10.46):
 * allocate_drawable_texture builds an ordinary MTLTextureDescriptor, sets its private
 * rotation/isDrawable/framebufferOnly/usage and sends this. The surface, its geometry and
 * every refusal live in src/hkdrawable.m, because they answer CoreAnimation rather than
 * Metal; what is left here is the object, which has to be of this class.
 *
 * The refusal is printed rather than returned silently, and that is not a style choice:
 * CoreAnimation does not test what it gets back, so a bare nil becomes a CAMetalDrawable
 * whose -texture is nil, and the application finds out later and somewhere else.
 */
/* A surface as a texture of its own, which goes on being copied when the surface changes (hkFollowSurface). */
/*
 * DRAWN AND PUT BACK (M5g, the owner: 「applibrary這裡沒有bg」). A surface somebody made with rows this GPU cannot draw (a 1125x2436 of 4512 bytes a row, run 000352, not a multiple of
 * 64: "no drawable", and what the App Library blurs stayed black). Its rows are its maker's: making them wider in the kernel sheared what its maker went on writing at the width it
 * asked for (a 120x120 asked at 480 and given 512: Maps' Multi-Stop Routing icon, runs 002126 and 002349). So a texture of this process's own to draw into, and the same one, this surface's
 * only one here, whatever this process asks of the surface and in whichever order (a read first was a second texture that never saw the draws once they stopped: App Library black, run 010556); and for anybody else what it holds goes into the surface, at the surface's rows, at once after a draw
 * that follows a rest and a second after the last of a run of draws (hkPutBackSoon).
 */
- (id)hkDrawnBackTexture:(const struct rwa_texture_shape *)shape surface:(void *)s descriptor:(MTLTextureDescriptor *)d
{
	MTLTextureDescriptor *plain =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)shape->pixel_format
							       width:shape->width height:shape->height
							   mipmapped:NO];
	[plain setUsage:shape->usage];
	id made = [self newTextureWithDescriptor:plain];

	if (made == nil) {
		return nil;
	}
	[made hkDrawBackTo:s];
	return hk_with_descriptor_swizzle(made, d);
}

/*
 * A PLANE, BY ITS BYTES (hkdrawable.m), where the GPU cannot have it in place: a 16-byte texel wants rows of 256, and a
 * surface's rows are its maker's. VideoToolbox makes such a texture for one transfer, and waits for it: so its bytes come
 * over when it is made and go back before the work is said to be done (hkDrawBack), a CPU copy each way.
 */
- (id)hkPlaneTexture:(const struct rwa_texture_shape *)shape surface:(void *)s plane:(NSUInteger)p descriptor:(MTLTextureDescriptor *)d
{
	struct rwd_plane where;
	unsigned bytes_per_pixel = 0;
	char unused[256];

	if (rwd_plane_of(s, p, &where) != 0 || shape->height != where.height || rwd_format_of_metal(shape->pixel_format, NULL, &bytes_per_pixel, unused, sizeof(unused)) != 0 ||
	    (uint64_t)shape->width * bytes_per_pixel > where.row) {
		return nil;
	}
	MTLTextureDescriptor *plain =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)shape->pixel_format
							       width:shape->width height:shape->height
							   mipmapped:NO];
	[plain setUsage:shape->usage];
	id made = [self newTextureWithDescriptor:plain];

	if (made == nil) {
		return nil;
	}
	[made hkStandInForPlane:p of:s];
	return hk_with_descriptor_swizzle(made, d);
}

- (id)hkCopiedTexture:(const struct rwa_texture_shape *)shape surface:(void *)s descriptor:(MTLTextureDescriptor *)d
{
	MTLTextureDescriptor *plain =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)shape->pixel_format
							       width:shape->width height:shape->height
							   mipmapped:NO];
	/* (one that may yet be asked to be drawn into, if the format has it: DRAWN AND PUT BACK) */
	[plain setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	id made = [self newTextureWithDescriptor:plain];

	if (made == nil) {
		[plain setUsage:MTLTextureUsageShaderRead];
		made = [self newTextureWithDescriptor:plain];
	}
	if (made == nil) {
		return nil;
	}
	[made hkFollow:s];
	[made hkStandInFor:s];
	return hk_with_descriptor_swizzle(made, d);
}

/* A sample of a 420 surface's lines, to tell one picture from another; numbers only, no picture leaves the phone */
static uint64_t
hk_420_sum(const uint8_t *y, size_t ys, NSUInteger w, NSUInteger h)
{
	uint64_t sum = 1469598103934665603ULL;

	for (NSUInteger j = 0; j < h; j += 8) {
		const uint8_t *line = y + j * ys;

		for (NSUInteger i = 0; i < w; i += 8) {
			sum = (sum ^ line[i]) * 1099511628211ULL;
		}
	}
	return sum;
}

/*
 * One line of a 420 surface in RGBA, BT.601 as below, in 16-bit fixed point (6 bits of fraction): a picture of 1.7
 * million pixels every frame, where the floats of the first copy took 51 ms. Chroma centred up and down only, 3/4 of
 * the near row and 1/4 of the far.
 */
static void
hk_420_line(const uint8_t *yl, const uint8_t *near, const uint8_t *far, NSUInteger w, bool video, uint8_t *out)
{
	NSUInteger i = 0;

	for (; i + 16 <= w; i += 16) {
		const uint8x8x2_t n = vld2_u8(near + i), f = vld2_u8(far + i);
		int16x8_t cb = vreinterpretq_s16_u16(vmovl_u8(vrhadd_u8(n.val[0], vrhadd_u8(n.val[0], f.val[0]))));
		int16x8_t cr = vreinterpretq_s16_u16(vmovl_u8(vrhadd_u8(n.val[1], vrhadd_u8(n.val[1], f.val[1]))));
		const uint8x16_t yy = vld1q_u8(yl + i);
		int16x8_t y0, y1;

		cb = vsubq_s16(cb, vdupq_n_s16(128));
		cr = vsubq_s16(cr, vdupq_n_s16(128));
		if (video) {
			const uint8x16_t over = vqsubq_u8(yy, vdupq_n_u8(16));

			cb = vshrq_n_s16(vmulq_n_s16(cb, 73), 6);
			cr = vshrq_n_s16(vmulq_n_s16(cr, 73), 6);
			y0 = vreinterpretq_s16_u16(vmull_u8(vget_low_u8(over), vdup_n_u8(74)));
			y1 = vreinterpretq_s16_u16(vmull_u8(vget_high_u8(over), vdup_n_u8(74)));
		} else {
			y0 = vreinterpretq_s16_u16(vshll_n_u8(vget_low_u8(yy), 6));
			y1 = vreinterpretq_s16_u16(vshll_n_u8(vget_high_u8(yy), 6));
		}
		const int16x8_t ro = vmulq_n_s16(cr, 90), bo = vmulq_n_s16(cb, 113);
		const int16x8_t go = vnegq_s16(vaddq_s16(vmulq_n_s16(cb, 22), vmulq_n_s16(cr, 46)));
		const int16x8x2_t r2 = vzipq_s16(ro, ro), g2 = vzipq_s16(go, go), b2 = vzipq_s16(bo, bo);
		uint8x16x4_t px;

		px.val[0] = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(y0, r2.val[0]), 6), vqrshrun_n_s16(vqaddq_s16(y1, r2.val[1]), 6));
		px.val[1] = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(y0, g2.val[0]), 6), vqrshrun_n_s16(vqaddq_s16(y1, g2.val[1]), 6));
		px.val[2] = vcombine_u8(vqrshrun_n_s16(vqaddq_s16(y0, b2.val[0]), 6), vqrshrun_n_s16(vqaddq_s16(y1, b2.val[1]), 6));
		px.val[3] = vdupq_n_u8(255);
		vst4q_u8(out + i * 4, px);
	}
	for (; i < w; i++) {
		const NSUInteger x = i / 2 * 2;
		int cb = (3 * near[x] + far[x] + 2) / 4 - 128, cr = (3 * near[x + 1] + far[x + 1] + 2) / 4 - 128;
		int l = video ? ((int)yl[i] > 16 ? (yl[i] - 16) * 74 : 0) : yl[i] * 64;

		if (video) {
			cb = cb * 73 / 64;
			cr = cr * 73 / 64;
		}
		const int rgb[3] = { (l + 90 * cr + 32) >> 6, (l - 22 * cb - 46 * cr + 32) >> 6, (l + 113 * cb + 32) >> 6 };

		for (unsigned k = 0; k < 3; k++) {
			out[i * 4 + k] = rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : (uint8_t)rgb[k];
		}
		out[i * 4 + 3] = 255;
	}
}

/*
 * MTLPixelFormatYCBCR8_420_2P (M5d), which CoreAnimation asks for by name for a 420f or 420v surface:
 * the stock wallpaper's Head.HEIC decodes to one, and without it the layer's mask is what shows, as a
 * red shape. A texture of one format over two planes needs a VkSamplerYcbcrConversion in an immutable
 * sampler, which no pipeline here can know it needs when it is made. So this texture is a copy, made
 * when the surface is asked for, with the chroma interpolated, and made again when the surface's seed
 * has moved and the texture is about to be drawn with (hkBiplanarLook). Once only, a pool's surfaces
 * came round for ever as the pictures they first held: the camera's preview, a ring of four (K38,
 * 「相机预览还是loop」). The file biplanar_once in share/probes puts that back, to compare.
 *
 * In RGB, not YCbCr: the pipeline CoreAnimation draws it with, PBGRABsapXm_TimgA2S1Xhf_Ixrg, has no matrix
 * in it (fc_has_ycbcr_matrix is not set), so sampling this format on Apple's GPU answers colours already.
 * BT.601 is measured, not assumed: Head.HEIC's middle is Y 105, Cb 141, Cr 83 on the phone (run 141155) and
 * this Mac's own decoder makes that (42, 133, 128). Real Metal on an M1, format 500 over a surface of those
 * three, answers exactly that for 420f and (32, 135, 130) for 420v, which is this matrix at each range, with
 * alpha 1; R8 of plane 0 and RG8 of plane 1 answer the raw bytes. A surface tagged 709 was not tried.
 */
- (id)hkBiplanarTexture:(MTLTextureDescriptor *)d surface:(void *)s raw:(BOOL)raw
{
	const uint64_t biplanar_began = hk_now_us();
	int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
	size_t (*planes)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
	void *(*base)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddressOfPlane");
	size_t (*row)(void *, size_t) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRowOfPlane");
	const NSUInteger w = [d width], h = [d height];

	if (lock == NULL || unlock == NULL || planes == NULL || base == NULL || row == NULL || planes(s) != 2 ||
	    w == 0 || h == 0) {
		RW_ONCE("a 420 surface of %lu planes that this driver cannot read\n",
			planes != NULL ? (unsigned long)planes(s) : 0ul);
		return nil;
	}
	uint8_t *rgba = malloc(w * h * 4);

	if (rgba == NULL || lock(s, 1 /* kIOSurfaceLockReadOnly */, NULL) != 0) {
		free(rgba);
		return nil;
	}
	const uint8_t *y = base(s, 0), *uv = base(s, 1);
	const size_t ys = row(s, 0), uvs = row(s, 1);
	const NSUInteger cw = (w + 1) / 2, ch = (h + 1) / 2;
	uint32_t (*pixel_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	const bool video = pixel_format != NULL && (pixel_format(s) & 0xff) == 'v';
	const float y_scale = video ? 255.0f / 219.0f : 1.0f, y_base = video ? 16.0f : 0.0f;
	const float c_scale = video ? 255.0f / 224.0f : 1.0f;

	for (NSUInteger j = 0; j < h; j++) {
		const uint8_t *yl = y + j * ys;
		/* centred chroma, half a texel each way: 3/4 of the near row and 1/4 of the far */
		const NSUInteger c0 = j / 2;
		const NSUInteger c1 = (j & 1) ? MIN(c0 + 1, ch - 1) : (c0 > 0 ? c0 - 1 : 0);
		const uint8_t *near = uv + c0 * uvs, *far = uv + c1 * uvs;
		uint8_t *out = rgba + j * w * 4;

		for (NSUInteger i = 0; i < w; i++) {
			const NSUInteger x0 = i / 2;
			const NSUInteger x1 = (i & 1) ? MIN(x0 + 1, cw - 1) : (x0 > 0 ? x0 - 1 : 0);

			float chroma[2], rgb[3];

			for (unsigned k = 0; k < 2; k++) {
				chroma[k] = ((9.0f * near[x0 * 2 + k] + 3.0f * near[x1 * 2 + k] + 3.0f * far[x0 * 2 + k] +
					      far[x1 * 2 + k]) / 16.0f - 128.0f) * c_scale;
			}
			if (raw) {
				const float cb = chroma[0] / c_scale + 128.0f, cr = chroma[1] / c_scale + 128.0f;

				out[i * 4] = (uint8_t)(cr + 0.5f);
				out[i * 4 + 1] = yl[i];
				out[i * 4 + 2] = (uint8_t)(cb + 0.5f);
				out[i * 4 + 3] = 255;
				continue;
			}

			const float luma = ((float)yl[i] - y_base) * y_scale;

			rgb[0] = luma + 1.402f * chroma[1];
			rgb[1] = luma - 0.344136f * chroma[0] - 0.714136f * chroma[1];
			rgb[2] = luma + 1.772f * chroma[0];
			for (unsigned k = 0; k < 3; k++) {
				out[i * 4 + k] = rgb[k] <= 0.0f ? 0 : rgb[k] >= 255.0f ? 255 : (uint8_t)(rgb[k] + 0.5f);
			}
			out[i * 4 + 3] = 255;
		}
	}

	MTLTextureDescriptor *plain = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
											 width:w height:h mipmapped:NO];
	[plain setUsage:MTLTextureUsageShaderRead];
	id made = [self newTextureWithDescriptor:plain];

	[made replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
	free(rgba);
	RW_ONCE("a 420 surface %lux%lu read into RGB: rows %zu and %zu, at the middle Y Cb Cr %u %u %u\n",
		 (unsigned long)w, (unsigned long)h, ys, uvs, y[h / 2 * ys + w / 2], uv[h / 4 * uvs + w / 2], uv[h / 4 * uvs + w / 2 + 1]);
	if (!raw) {
		uint32_t (*seed_of)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");

		[made hkBiplanarOf:s seed:seed_of != NULL ? seed_of(s) : 0 sum:hk_420_sum(y, ys, w, h)];
	}
	unlock(s, 1, NULL);
	atomic_fetch_add(&hk_stat.biplanar, 1);
	atomic_fetch_add(&hk_stat.biplanar_us, hk_now_us() - biplanar_began);
	return made;
}

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)s plane:(NSUInteger)p
{
	const uint64_t import_began = hk_now_us();
	char why[512] = { 0 };
	struct rwd_surface took;
	struct rwa_texture_shape shape;   /* this file's own declaration; rwd_texture_shape is the
					   * same layout under the other file's name, and the cast
					   * below is the one place the two meet */
	struct rwa_texture *texture;

	if (_mesa == NULL) {
		rw_print("a drawable's texture needs the compiler library\n");
		return nil;
	}
	memset(&shape, 0, sizeof(shape));
	shape.pixel_format = (unsigned)[d pixelFormat];
	shape.type = (unsigned)[d textureType];
	shape.width = (unsigned)[d width];
	shape.height = (unsigned)[d height];
	shape.depth = (unsigned)[d depth];
	shape.array_length = (unsigned)[d arrayLength];
	shape.mip_levels = (unsigned)[d mipmapLevelCount];
	shape.sample_count = (unsigned)[d sampleCount];
	shape.usage = (unsigned)[d usage];
	hk_note_packed16("iosurface", shape.pixel_format, shape.usage, [d swizzle], NULL, s);
	if (p == 0) {
		/* (DRAWN AND PUT BACK: this surface already has its one texture in this process, a copy that follows it or a stand-in) */
		@synchronized([HKMetalTexture class]) {
			for (unsigned i = 0; i < 64; i++) {
				if (hk_stand_ins[i].surface == s && [hk_stand_ins[i].texture pixelFormat] == shape.pixel_format) {
					if ((shape.usage & MTLTextureUsageRenderTarget) && ([hk_stand_ins[i].texture usage] & MTLTextureUsageRenderTarget)) {
						[hk_stand_ins[i].texture hkStartDrawingBack];
						return [hk_stand_ins[i].texture retain];
					}
					if (!(shape.usage & MTLTextureUsageRenderTarget)) {
						return [hk_stand_ins[i].texture retain];
					}
				}
			}
		}
	}
	if (shape.pixel_format == 500 && p == 0) {
		return [self hkBiplanarTexture:d surface:s raw:NO];
	}
	/*
	 * M5e, format 540: what VideoToolbox asks for a 420f or 420v source, with the matrix in its own shader
	 * (_FC_src0UseAdvancedMTLPixelFormat). Real Metal on an M1 answers the planes' raw values in the order Cr, Y, Cb,
	 * alpha 1, for both ranges: 83 105 141 255 over Y 105, Cb 141, Cr 83; centred chroma, by a read as well as a sample
	 * (rows surface-540-*). The same copy as format 500's, and the same limit, which does not hurt here:
	 * VideoToolbox makes its texture for one transfer and the source has its picture by then.
	 */
	if (shape.pixel_format == 540 && p == 0 && !(shape.usage & MTLTextureUsageShaderWrite)) {
		return [self hkBiplanarTexture:d surface:s raw:YES];
	}
	/*
	 * A row this GPU cannot read in place (a 120x120 BGRA surface of 480 bytes a row, run 142131:
	 * a layer's own picture, which the kernel's aligned surfaces are not). A copy that follows the
	 * surface (hkFollowSurface). Only what is only read.
	 */
	{
		size_t (*bytes_per_row)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
		void *(*base_address)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
		int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
		int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
		const size_t row = bytes_per_row != NULL ? bytes_per_row(s) : 64;

		if (row % 64 != 0 && p == 0 && !(shape.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) && base_address != NULL &&
		    lock != NULL && unlock != NULL) {
			id copied = [self hkCopiedTexture:&shape surface:s descriptor:d];

			if (copied != nil) {
				RW_ONCE("a surface of %zu bytes a row, which this GPU reads only at 64: a copy\n", row);
				return copied;
			}
		}
	}
	{
		/* (WHAT A SYSTEM PIPELINE WAS GIVEN) one line per shape of import */
		static struct { unsigned format, width, height; unsigned long plane; uint32_t fourcc; } seen[64];
		static unsigned count;
		uint32_t (*pixel_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
		uint32_t fourcc = pixel_format != NULL ? pixel_format(s) : 0;
		bool is_new = count < 64;

		@synchronized([NSObject class]) {
			for (unsigned i = 0; is_new && i < count; i++) {
				is_new = !(seen[i].format == shape.pixel_format && seen[i].width == shape.width &&
					   seen[i].height == shape.height && seen[i].plane == p &&
					   seen[i].fourcc == fourcc);
			}
			if (is_new) {
				seen[count].format = shape.pixel_format;
				seen[count].width = shape.width;
				seen[count].height = shape.height;
				seen[count].plane = p;
				seen[count++].fourcc = fourcc;
			}
		}
		if (is_new && (hk_tracing() || p != 0 || (shape.usage & MTLTextureUsageShaderWrite))) {
			rw_print("iosurface import: format %u plane %lu %ux%u usage %#x fourcc %c%c%c%c\n",
				 shape.pixel_format, p, shape.width, shape.height, shape.usage,
				 (int)(fourcc >> 24 & 0xff), (int)(fourcc >> 16 & 0xff), (int)(fourcc >> 8 & 0xff),
				 (int)(fourcc & 0xff));
		}
	}
	texture = rwd_texture_for_surface(_mesa, s, p, (const struct rwd_texture_shape *)&shape,
					  &took, why, sizeof(why));
	if (texture == NULL) {
		/*
		 * M5d: what this GPU cannot have in place but this process can read (a surface over a process's
		 * own memory, run 152245: a 1125x1905 BGRA, the kernel says so in `why`). A copy that follows the
		 * surface, as in the misaligned case above; what is written through this texture has to be in place.
		 */
		id copied = (p == 0 && !(shape.usage & MTLTextureUsageRenderTarget) &&
			     !(shape.usage & MTLTextureUsageShaderWrite))
				? [self hkCopiedTexture:&shape surface:s descriptor:d] : nil;

		if (copied != nil) {
			RW_ONCE("a surface read as a copy: %s\n", why);
			atomic_fetch_add(&hk_stat.copied, 1);
			return copied;
		}
		id back = p == 0 && (shape.usage & MTLTextureUsageRenderTarget) && !(shape.usage & MTLTextureUsageShaderWrite) && shape.mip_levels <= 1 && shape.sample_count <= 1
		    ? [self hkDrawnBackTexture:&shape surface:s descriptor:d] : nil;

		if (back != nil) {
			RW_ONCE("a surface drawn into through a texture of its own and put back: %s\n", why);
			return back;
		}
		id plane_copy = shape.mip_levels <= 1 && shape.sample_count <= 1 && !(shape.usage & MTLTextureUsageRenderTarget)
		    ? [self hkPlaneTexture:&shape surface:s plane:p descriptor:d] : nil;

		if (plane_copy != nil) {
			RW_ONCE("a plane read and written through a texture of its own: %s\n", why);
			return plane_copy;
		}
		RW_AGAIN("no drawable: %s\n", why);
		return nil;
	}
	HKMetalTexture *over = [[HKMetalTexture alloc] initWithHKDevice:self
									   texture:texture
									     shape:&shape];

	if (hk_frame_dumping() || access("/hackios/share/probes/census", F_OK) == 0) {
		[over hkRememberSurface:s];
	}
	atomic_fetch_add(&hk_stat.imports, 1);
	atomic_fetch_add(&hk_stat.import_us, hk_now_us() - import_began);
	return hk_with_descriptor_swizzle(over, d);
}
- (id)newBufferWithIOSurface:(void *)surface        { (void)surface; return nil; }
- (id)newBufferWithBytesNoCopy:(void *)bytes
			length:(NSUInteger)length
		       options:(NSUInteger)options
		   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	/*
	 * The caller's own pages are not the GPU's here (the kernel would have to map them), so this is a buffer of ours with the caller's bytes copied in, and what
	 * the GPU writes through a texture over it or as a compute buffer copied back before the work is said to be done (hkDrawBack). Nil was CoreImage's blank image
	 * whenever a bitmap came to whole pages: it renders straight into such memory (128x128, 256x256, 512x512, 1024x768 empty and the others right, run 051210).
	 */
	HKMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer == nil) {
		return nil;
	}
	memcpy([buffer contents], bytes, length);
	[buffer hkWrap:bytes deallocator:deallocator];
	return buffer;
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
			      maxCommandCount:(NSUInteger)count
				      options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectCommandBufferWithDescriptor:(id)d
				    maxCount:(NSUInteger)count
				     options:(NSUInteger)options
{
	(void)d; (void)count; (void)options;
	return nil;
}
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)b { (void)b; return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)b  { (void)b; return nil; }
- (id)newIndirectArgumentBufferLayoutWithStructType:(id)t { (void)t; return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)d      { (void)d; return nil; }
- (id)newIntersectionFunctionTableWithDescriptor:(id)d { (void)d; return nil; }

/*
 * The last paths in the chain that would still abort.
 *
 * Asking the device whether it responds to these says yes -- they are real entries in
 * _MTLDevice's method list -- and calling one aborts, because each body is
 * [self doesNotRecognizeSelector:_cmd]. Seven methods of _MTLDevice are abstract in that
 * way (tools/metal/absmeth.py), one of them is -initWithAcceleratorPort: which this driver
 * implements, and the other six are these: ray tracing. Overriding them to decline turns the
 * last aborting paths into ordinary nil, which is what the "0 unanswered" measurement needs
 * in order to mean anything.
 */
- (id)newAccelerationStructureWithSize:(NSUInteger)size { (void)size; return nil; }
- (id)newAccelerationStructureWithSize:(NSUInteger)size resourceIndex:(NSUInteger)index
{
	(void)size; (void)index;
	return nil;
}
- (id)newAccelerationStructureWithSize:(NSUInteger)size withDescriptor:(id)descriptor
{
	(void)size; (void)descriptor;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer offset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	return nil;
}
- (id)newAccelerationStructureWithBuffer:(id)buffer
				  offset:(NSUInteger)offset
			   resourceIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	return nil;
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size
{
	(void)size;
	return (MTLSizeAndAlign){ 0, 0 };
}

- (BOOL)metalAssertionsEnabled { return _assertions; }
- (void)setMetalAssertionsEnabled:(BOOL)enabled { _assertions = enabled; }

/* Without this the device inherits _MTLDevice's own answer, which is "Apple GPU" -- true of
 * the code and false of the hardware. The name is Mesa's renderer string for this chip
 * (docs/air.md: `Adreno 7c+ Gen 3, chip 0x06030500`), and the identifier beside it is asked
 * of the kernel rather than written down, so a different phone says something different.
 * Nothing in Metal's registration reads this; a person reading a log does. */
- (NSString *)name
{
	uint64_t chip = 0;

	if (gpu_param(_gpu, 0x03, &chip)) {
		/* by chip, because Mesa's own name is "FD650" and hk's string is in its logs */
		unsigned id = (unsigned)(chip & 0xffffffffu);
		const char *model = (id & 0xffff0000u) == 0x06050000u ? "Adreno 650" :
				    (id & 0xffff0000u) == 0x06030000u ? "Adreno 7c+ Gen 3" : "Adreno";

		return [NSString stringWithFormat:@"%s (chip %#x, HK)", model, id];
	}
	return @"Adreno (HK; the GPU is not started)";
}

/*
 * The answer that could not be "nothing", and the first place in this driver where that is
 * true. logs/210311-run.log is backboardd dying every ten seconds on
 *
 *   signal 11 at 0x10, pc 0x181a3ca3c, called from 0x18847f6dc
 *
 * which is MTLPipelineLibraryBuilder::newLibraryWithFile + 836, reached from
 * CA::OGL::new_metal_context + 1808 -- QuartzCore asking this device for the precompiled
 * pipelines of its own default.pipelinelib. Nothing of ours is on that stack:
 * -[_MTLDevice newPipelineLibraryWithFilePath:error:] is three instructions (0x181a3d9b0,
 * `mov x1, x0; ldr x0, [x0, #0x180]; b`) that hand the device and its own builder ivar to
 * Apple's C++, which gets as far as parsing pipelines.desc out of the image and then asks us
 * one question. The three instructions that matter:
 *
 *   0x181a3c9ec  objc_opt_respondsToSelector(device, "targetDeviceInfo")   -> YES
 *   0x181a3c9f8  objc_msgSend$targetDeviceInfo                             -> NULL
 *   0x181a3ca3c  ldr x8, [x23, #0x10]                                      -> 0x10
 *
 * Both halves of that are Apple's. `_MTLDevice` HAS an entry for -targetDeviceInfo and its
 * body is `mov x0, #0; ret` (0x181aa8d98 -- tools/metal/absmeth.py calls that a nil-stub),
 * so the runtime answers the question YES on our behalf and the pointer is NULL; and the
 * caller treats a YES as a promise and dereferences what comes back. Declining is therefore
 * not on the table: returning nil IS what crashes, and the only way to make the runtime say
 * NO would be to lie in -respondsToSelector:, which would also lie to every caller that
 * does not ask first. Of the eight objc_opt_respondsToSelector sites in all of
 * Metal.framework this is the only one whose answer is a raw pointer rather than a scalar or
 * an object -- the other seven ask about -shaderDebugInfoCaching, -maximumComputeSubstreams
 * (twice), -presentWithOptions: (twice), -boolValue and one selector held in a register.
 *
 * The shape can recur, and the population is counted rather than feared: of `_MTLDevice`'s
 * 522 instance methods 74 are nil-stubs, and 14 of those are declared to return a raw
 * pointer (absmeth.py joined to protometh.py by implementation address). Thirteen of the
 * fourteen are the pipeline-cache key family -- `*FunctionKeyWith...:keySize:` and
 * `get*FunctionId:compilerOptions:compilerOptionsSize:` -- which is reached only once a
 * pipeline cache actually holds something; this is the fourteenth, and the first one
 * anything asked for.
 *
 * So the struct is real, and its contents are the ones Apple's own code writes in the branch
 * it takes for a device that does not respond (0x181a3ca04): zero, zero, and the device's
 * -name with the spaces removed. Its shape is known twice over, from two sources that agree
 * -- that branch, which builds one on its own stack, and the type MTLDeviceSPI records for
 * this method, `r^{MTLTargetDeviceArch=QI*}16@0:8`. Each field by its reader:
 *
 *   +0x00  nobody on this path reads it, and Apple's branch writes 0.
 *   +0x08  read by -[_MTLPipelineCache initWithFilePath:readOnly:deviceInfo:] (0x181a3b720),
 *          which pairs it with 1 into an MTLCompilerCache::Version for
 *          MTLCompilerCache::createPipelineDataCache. That version decides whether a cache
 *          FILE is stale; this cache is opened read-only (w3=1 at 0x181a3ca64) over a file
 *          no image here has, so there is nothing for it to invalidate, and 0 is the number
 *          Apple's own branch uses. It becomes a real question the day this driver WRITES a
 *          pipeline cache, and not before.
 *   +0x10  formatted into "%s/pipelines.%s" (0x181a3ca48) -- the per-GPU cache file inside
 *          QuartzCore.framework/default.pipelinelib. It must be a C string and not NULL:
 *          Metal's second cache, the one for `functions`, it builds with a zeroed struct and
 *          an EMPTY name itself (0x181a3ca7c), which is the measurement that says a zeroed
 *          struct is acceptable to _MTLPipelineCache.
 *
 * What this buys is a refusal with a reason instead of a crash, and the refusal is the
 * honest one: that image ships `functions` and `pipelines.desc` and no pipelines.* for any
 * GPU at all (not even the A11 this phone was born with), so the cache comes back nil,
 * Metal fills the NSError with "Could not open pipeline cache file: %s" (code 6,
 * 0x181a3cb2c), and QuartzCore logs "Metal failed to load pipeline library: %s" and carries
 * on -- 0x18847f6f0 is a `cbnz`, not an assert. Nothing is lost by it: a precompiled
 * pipeline in that cache would be A11 machine code, which this driver could not run anyway.
 * The pipelines CoreAnimation needs get compiled the ordinary way, through
 * -newRenderPipelineStateWithDescriptor: and AIR.
 */
- (const struct MTLTargetDeviceArch *)targetDeviceInfo
{
	/* The whole point of this method is that a NULL here is fatal, so it must not depend on
	 * an initialiser having run: an object built with plain -init rather than Metal's
	 * -initWithAcceleratorPort: has a zeroed ivar, and an empty name is what Apple's own
	 * second cache passes for itself anyway. The array is zeroed by the runtime, so it
	 * already spells "". */
	if (_target.name == NULL) {
		_target.name = _targetName;
	}
	RW_ONCE("Metal asked for targetDeviceInfo (pipelines.%s); it is answered "
		"with a real struct because Apple's own answer here is a NULL this driver does "
		"not get to decline\n", _target.name);
	return &_target;
}

/* The two buffer entry points of MTLDevice that nothing in the chain answers. Anything
 * Metal would ask about storage modes is answered by the buffer itself. */
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	(void)options;   /* every allocation here is CPU-visible; the buffer says so honestly */
	return [[HKMetalBuffer alloc] initWithDevice:self gpu:_gpu length:length];
}

- (id)hkTakeBytesChunk
{
	id chunk = nil;

	@synchronized (self) {
		if ([_bytesChunks count] != 0) {
			chunk = [[_bytesChunks lastObject] retain];
			[_bytesChunks removeLastObject];
		}
	}
	return chunk != nil ? chunk : [self newBufferWithLength:RW_BYTES_CHUNK options:0];
}

- (void)hkGiveBackBytesChunks:(NSArray *)chunks
{
	@synchronized (self) {
		if (_bytesChunks == nil) {
			_bytesChunks = [[NSMutableArray alloc] init];
		}
		for (id chunk in chunks) {
			if ([_bytesChunks count] < RW_BYTES_CHUNKS_KEPT) {
				[_bytesChunks addObject:chunk];
			}
		}
	}
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(NSUInteger)options
{
	HKMetalBuffer *buffer = [self newBufferWithLength:length options:options];

	if (buffer != nil && bytes != NULL && [buffer contents] != NULL) {
		memcpy([buffer contents], bytes, length);
	}
	return buffer;
}

/* Profile 0's own table says maxVertexAmplificationCount is 1, so anything above one is
 * not supported, and saying otherwise would be a lie Metal would act on. */
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
	return count <= 1;
}

@end

/*
 * BUFFERS KEPT FOR THE NEXT ONE. Each buffer here is its own VkDeviceMemory, mapped: about 0.5 ms to make, and with a map's tiles
 * coming in Maps makes up to 1900 in 5 s, about a second of its main thread, 70% of them let go again soon after (run 104515, K35;
 * 「不跟手」). So small ones are kept by size, oldest first, and given again zeroed, as a new one is. A buffer let go of may
 * still be in the GPU's hands, so the oldest is given again only when the library says the work that last named it has finished
 * (rwa_buffer_busy: what it frees by), and otherwise a new one is made, with no waiting. Not one that has been an argument
 * buffer: the library keeps records of those that only destroying forgets. A member of an argument buffer let go of stays in that
 * record, alive; only a draw through a member the application has released could see the next user's bytes.
 * The file buffer_no_pool in share/probes: none, to compare.
 */
#define HK_POOL_CLASSES 32
#define HK_POOL_EACH    128
#define HK_POOL_BYTES   (32u << 20)
static struct rwa_buffer *hk_pool[HK_POOL_CLASSES][HK_POOL_EACH];
static unsigned hk_pool_head[HK_POOL_CLASSES], hk_pool_count[HK_POOL_CLASSES];
static size_t hk_pool_bytes;
static void *hk_pool_device;
static pthread_mutex_t hk_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Its class, and the size a buffer of that class has: to 64 KB by 4 KB, to 1 MB by 64 KB; -1: too big to keep */
static int
hk_pool_class(size_t length, size_t *rounded)
{
	if (length == 0 || length > (1u << 20)) {
		return -1;
	}
	if (length <= (64u << 10)) {
		*rounded = ((length - 1) / 4096 + 1) * 4096;
		return (int)((length - 1) / 4096);
	}
	*rounded = ((length - 1) / 65536 + 1) * 65536;
	return 16 + (int)((length - 1) / 65536);
}

static bool
hk_pool_on(void)
{
	static int on = -1;

	if (on < 0) {
		on = mesa.buffer_busy != NULL && access("/hackios/share/probes/buffer_no_pool", F_OK) != 0;
	}
	return on != 0;
}

static struct rwa_buffer *
hk_pool_take(void *device, size_t length)
{
	struct rwa_buffer *found = NULL;
	size_t rounded = 0;
	const int c = hk_pool_class(length, &rounded);

	if (c < 0 || !hk_pool_on()) {
		return NULL;
	}
	pthread_mutex_lock(&hk_pool_lock);
	if (device == hk_pool_device && hk_pool_count[c] != 0) {
		found = hk_pool[c][hk_pool_head[c]];
		hk_pool_head[c] = (hk_pool_head[c] + 1) % HK_POOL_EACH;
		hk_pool_count[c]--;
		hk_pool_bytes -= rounded;
	}
	pthread_mutex_unlock(&hk_pool_lock);
	if (found != NULL && mesa.buffer_busy(found)) {
		bool back = false;

		/* (asked with the lock let go: the library may have to ask the kernel) */
		pthread_mutex_lock(&hk_pool_lock);
		if (hk_pool_count[c] < HK_POOL_EACH) {
			hk_pool_head[c] = (hk_pool_head[c] + HK_POOL_EACH - 1) % HK_POOL_EACH;
			hk_pool[c][hk_pool_head[c]] = found;
			hk_pool_count[c]++;
			hk_pool_bytes += rounded;
			back = true;
		}
		pthread_mutex_unlock(&hk_pool_lock);
		if (!back) {
			mesa.buffer_destroy(found);
		}
		atomic_fetch_add(&hk_live.pool_busy, 1);
		return NULL;
	}
	if (found != NULL) {
		atomic_fetch_add(&hk_live.pool_taken, 1);
	}
	return found;
}

static bool
hk_pool_put(void *device, struct rwa_buffer *buffer, size_t length)
{
	size_t rounded = 0;
	const int c = hk_pool_class(length, &rounded);
	bool kept = false;

	if (c < 0 || !hk_pool_on()) {
		return false;
	}
	pthread_mutex_lock(&hk_pool_lock);
	if (hk_pool_device == NULL) {
		hk_pool_device = device;
	}
	if (device == hk_pool_device && hk_pool_count[c] < HK_POOL_EACH && hk_pool_bytes + rounded <= HK_POOL_BYTES) {
		const unsigned at = (hk_pool_head[c] + hk_pool_count[c]) % HK_POOL_EACH;

		hk_pool[c][at] = buffer;
		hk_pool_count[c]++;
		hk_pool_bytes += rounded;
		kept = true;
	}
	pthread_mutex_unlock(&hk_pool_lock);
	if (kept) {
		atomic_fetch_add(&hk_live.pool_kept, 1);
	}
	return kept;
}

@implementation HKMetalBuffer

- (void)hkNeverKeep
{
	_neverKept = YES;
}

- (void)hkWrap:(void *)bytes deallocator:(void (^)(void *, NSUInteger))deallocator
{
	_wrapped = bytes;
	_wrappedGone = [deallocator copy];
	atomic_fetch_add(&hk_draw_backs, 1);
}

- (void)hkWrappedBack
{
	if (_wrapped != NULL) {
		memcpy(_wrapped, [self contents], [self length]);
	}
}

+ (void)load
{
	Protocol *p = objc_getProtocol("MTLBuffer");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBuffer) refused\n");
	}
}

- (instancetype)initWithDevice:(HKMetalDevice *)device
			   gpu:(io_connect_t)gpu
			length:(NSUInteger)length
{
	struct gem_new request = { .size = length, .flags = 0, .handle = 0 };
	size_t size = sizeof(request);

	self = [super init];
	if (self == nil) {
		return nil;
	}

	/* Two ways to the same kernel. When the compiler library is loaded, the memory comes
	 * from Mesa, because only such a buffer can be handed to a dispatch; without it the
	 * kernel's GEM interface is used directly, so that a device with no GPU library still
	 * gives memory a program can write. Which one happened is visible in -mesaBuffer. */
	if (mesa_available() && [device mesaDevice] != NULL) {
		char why[256] = { 0 };

		size_t size = length;

		_mesa = hk_pool_take([device mesaDevice], length);
		if (_mesa != NULL) {
			memset(mesa.buffer_map(_mesa), 0, length);
		} else {
			(void)hk_pool_class(length, &size);
			_mesa = mesa.buffer_create([device mesaDevice], size, why, sizeof(why));
		}
		if (_mesa == NULL) {
			rw_print("Mesa would not allocate %lu bytes: %s\n",
			         (unsigned long)length, why);
			[self release];
			return nil;
		}
		atomic_fetch_add(&hk_live.buffers, 1);
		atomic_fetch_add(&hk_live.buffer_bytes, (int64_t)length);
		_owner = device;
		_length = length;
		_contents = mesa.buffer_map(_mesa);
		return self;
	}
	if (gpu == IO_OBJECT_NULL || length == 0) {
		rw_print("a buffer of %lu bytes needs a gpu client\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	if (IOConnectCallStructMethod(gpu, kMsmGemNew, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS || request.handle == 0) {
		rw_print("the kernel would not allocate %lu bytes\n",
		         (unsigned long)length);
		[self release];
		return nil;
	}
	_owner = device;
	_gpu = gpu;
	_handle = request.handle;
	_length = length;

	/* the address the GPU will see, which is what a command stream carries */
	struct gem_info info = { .handle = _handle, .info = MSM_INFO_GET_IOVA };

	size = sizeof(info);
	if (IOConnectCallStructMethod(gpu, kMsmGemInfo, &info, sizeof(info), &info,
				      &size) == KERN_SUCCESS) {
		_iova = info.value;
	}

	/* and the address this processor will see: the mapping is as the memory is made,
	 * write-combined, exactly as Mesa takes it */
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	if (IOConnectMapMemory64(gpu, _handle, mach_task_self(), &address, &mapped,
				 kIOMapAnywhere) == KERN_SUCCESS) {
		_contents = (void *)(uintptr_t)address;
	} else {
		rw_print("buffer %u is allocated but could not be mapped\n", _handle);
	}
	return self;
}

- (struct rwa_buffer *)mesaBuffer
{
	return _mesa;
}

- (void)dealloc
{
	if (_wrapped != NULL) {
		if (_wrappedGone != nil) {
			_wrappedGone(_wrapped, _length);
			[_wrappedGone release];
		}
		atomic_fetch_sub(&hk_draw_backs, 1);
	}
	if (_mesa != NULL) {
		atomic_fetch_sub(&hk_live.buffers, 1);
		atomic_fetch_sub(&hk_live.buffer_bytes, (int64_t)_length);
		if (_wrapped != NULL || _neverKept || !hk_pool_put([_owner mesaDevice], _mesa, _length)) {
			mesa.buffer_destroy(_mesa);
		}
		[super dealloc];
		return;
	}
	if (_contents != NULL) {
		IOConnectUnmapMemory64(_gpu, _handle, mach_task_self(),
				       (mach_vm_address_t)(uintptr_t)_contents);
	}
	if (_handle != 0) {
		struct gem_close request = { .handle = _handle, .pad = 0 };
		size_t size = sizeof(request);

		IOConnectCallStructMethod(_gpu, kDrmGemClose, &request, sizeof(request), &request,
					  &size);
	}
	[super dealloc];
}

- (void *)contents          { return _contents; }
- (NSUInteger)length        { return _length; }
- (id)device                { return _owner; }
- (uint64_t)gpuAddress      { return _iova; }
- (NSUInteger)allocatedSize { return _length; }

/* The memory is write-combined and the GPU reads it through no cache of ours, so there is
 * nothing to push out; saying so is not the same as doing nothing by accident. */
- (void)didModifyRange:(NSRange)range    { (void)range; }

/* What was asked for does not change what was given: every allocation here is one kind of
 * memory, and the honest answer is the kind it is. */
- (NSUInteger)storageMode        { return 0; }   /* MTLStorageModeShared */
- (NSUInteger)cpuCacheMode       { return 1; }   /* MTLCPUCacheModeWriteCombined */
- (NSUInteger)resourceOptions    { return 0; }
- (NSUInteger)hazardTrackingMode { return 0; }   /* MTLHazardTrackingModeDefault */

/* MTLPurgeableStateNonVolatile (2), whatever was asked for, and that is the answer and not
 * a refusal: this memory is never volatile -- the kernel holds it from kMsmGemNew to
 * kDrmGemClose -- so there is no state for a caller to change and none to report but this
 * one. (Zero, which this used to answer, is not an MTLPurgeableState at all, and Metal has
 * a path that then believes the resource was purged: see -addPurgedResource: on the command
 * buffer.) */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }

/*
 * What <MTLResourceSPI> and <MTLBuffer> require and nothing in this chain answered: ten
 * methods, every one of them an abort in whatever daemon asked first. They are answers, and
 * most of them are true rather than evasive -- which is the point of counting what a
 * protocol demands instead of what our own tests happen to send.
 *
 * Aliasing: nothing here aliases anything. Every buffer is its own GEM allocation (or its
 * own Mesa allocation), a heap's included (HKMetalHeap), and -makeAliasable only gives its
 * size back to the heap's account, so "does this overlap that" is NO for every pair, for certain.
 *
 * Completion: this driver's -commit runs the recorded work and -waitUntilCompleted fences
 * it, and nothing hands a resource back to a caller before the submission it was part of
 * has been made. There is no per-resource completion to wait for, so YES and a return are
 * the honest pair -- and if a second ring ever appears, these two are where it has to be
 * answered again.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isComplete        { return YES; }
- (void)waitUntilComplete { }
- (BOOL)isPurgeable       { return NO; }
/* Protected (DRM) memory is a kernel capability this port does not have; zero is "none of
 * the protection options", which is what this memory has. */
- (NSUInteger)protectionOptions { return 0; }
/* The options before the device filtered them: the device filters nothing, so this is the
 * same answer -resourceOptions gives. */
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Whose allocation this is for the kernel's accounting. We do not tell the kernel, so the
 * only true answer is this process, and a setter that remembers what it was told. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }
/*
 * A texture laid over this buffer's memory (M5g, the owner: 「地圖在widget顯示黃色」). RenderBox lays each large image over a buffer of its own, and without a texture draws one
 * yellow pixel instead (rwa_texture_over_buffer, air_vk.c). Where the library cannot, or the rows are not what this GPU reads, a copy of what the buffer holds now.
 */
- (id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
		   bytesPerRow:(NSUInteger)bytesPerRow
{
	char why[320] = "the compiler library has no call for it";
	struct rwa_texture_shape shape = {
		.pixel_format = (unsigned)[descriptor pixelFormat],
		.type = (unsigned)[descriptor textureType],
		.width = (unsigned)[descriptor width],
		.height = (unsigned)[descriptor height],
		.depth = (unsigned)[descriptor depth],
		.array_length = (unsigned)[descriptor arrayLength],
		.mip_levels = (unsigned)[descriptor mipmapLevelCount],
		.sample_count = (unsigned)[descriptor sampleCount],
		.usage = (unsigned)[descriptor usage],
	};
	struct rwa_texture *texture = mesa.texture_over_buffer != NULL && _mesa != NULL
	    ? mesa.texture_over_buffer(_mesa, &shape, offset, (unsigned)bytesPerRow, why, sizeof(why)) : NULL;

	if (texture != NULL) {
		HKMetalTexture *over = [[HKMetalTexture alloc] initWithHKDevice:_owner
									texture:texture
									  shape:&shape];

		[over hkOverBuffer:self row:bytesPerRow];
		return hk_with_descriptor_swizzle(over, descriptor);
	}
	RW_ONCE("a texture over a buffer, as a copy: %s\n", why);

	id copied = [_owner newTextureWithDescriptor:descriptor];

	[copied replaceRegion:MTLRegionMake2D(0, 0, shape.width, shape.height) mipmapLevel:0 withBytes:(const char *)[self contents] + offset bytesPerRow:bytesPerRow];
	return copied;
}

- (id)newLinearTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
			 bytesPerRow:(NSUInteger)bytesPerRow
		       bytesPerImage:(NSUInteger)bytesPerImage
{
	(void)bytesPerImage;
	return [self newTextureWithDescriptor:descriptor offset:offset bytesPerRow:bytesPerRow];
}
- (void)addDebugMarker:(NSString *)marker range:(NSRange)range { (void)marker; (void)range; }
- (void)removeAllDebugMarkers { }

@end

/* M5e: how often a command buffer was committed, which is as often as a copied surface is looked at (hkFollowSurface) */
static _Atomic(uint64_t) hk_commits;

@implementation HKMetalTexture

+ (void)load
{
	/* MTLTexture is what an application sees. MTLTextureImplementation is what Metal
	 * itself checks: -[MTLRenderPassColorAttachmentDescriptorInternal setTexture:]
	 * (0x181a350a0) asks conformsToProtocol: for it and reports "texture is not a
	 * MTLTextureImplementation." Only the conformance is tested on that path -- nothing
	 * sends its 68 members -- so declaring it is honest and is what lets a texture of ours
	 * be a render target at all. */
	const char *names[] = { "MTLTexture", "MTLTextureImplementation" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *p = objc_getProtocol(names[i]);

		if (p == NULL) {
			continue;
		}
		if (!class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
			rw_print("class_addProtocol(%s) refused\n", names[i]);
		}
	}
}

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			      texture:(struct rwa_texture *)texture
				shape:(const struct rwa_texture_shape *)shape
{
	self = [super init];
	if (self == nil) {
		mesa.texture_destroy(texture);
		return nil;
	}
	_owner = device;
	_texture = texture;
	_shape = *shape;
	_liveBytes = (int64_t)shape->width * MAX(shape->height, 1) * MAX(shape->depth, 1) * MAX(shape->array_length, 1) * MAX(shape->sample_count, 1) * 4;
	atomic_fetch_add(&hk_live.textures, 1);
	atomic_fetch_add(&hk_live.texture_bytes, _liveBytes);
	_swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
						 MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
	return self;
}

/*
 * A copy of a surface does not stay right by itself. Whoever draws into the surface draws on, and often no
 * seed moves (eagl.c's surface_changed found that in hk-gl). Safari's page was copied while it was still
 * empty and stayed black, run 152737. So once per command buffer committed, when the texture is about to be
 * drawn with, the surface's memory is added up and copied again if the sum has changed. A frame caught
 * half drawn is put right by the next look. Each power of ten of copies says what they cost.
 */
- (void)hkFollow:(void *)surface
{
	_followed = (void *)CFRetain(surface);
	[self hkFollowSurface];
}

- (void)hkBiplanarOf:(void *)surface seed:(uint32_t)seed sum:(uint64_t)sum
{
	_biplanar = (void *)CFRetain(surface);
	_biplanarSeed = seed;
	_biplanarSum = sum;
}

/*
 * Each time format 500's copy is about to be drawn with: made again if its surface's seed has moved. And with
 * metal_stats, which of them came up in what order and whether what they show is what their surfaces hold: the
 * test of a ring of pictures. Numbers only.
 */
- (void)hkBiplanarLook
{
	static int once = -1;
	static HKMetalTexture *known[16];
	static unsigned char order[64];
	static unsigned drawn, moved, copied;
	static uint32_t (*seed_of)(void *);
	static int (*lock)(void *, uint32_t, uint32_t *);
	static int (*unlock)(void *, uint32_t, uint32_t *);
	static void *(*base)(void *, size_t);
	static size_t (*row)(void *, size_t);
	const uint64_t now = atomic_load(&hk_commits) + 1;

	if (_followedLooked == now) {
		return;
	}
	_followedLooked = now;
	if (once < 0) {
		*(void **)&seed_of = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");
		*(void **)&lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
		*(void **)&unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
		*(void **)&base = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddressOfPlane");
		*(void **)&row = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRowOfPlane");
		once = access("/hackios/share/probes/biplanar_once", F_OK) == 0;
	}
	if (seed_of == NULL || lock == NULL || unlock == NULL || base == NULL || row == NULL) {
		return;
	}
	const uint32_t seed = seed_of(_biplanar);
	const NSUInteger w = _shape.width, h = _shape.height;
	const bool changed = seed != _biplanarSeed;

	moved += changed;
	if (changed && !once && lock(_biplanar, 1, NULL) == 0) {
		const uint64_t began = hk_now_us();
		const uint8_t *y = base(_biplanar, 0), *uv = base(_biplanar, 1);
		const size_t ys = row(_biplanar, 0), uvs = row(_biplanar, 1);
		uint32_t (*pixel_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
		const bool video = pixel_format != NULL && (pixel_format(_biplanar) & 0xff) == 'v';

		if (_biplanarBytes == NULL) {
			_biplanarBytes = malloc(w * h * 4);
		}
		if (_biplanarBytes != NULL) {
			const NSUInteger ch = (h + 1) / 2;

			for (NSUInteger j = 0; j < h; j++) {
				const NSUInteger c0 = j / 2;
				const NSUInteger c1 = (j & 1) ? MIN(c0 + 1, ch - 1) : (c0 > 0 ? c0 - 1 : 0);

				hk_420_line(y + j * ys, uv + c0 * uvs, uv + c1 * uvs, w, video, _biplanarBytes + j * w * 4);
			}
			[self replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:_biplanarBytes bytesPerRow:w * 4];
			_biplanarSeed = seed;
			copied++;
			if (hk_stats_on()) {
				_biplanarSum = hk_420_sum(y, ys, w, h);
			}
		}
		unlock(_biplanar, 1, NULL);
		atomic_fetch_add(&hk_stat.biplanar, 1);
		atomic_fetch_add(&hk_stat.biplanar_us, hk_now_us() - began);
	}
	if (!hk_stats_on()) {
		return;
	}
	unsigned index = 0;

	for (; index < 16 && known[index] != self; index++) {
		if (known[index] == nil) {
			known[index] = [self retain];   /* (only with metal_stats) */
			break;
		}
	}
	order[drawn % 64] = index < 16 ? (unsigned char)index : 16;
	if (++drawn % 64 == 0) {
		char line[1000], *at = line, *end = line + sizeof(line);
		unsigned stale = 0, textures = 0;

		at += snprintf(at, (size_t)(end - at), "hk-420: %u looks, last in order ", drawn);
		for (unsigned i = 0; i < 64 && at < end - 2; i++) {
			*at++ = "0123456789abcdefg"[order[i]];
		}
		*at = 0;
		at += snprintf(at, (size_t)(end - at), "; seeds moved %u, copied %u;", moved, copied);
		for (unsigned i = 0; i < 16 && known[i] != nil && at < end - 60; i++) {
			HKMetalTexture *t = known[i];
			uint64_t current = 0;

			if (lock(t->_biplanar, 1, NULL) == 0) {
				current = hk_420_sum(base(t->_biplanar, 0), row(t->_biplanar, 0), t->_shape.width, t->_shape.height);
				unlock(t->_biplanar, 1, NULL);
			}
			stale += current != t->_biplanarSum;
			textures++;
			at += snprintf(at, (size_t)(end - at), " %u:%04x/%04x", i, (unsigned)(t->_biplanarSum & 0xffff), (unsigned)(current & 0xffff));
		}
		at += snprintf(at, (size_t)(end - at), "; %u of %u show other than their surfaces hold\n", stale, textures);
		rw_print("%s", line);
	}
}

- (void)hkStandInFor:(void *)surface
{
	@synchronized([HKMetalTexture class]) {
		for (unsigned i = 0; i < 64; i++) {
			if (hk_stand_ins[i].surface == NULL) {
				hk_stand_ins[i].surface = surface;
				hk_stand_ins[i].texture = self;
				break;
			}
		}
	}
}

/* (a copy that has followed the surface until now is asked to be drawn into: from now on what it holds is the surface's picture) */
- (void)hkStartDrawingBack
{
	if (!_drawsBack) {
		_followedInPlace = YES;
		_drawsBack = YES;
		atomic_fetch_add(&hk_draw_backs, 1);
	}
}

- (void)hkStandInForPlane:(NSUInteger)plane of:(void *)surface
{
	int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
	struct rwd_plane where;

	_followed = (void *)CFRetain(surface);
	_followedInPlace = YES;
	_planeStandIn = YES;
	_plane = plane;
	atomic_fetch_add(&hk_draw_backs, 1);
	if (rwd_plane_of(surface, plane, &where) == 0 && lock(surface, 1 /* kIOSurfaceLockReadOnly */, NULL) == 0) {
		[self replaceRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0 withBytes:where.base bytesPerRow:where.row];
		unlock(surface, 1, NULL);
	}
}

- (BOOL)hkWritesPlaneBack { return _planeStandIn && (_shape.usage & MTLTextureUsageShaderWrite) != 0; }

- (id)hkOverBuffer { return _parent != nil ? [(HKMetalTexture *)_parent hkOverBuffer] : _overBuffer; }

- (void)hkDrawBackTo:(void *)surface
{
	size_t (*surface_row)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	void *(*base_address)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
	int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");

	_followed = (void *)CFRetain(surface);
	_followedInPlace = YES;
	_drawsBack = YES;
	atomic_fetch_add(&hk_draw_backs, 1);
	[self hkStandInFor:surface];
	uint32_t (*seed_of)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");

	if (lock(surface, 1 /* kIOSurfaceLockReadOnly */, NULL) == 0) {
		[self replaceRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0 withBytes:base_address(surface) bytesPerRow:surface_row(surface)];
		unlock(surface, 1, NULL);
		_followedSeed = seed_of != NULL ? seed_of(surface) : 0;
	}
}

- (BOOL)hkDrawsBack { return _drawsBack; }

/* (a second after the last draw: a whole surface a frame took 45 ms a frame, run 003149, and one every second held the GPU's queue for 180 ms, run 003605) */
- (void)hkPutBackLater
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		if (hk_now_us() - atomic_load(&_lastDrawn) < 900000) {
			[self hkPutBackLater];
			return;
		}
		__atomic_store_n(&_putBackDue, NO, __ATOMIC_RELEASE);
		[self hkPutBack];
		[self release];
	});
}

- (void)hkPutBackSoon
{
	const uint64_t now = hk_now_us(), before = atomic_exchange(&_lastDrawn, now);

	/* (the first draw after a rest goes back at once, before the work is said to be done: a snapshot, whose maker reads it then. App Library held still, black without this, run 011117) */
	if (now - before > 1000000) {
		[self hkPutBack];
		return;
	}
	if (__atomic_exchange_n(&_putBackDue, YES, __ATOMIC_ACQ_REL)) {
		return;
	}
	[self retain];
	[self hkPutBackLater];
}

- (void)hkPutBack
{
	size_t (*surface_row)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	void *(*base_address)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
	int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");

	uint32_t (*seed_of)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");

	if (_planeStandIn) {
		struct rwd_plane where;

		if (rwd_plane_of(_followed, _plane, &where) == 0 && lock(_followed, 0, NULL) == 0) {
			[self getBytes:where.base bytesPerRow:where.row fromRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0];
			unlock(_followed, 0, NULL);
		}
		return;
	}
	if (lock(_followed, 0, NULL) == 0) {
		[self getBytes:base_address(_followed) bytesPerRow:surface_row(_followed) fromRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0];
		unlock(_followed, 0, NULL);
		_followedSeed = seed_of != NULL ? seed_of(_followed) : 0;
	}
}

- (void)hkOverBuffer:(id)buffer row:(NSUInteger)row
{
	_overBuffer = [buffer retain];
	_rowGiven = row;
}

- (size_t)hkCensus:(char *)out size:(size_t)size centre:(uint8_t *)centre
{
	uint32_t (*surface_format)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	HKMetalTexture *root = _parent != nil ? (HKMetalTexture *)_parent : self;
	size_t (*surface_row)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	const uint32_t fourcc = root->_followed != NULL && surface_format != NULL ? surface_format(root->_followed) : 0;
	const size_t row = root->_followed != NULL && surface_row != NULL ? surface_row(root->_followed) : root->_rowGiven;
	const unsigned format = _shape.pixel_format;

	memset(centre, 0, 16);
	if (format < 130 && _shape.sample_count <= 1 && _shape.type == 2) {
		[self getBytes:centre bytesPerRow:16 fromRegion:MTLRegionMake2D(MAX(_shape.width, 1) / 2, MAX(_shape.height, 1) / 2, 1, 1) mipmapLevel:0];
	}
	/* (and over the whole of it, in numbers, for what a centre cannot say: K33, a gain map reads 0 0 0 0 there) */
	unsigned top[4] = { 0 }, nonzero = 0;

	if (format == 70 && _shape.width >= 256 && _shape.height >= 256 && _shape.sample_count <= 1 && _shape.type == 2) {
		for (unsigned j = 0; j < 16; j++) {
			for (unsigned i = 0; i < 16; i++) {
				uint8_t p[4];

				[self getBytes:p bytesPerRow:16 fromRegion:MTLRegionMake2D((_shape.width - 1) * i / 15, (_shape.height - 1) * j / 15, 1, 1) mipmapLevel:0];
				for (unsigned k = 0; k < 4; k++) {
					top[k] = p[k] > top[k] ? p[k] : top[k];
				}
				nonzero += (p[0] | p[1] | p[2] | p[3]) != 0;
			}
		}
	}
	return (size_t)snprintf(out, size, " [%s%s %ux%u format %u (of %u) swizzle %u%u%u%u row %zu fourcc %08x '%.4s' centre %u %u %u %u; of 256 points %u not zero, highest %u %u %u %u]", _parent != nil ? "view of " : "",
				root->_overBuffer != nil ? "buffer" : root->_followedInPlace ? "surface" : root->_followed != NULL ? "copied surface" : "descriptor", _shape.width, _shape.height, format, root->_shape.pixel_format,
				(unsigned)_swizzle.red, (unsigned)_swizzle.green, (unsigned)_swizzle.blue, (unsigned)_swizzle.alpha, row, fourcc, (const char *)&fourcc,
				centre[0], centre[1], centre[2], centre[3], nonzero, top[0], top[1], top[2], top[3]);
}

- (void)hkRememberSurface:(void *)surface
{
	_followed = (void *)CFRetain(surface);
	_followedInPlace = YES;
	if (!_liveSurface) {
		_liveSurface = YES;
		atomic_fetch_add(&hk_live.surfaces, 1);
		atomic_fetch_add(&hk_live.surface_bytes, _liveBytes);
	}
}

/* Which surface, how it reaches the GPU, and a look at 25 of its pixels from this processor. */
- (size_t)hkDescribeSurface:(char *)out size:(size_t)size
{
	if (_parent != nil) {
		return [(HKMetalTexture *)_parent hkDescribeSurface:out size:size];
	}
	if (_followed == NULL) {
		return (size_t)snprintf(out, size, " own");
	}
	uint32_t (*surface_id)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetID");
	uint32_t (*seed)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");
	size_t (*bytes_per_row)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	void *(*base_address)(void *) = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
	int (*lock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	int (*unlock)(void *, uint32_t, uint32_t *) = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
	unsigned nonzero = 0;
	uint32_t last = 0;

	if (lock(_followed, 1, NULL) == 0) {
		const uint8_t *base = base_address(_followed);
		const size_t row = bytes_per_row(_followed);

		for (unsigned y = 1; base != NULL && y <= 5; y++) {
			for (unsigned x = 1; x <= 5; x++) {
				uint32_t pixel;

				memcpy(&pixel, base + (size_t)(_shape.height * y / 6) * row + (size_t)(_shape.width * x / 6) * 4, 4);
				nonzero += pixel != 0;
				last = pixel != 0 ? pixel : last;
			}
		}
		unlock(_followed, 1, NULL);
	}
	return (size_t)snprintf(out, size, " %s#%u seed%u nz%u/%08x", _followedInPlace ? "place" : "copy",
				surface_id(_followed), seed(_followed), nonzero, last);
}

- (void)hkFollowSurface
{
	static _Atomic(uint64_t) copies, spent_us;
	const uint64_t now = atomic_load(&hk_commits) + 1;

	if (_parent != nil) {
		[(HKMetalTexture *)_parent hkFollowSurface];
		return;
	}
	if (_biplanar != NULL) {
		[self hkBiplanarLook];
		return;
	}
	/* (DRAWN AND PUT BACK: a stand-in goes on following what others write into the surface: its seed moves, and not by hkPutBack) */
	if (_followed == NULL || (_followedInPlace && !_drawsBack) || _followedLooked == now) {
		return;
	}
	_followedLooked = now;

	static size_t (*bytes_per_row)(void *);
	static size_t (*bytes_per_element)(void *);
	static void *(*base_address)(void *);
	static int (*lock)(void *, uint32_t, uint32_t *);
	static int (*unlock)(void *, uint32_t, uint32_t *);
	static uint32_t (*seed_of)(void *);

	if (unlock == NULL) {
		*(void **)&seed_of = dlsym(RTLD_DEFAULT, "IOSurfaceGetSeed");
		*(void **)&bytes_per_row = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
		*(void **)&bytes_per_element = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerElement");
		*(void **)&base_address = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
		*(void **)&lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock");
		*(void **)&unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
	}
	const uint64_t look_began = hk_now_us();
	const uint32_t seed = seed_of != NULL ? seed_of(_followed) : 0;

	/*
	 * (M5g) Who writes a surface says so by locking it, which moves its seed. Adding up all of it at every commit was 650 MB
	 * in 5 s in Maps, run 211412, for surfaces that had not changed, 5% of a core on the thread that draws. The sum stays, a
	 * few times a second, for a writer that does not say.
	 */
	if (_followedSum != 0 && seed_of != NULL && seed == _followedSeed && look_began - _followedSummedAt < 250000) {
		return;
	}
	if (_drawsBack && seed == _followedSeed) {
		return;
	}
	_followedSeed = seed;
	_followedSummedAt = look_began;

	if (bytes_per_row == NULL || bytes_per_element == NULL || base_address == NULL || lock == NULL ||
	    unlock == NULL || lock(_followed, 1, NULL) != 0) {
		return;
	}
	const size_t row = bytes_per_row(_followed);
	const size_t size = row * (_shape.height - 1) + _shape.width * bytes_per_element(_followed);
	const uint8_t *base = base_address(_followed);
	uint64_t sum = size, word;

	for (size_t at = 0; base != NULL && at + 8 <= size; at += 8) {
		memcpy(&word, base + at, 8);
		sum = (sum ^ word) * 0x9e3779b97f4a7c15ull;
		sum ^= sum >> 29;
	}
	if (base != NULL && (sum != _followedSum || _followedSum == 0)) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		_followedSum = sum;
		[self replaceRegion:MTLRegionMake2D(0, 0, _shape.width, _shape.height) mipmapLevel:0
			  withBytes:base bytesPerRow:row];
		clock_gettime(CLOCK_MONOTONIC, &b);

		const uint64_t us = atomic_fetch_add(&spent_us, (uint64_t)((b.tv_sec - a.tv_sec) * 1000000 +
								 (b.tv_nsec - a.tv_nsec) / 1000));
		const uint64_t n = atomic_fetch_add(&copies, 1) + 1;

		if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000) {
			rw_print("a surface's copy made again %llu times so far, this one %ux%u; %llu ms in all\n",
				 (unsigned long long)n, _shape.width, _shape.height,
				 (unsigned long long)(us / 1000));
		}
	}
	unlock(_followed, 1, NULL);
	atomic_fetch_add(&hk_stat.looks, 1);
	atomic_fetch_add(&hk_stat.look_bytes, size);
	atomic_fetch_add(&hk_stat.look_us, hk_now_us() - look_began);
}

- (void)dealloc
{
	atomic_fetch_sub(&hk_live.textures, 1);
	atomic_fetch_sub(&hk_live.texture_bytes, _liveBytes);
	if (_liveSurface) {
		atomic_fetch_sub(&hk_live.surfaces, 1);
		atomic_fetch_sub(&hk_live.surface_bytes, _liveBytes);
	}
	if (_texture != NULL) {
		mesa.texture_destroy(_texture);
	}
	[_parent release];
	[_overBuffer release];
	if (_followed != NULL) {
		CFRelease(_followed);
	}
	if (_biplanar != NULL) {
		CFRelease(_biplanar);
		free(_biplanarBytes);
	}
	if (_drawsBack || _planeStandIn) {
		atomic_fetch_sub(&hk_draw_backs, 1);
	}
	if (_followed != NULL) {
		@synchronized([HKMetalTexture class]) {
			for (unsigned i = 0; i < 64; i++) {
				if (hk_stand_ins[i].texture == self) {
					hk_stand_ins[i].surface = NULL;
					hk_stand_ins[i].texture = nil;
				}
			}
		}
	}
	[super dealloc];
}

- (struct rwa_texture *)mesaTexture { return _texture; }

- (NSUInteger)hkSlices
{
	NSUInteger cube = (_shape.type == MTLTextureTypeCube ||
			   _shape.type == MTLTextureTypeCubeArray) ? 6 : 1;

	return MAX(_shape.array_length, 1) * cube;
}

- (id)hkViewFormat:(NSUInteger)format
	      type:(NSUInteger)type
	    levels:(NSRange)levels
	    slices:(NSRange)slices
	   swizzle:(MTLTextureSwizzleChannels)swizzle
{
	char why[320] = { 0 };

	/* (said on both back ends, before the library is asked: what it asked for is the point) */
	hk_note_packed16("view", (unsigned)format, _shape.usage, _swizzle, &swizzle, NULL);
	if (mesa.texture_view_create == NULL) {
		RW_ONCE("a texture view is not implemented on this compiler library (the Gallium "
			"one has no call that makes a second texture over the same memory); nil\n");
		return nil;
	}
	const struct rwa_texture_view_shape vs = {
		.pixel_format = (unsigned)format,
		.type = (unsigned)type,
		.base_level = (unsigned)levels.location,
		.level_count = (unsigned)levels.length,
		.base_slice = (unsigned)slices.location,
		.slice_count = (unsigned)slices.length,
		.swizzle = { swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha },
	};
	struct rwa_texture *view = mesa.texture_view_create(_texture, &vs, why, sizeof(why));

	if (view == NULL) {
		rw_print("a texture view: %s\n", why);
		return nil;
	}
	struct rwa_texture_shape shape = _shape;

	shape.pixel_format = (unsigned)format;
	shape.type = (unsigned)type;
	shape.width = MAX(_shape.width >> levels.location, 1);
	shape.height = MAX(_shape.height >> levels.location, 1);
	shape.depth = type == MTLTextureType3D ? MAX(_shape.depth >> levels.location, 1) : 1;
	shape.mip_levels = (unsigned)levels.length;
	shape.array_length = (unsigned)((type == MTLTextureTypeCube ||
					 type == MTLTextureTypeCubeArray)
					    ? slices.length / 6 : slices.length);
	HKMetalTexture *made = [[HKMetalTexture alloc] initWithHKDevice:_owner
								texture:view
								  shape:&shape];
	if (made != nil) {
		made->_parent = [self retain];
		atomic_fetch_sub(&hk_live.texture_bytes, made->_liveBytes);   /* (a view: its bytes are its parent's) */
		made->_liveBytes = 0;
		made->_parentLevel = levels.location;
		made->_parentSlice = slices.location;
		made->_swizzle = swizzle;
	}
	return made;
}

- (id)hkWindowLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	/* one texel format, and the type of one slice of this one; of a 3D texture, of one plane */
	NSUInteger type = _shape.type;

	if (type == MTLTextureType1DArray) {
		type = MTLTextureType1D;
	} else if (type == MTLTextureType2DArray || type == MTLTextureTypeCube ||
		   type == MTLTextureTypeCubeArray || type == MTLTextureType3D) {
		type = MTLTextureType2D;
	} else if (type == MTLTextureType2DMultisampleArray) {
		type = MTLTextureType2DMultisample;
	}
	id view = [self hkViewFormat:_shape.pixel_format
				type:type
			      levels:NSMakeRange(level, 1)
			      slices:NSMakeRange(slice, 1)
			     swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,
								   MTLTextureSwizzleGreen,
								   MTLTextureSwizzleBlue,
								   MTLTextureSwizzleAlpha)];
	return [view autorelease];
}

- (id)device                       { return _owner; }
- (NSUInteger)width                { return _shape.width; }
- (NSUInteger)height               { return _shape.height; }
- (NSUInteger)depth                { return _shape.depth; }
- (NSUInteger)mipmapLevelCount     { return _shape.mip_levels; }
- (NSUInteger)arrayLength          { return _shape.array_length; }
- (NSUInteger)sampleCount          { return _shape.sample_count; }
- (NSUInteger)pixelFormat          { return _shape.pixel_format; }
- (NSUInteger)textureType          { return _shape.type; }
- (NSUInteger)usage                { return _shape.usage; }
- (BOOL)isFramebufferOnly          { return NO; }
- (BOOL)allowGPUOptimizedContents  { return NO; }
/* [M5b: a view has one, retained, with where it is in it; a texture that was made has none.] */
- (id)parentTexture                { return _parent; }
- (NSUInteger)parentRelativeLevel  { return _parentLevel; }
- (NSUInteger)parentRelativeSlice  { return _parentSlice; }
- (id)buffer                       { return nil; }
- (NSUInteger)bufferOffset         { return 0; }
- (NSUInteger)bufferBytesPerRow    { return 0; }
- (id)iosurface                    { return nil; }
- (NSUInteger)iosurfacePlane       { return 0; }
/* MTLStorageModeShared, except for a multisample texture, which Metal does not allow to be
 * one: "MTLTextureDescriptor: Multisample textures cannot be allocated with
 * MTLStorageModeShared on this device" is Metal's own sentence, in the framework's own
 * __cstring and not in the validation layer. Answering Private for one is not bookkeeping
 * -- Shared is what makes -replaceRegion: and -getBytes: legal, and those are exactly the
 * two calls the compiler library refuses on a multisample texture (and refuses because
 * neither side can give the samples of a texel a defined order through a map). So the
 * storage mode this reports and the calls that work on it say the same thing. */
- (NSUInteger)storageMode
{
	return _shape.sample_count > 1 ? 2 : 0;   /* MTLStorageModePrivate : Shared */
}
- (NSUInteger)cpuCacheMode         { return 1; }
- (NSUInteger)resourceOptions      { return 0; }
- (NSUInteger)hazardTrackingMode   { return 0; }
- (NSUInteger)allocatedSize        { return 0; }   /* not accounted for yet, and says so */
/* MTLPurgeableStateNonVolatile, for the reason written over the buffer's own: a texture of
 * the compiler library's is held until it is destroyed, so there is no volatile state to
 * report and zero was not a state at all. */
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }

/*
 * The one recording the compiler library has, and who may touch it. -commit took this and nothing else
 * did: "uploads and reads (-replaceRegion:, -getBytes:) from a second thread still share the context
 * unguarded -- take it there too if one shows up", said this comment. On the Vulkan library one does
 * (M5d): a texel call records into the same command buffer a -commit on another thread is
 * recording, which Vulkan calls externally synchronised and air_vk.c's record_begin names as the road
 * left open. A window server's two threads are that road. So the texel calls take it as well,
 * for the whole call: a read waits for the GPU inside it, and a -commit elsewhere waits behind that.
 * It is not recursive, and nothing in here calls one of these with it held: -hkRun calls the library.
 */
static pthread_mutex_t rw_pipe_lock = PTHREAD_MUTEX_INITIALIZER;

static int
rw_texture_write(struct rwa_texture *t, unsigned level, unsigned slice, const unsigned origin[3],
		 const unsigned size[3], const void *bytes, size_t stride, char *why, size_t why_size)
{
	pthread_mutex_lock(&rw_pipe_lock);
	int rc = mesa.texture_write(t, level, slice, origin, size, bytes, stride, why, why_size);
	pthread_mutex_unlock(&rw_pipe_lock);
	return rc;
}

static int
rw_texture_read(struct rwa_texture *t, unsigned level, unsigned slice, const unsigned origin[3],
		const unsigned size[3], void *bytes, size_t stride, char *why, size_t why_size)
{
	pthread_mutex_lock(&rw_pipe_lock);
	int rc = mesa.texture_read(t, level, slice, origin, size, bytes, stride, why, why_size);
	pthread_mutex_unlock(&rw_pipe_lock);
	return rc;
}

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
		slice:(NSUInteger)slice
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
	bytesPerImage:(NSUInteger)image
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	_rowGiven = stride;
	if ([self pixelFormat] == 115 && size[0] >= 64 && bytes != NULL) {
		static unsigned said;

		if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 6) {
			const __fp16 *h = bytes;
			unsigned last = size[0] - 1, mid = size[0] / 2;

			rw_print("RGBA16F upload %ux%u stride %lu: [0] %.3f %.3f %.3f %.3f [%u] %.3f %.3f %.3f %.3f [%u] %.3f %.3f %.3f %.3f\n",
				 size[0], size[1], (unsigned long)stride, (double)h[0], (double)h[1], (double)h[2], (double)h[3],
				 mid, (double)h[mid * 4], (double)h[mid * 4 + 1], (double)h[mid * 4 + 2], (double)h[mid * 4 + 3],
				 last, (double)h[last * 4], (double)h[last * 4 + 1], (double)h[last * 4 + 2], (double)h[last * 4 + 3]);
		}
	}
	/* [M5b: "(void)image;" stood here, which is the packed case: the contract has one stride
	 * and a 3D region's images follow each other at stride * height. An application's other
	 * bytesPerImage is said one plane at a time, where it means the same thing.] */
	if (region.size.depth > 1 && image != 0 && image != stride * region.size.height) {
		const uint8_t *from = bytes;

		for (NSUInteger z = 0; z < region.size.depth; z++, from += image) {
			origin[2] = (unsigned)(region.origin.z + z);
			size[2] = 1;
			if (rw_texture_write(_texture, (unsigned)level, (unsigned)slice, origin, size,
					       from, stride, why, sizeof(why)) != 0) {
				rw_print("writing texels: %s\n", why);
				return;
			}
		}
		return;
	}
	if (rw_texture_write(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			       stride, why, sizeof(why)) != 0) {
		rw_print("writing texels: %s\n", why);
	}
}

- (void)replaceRegion:(MTLRegion)region
	  mipmapLevel:(NSUInteger)level
	    withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)stride
{
	[self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:stride
	      bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
   bytesPerImage:(NSUInteger)image
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
	   slice:(NSUInteger)slice
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	/* [M5b: as -replaceRegion: above] */
	if (region.size.depth > 1 && image != 0 && image != stride * region.size.height) {
		uint8_t *to = bytes;

		for (NSUInteger z = 0; z < region.size.depth; z++, to += image) {
			origin[2] = (unsigned)(region.origin.z + z);
			size[2] = 1;
			if (rw_texture_read(_texture, (unsigned)level, (unsigned)slice, origin, size,
					      to, stride, why, sizeof(why)) != 0) {
				rw_print("reading texels: %s\n", why);
				return;
			}
		}
		return;
	}
	if (rw_texture_read(_texture, (unsigned)level, (unsigned)slice, origin, size, bytes,
			      stride, why, sizeof(why)) != 0) {
		rw_print("reading texels: %s\n", why);
	}
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)stride
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:stride bytesPerImage:0 fromRegion:region
	   mipmapLevel:level slice:0];
}

- (BOOL)hkReadDepth:(float *)depth
		 stencil:(uint8_t *)stencil
	      fromRegion:(MTLRegion)region
{
	char why[256] = { 0 };
	unsigned origin[3] = { (unsigned)region.origin.x, (unsigned)region.origin.y,
			       (unsigned)region.origin.z };
	unsigned size[3] = { (unsigned)region.size.width, (unsigned)region.size.height,
			     (unsigned)region.size.depth };

	if (mesa.depth_read == NULL) {
		rw_print("this compiler library has no rwa_depth_read, so it is "
		         "older than this driver and cannot say what a depth buffer holds\n");
		return NO;
	}
	pthread_mutex_lock(&rw_pipe_lock);
	const int depth_rc = mesa.depth_read(_texture, 0, 0, origin, size, depth, stencil, why,
					     sizeof(why));
	pthread_mutex_unlock(&rw_pipe_lock);
	if (depth_rc != 0) {
		rw_print("reading the depth of a texture: %s\n", why);
		return NO;
	}
	return YES;
}

/* A view of a texture is another texture over the same memory, and the compiler library has
 * no call for that yet; nil is the honest answer, and it is better than a view that aliases
 * nothing. */
/* [M5b: the Vulkan library has that call now (rwa_texture_view_create). On the Gallium one this
 * is still the nil that paragraph says, through hkViewFormat:. A view of everything the texture
 * has, in another format. */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
{
	return [self hkViewFormat:format
			     type:_shape.type
			   levels:NSMakeRange(0, MAX(_shape.mip_levels, 1))
			   slices:NSMakeRange(0, [self hkSlices])
			  swizzle:_swizzle];
}

/*
 * THE REST OF WHAT <MTLTexture>, <MTLTextureSPI>, <MTLResource> AND <MTLResourceSPI>
 * REQUIRE: 27 methods with no answer anywhere in this chain before this section. The same
 * three kinds as everywhere else -- served, true by construction, or refused out loud.
 *
 * The two view calls are the only ones here that hand nothing back, and they say so: a view
 * is a second texture over the first one's memory, and rwa has no call that makes one. A
 * view that aliased nothing would be found much later, by a picture that did not change.
 */
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
{
	/* [M5b: a view's swizzle is its parent's unless it says, as Metal's is] */
	return [self hkViewFormat:format type:type levels:levels slices:slices swizzle:_swizzle];
}
- (id)newTextureViewWithPixelFormat:(NSUInteger)format
			textureType:(NSUInteger)type
			     levels:(NSRange)levels
			     slices:(NSRange)slices
			    swizzle:(MTLTextureSwizzleChannels)swizzle
{
	return [self hkViewFormat:format type:type levels:levels slices:slices swizzle:swizzle];
}
- (id)newSharedTextureHandle { return nil; }   /* nothing here is shared between processes */

/* Served out of the shape this texture was made with, which is the only place that
 * knowledge exists: a cube is six faces (MTLTextureTypeCube is 3, CubeArray is 4),
 * everything else is one. */
/* [M5b: and those two numbers were not those: 3 is MTLTextureType2DArray and 4 is 2DMultisample
 * (MTLTexture.h: 1D 0, 1DArray 1, 2D 2, 2DArray 3, 2DMultisample 4, Cube 5, CubeArray 6, 3D 7), so a
 * 2D array said six faces and a cube said one, until a cube was made here. By name.] */
- (NSUInteger)numFaces
{
	return (_shape.type == MTLTextureTypeCube || _shape.type == MTLTextureTypeCubeArray) ? 6 : 1;
}
/* The identity swizzle, and it is the truth rather than a default: a texture of this driver
 * is made from a descriptor's format alone and nothing reorders its channels, so red reads
 * as red. MTLTextureSwizzleChannelsDefault is that same quadruple. */
/* [M5b: a view's own, which is that sentence's quadruple everywhere but on a view that asked.] */
- (MTLTextureSwizzleChannels)swizzle
{
	return _swizzle;
}
- (unsigned int)swizzleKey { return 0; }   /* the key of "nothing is swizzled" */
/* Nothing here is a view, so every texture is its own root, and nil is what Apple's own
 * resource answers for one that was allocated rather than derived. */
/* [M5b: and a view's root is what it was derived from, all the way up.] */
- (id)rootResource
{
	return _parent != nil ? ([_parent rootResource] ?: _parent) : nil;
}

/*
 * True by construction, and each is a sentence about this driver rather than a shrug:
 *
 *   - nothing aliases: each texture is its own allocation of the compiler library's, there
 *     are no heaps, and -makeAliasable does nothing, so "does this overlap that" is NO for
 *     every pair, for certain;
 *   - nothing is compressed: the library allocates surfaces with no lossless-compression
 *     metadata, so there is no compression and nothing to feed back;
 *   - nothing is sparse: there is no tail, so its size and its first mipmap are zero;
 *   - nothing is a drawable: a texture of this driver never comes from a CAMetalLayer, it is
 *     the attachment a pass draws into and -getBytes: reads back;
 *   - nothing is purgeable or incomplete, for the reason written over the buffer's pair.
 */
- (BOOL)doesAliasResource:(id)resource { (void)resource; return NO; }
- (BOOL)doesAliasAnyResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)doesAliasAllResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
	return NO;
}
- (BOOL)isCompressed             { return NO; }
- (NSInteger)compressionFeedback { return 0; }
- (BOOL)isSparse                 { return NO; }
- (NSUInteger)firstMipmapInTail  { return 0; }
- (NSUInteger)tailSizeInBytes    { return 0; }
- (BOOL)isShareable              { return NO; }
- (BOOL)isDrawable               { return NO; }
- (BOOL)isComplete               { return YES; }
- (void)waitUntilComplete        { }
- (BOOL)isPurgeable              { return NO; }
- (NSUInteger)protectionOptions  { return 0; }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
/* Zero is "not rotated": this texture is not a rotated view of a display surface, which is
 * the only thing in Metal that sets this. */
- (NSUInteger)rotation { return 0; }
/*
 * The identifier an argument buffer would hold for this texture. This driver has no
 * argument buffers -- the device answers no to every tier of them -- and the compiler
 * library hands out no bindless handle, so there is no identifier to give; zero is the one
 * value that cannot be mistaken for a resource, and it is said once because a caller that
 * asks is a caller that was about to write it into a buffer a shader reads.
 */
- (MTLResourceID)gpuResourceID
{
	MTLResourceID resourceID = { 0 };

	RW_ONCE("a GPU resource ID was asked of a texture; this driver has no "
		"argument buffers and no bindless handles, so it answers zero\n");
	return resourceID;
}
- (NSUInteger)gpuHandle { return 0; }
/* Whose allocation this is for the kernel's accounting. We never tell the kernel, so the
 * true answer is this process until somebody says otherwise, and then it is what they said. */
- (int)responsibleProcess
{
	return _responsibleProcess != 0 ? _responsibleProcess : getpid();
}
- (void)setResponsibleProcess:(int)pid { _responsibleProcess = pid; }

@end

@implementation HKMetalSampler

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			      sampler:(struct rwa_sampler *)sampler
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilySampler sends to super; the descriptor is passed on
	 * because the base keeps it and the pipeline-state assertions taught us not to send
	 * nil to one of these */
	self = [super initWithDevice:device samplerDescriptor:descriptor];
	if (self == nil) {
		mesa.sampler_destroy(sampler);
		return nil;
	}
	_sampler = sampler;
	return self;
}

- (void)dealloc
{
	if (_sampler != NULL) {
		mesa.sampler_destroy(_sampler);
	}
	[super dealloc];
}

- (struct rwa_sampler *)mesaSampler { return _sampler; }

@end

@implementation HKMetalDepthStencilState

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
				state:(struct rwa_depth_stencil *)state
			   descriptor:(MTLDepthStencilDescriptor *)descriptor
{
	/* The base class's own designated initialiser, which validates the descriptor,
	 * sends `init` to super and fills _device and _label (0x181a29b38). The descriptor
	 * is handed on for the same reason the sampler's is: the base reads it, and a nil
	 * there is reported by Apple's own validation. */
	self = [super initWithDevice:device depthStencilDescriptor:descriptor];
	if (self == nil) {
		mesa.depth_stencil_destroy(state);
		return nil;
	}
	_state = state;

	/*
	 * The four answers <MTLDepthStencilStateSPI> requires and the base class gives as
	 * NO (nil-stubs at 0x181b1b5e0 and its neighbours). They are the only methods of
	 * this object that are a statement about what it will do, so they are derived from
	 * the descriptor rather than left at a default that would be false for every state
	 * that writes anything.
	 *
	 * "Reads" means the test consults the buffer: MTLCompareFunctionAlways consults
	 * nothing, anything else does. For the stencil an operation other than Keep also
	 * reads it, because incrementing or inverting a value means fetching it first.
	 */
	MTLStencilDescriptor *faces[2] = { [descriptor frontFaceStencil],
					   [descriptor backFaceStencil] };

	_writesDepth = [descriptor isDepthWriteEnabled];
	_readsDepth = [descriptor depthCompareFunction] != MTLCompareFunctionAlways;
	for (unsigned i = 0; i < 2; i++) {
		MTLStencilDescriptor *f = faces[i];
		BOOL keeps;

		if (f == nil) {
			continue;   /* null_resettable, so this is Metal's default: no stencil */
		}
		keeps = [f stencilFailureOperation] == MTLStencilOperationKeep &&
			[f depthFailureOperation] == MTLStencilOperationKeep &&
			[f depthStencilPassOperation] == MTLStencilOperationKeep;
		if ([f stencilCompareFunction] != MTLCompareFunctionAlways || !keeps) {
			_readsStencil = YES;
		}
		if (!keeps && [f writeMask] != 0) {
			_writesStencil = YES;
		}
	}
	return self;
}

- (void)dealloc
{
	if (_state != NULL) {
		mesa.depth_stencil_destroy(_state);
	}
	[super dealloc];
}

- (struct rwa_depth_stencil *)mesaDepthStencil { return _state; }

- (BOOL)readsDepth    { return _readsDepth; }
- (BOOL)writesDepth   { return _writesDepth; }
- (BOOL)readsStencil  { return _readsStencil; }
- (BOOL)writesStencil { return _writesStencil; }

@end

@implementation HKMetalComputePipeline

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			     compute:(struct rwa_compute *)compute
				info:(const struct rwa_compute_info *)info
			  descriptor:(id)descriptor
{
	/* _MTLComputePipelineState's designated initialiser is the one AGX sends to super
	 * (0x2092477e0 -> 0x181a2e068). The descriptor must not be nil: the base asserts on
	 * that at its own line 1504, and the first version of this passed nil and was killed
	 * by it. */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.compute_destroy(compute);
		return nil;
	}
	_owner = device;
	_compute = compute;
	_info = *info;
	/* and the same for a kernel with a set, for the same reason */
	if (_info.argbuf_mask != 0) {
		RW_ONCE("a compute pipeline declares argument buffers at Metal buffer indices "
			"%#x; their members are bound from what an MTLArgumentEncoder "
			"recorded\n", _info.argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_compute != NULL) {
		mesa.compute_destroy(_compute);
	}
	[super dealloc];
}

- (struct rwa_compute *)compute
{
	return _compute;
}

/* The three the base class answers with nothing at all (docs/metal.md 10.8), so they can
 * only come from whoever compiled the shader -- which is why the contract carries them. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _info.max_threads_per_group; }
- (NSUInteger)threadExecutionWidth          { return _info.thread_execution_width; }
- (NSUInteger)staticThreadgroupMemoryLength { return _info.static_shared_bytes; }

- (id)device { return _owner; }

@end

/* (WHAT A FRAME PUT WHERE, above) */
static void
hk_note_frame_draw(HKMetalWork *work)
{
	const uint64_t frame = atomic_load(&hk_commits);

	if (!hk_frame_dumping() || frame % 300 != 0) {
		return;
	}
	NSString *label = [work->render label] ?: @"(none)";
	id target = work->colorCount > 0 ? work->color[0] : nil;
	char line[900];
	size_t at = (size_t)snprintf(line, sizeof(line), "frame %llu %s -> %lux%lu, %u verts", (unsigned long long)frame,
				     [label hasPrefix:@"com.apple.coreanimation"]
					 ? [[label substringFromIndex:29] UTF8String] : [label UTF8String],
				     target != nil ? (unsigned long)[target width] : 0ul,
				     target != nil ? (unsigned long)[target height] : 0ul, work->vertexCount);

	if (work->hasScissorRect) {
		at += (size_t)snprintf(line + at, sizeof(line) - at, " sc %u,%u %ux%u", work->scissorRect[0],
				       work->scissorRect[1], work->scissorRect[2], work->scissorRect[3]);
	}
	if (work->vertexBufferCount > 1 && work->vertexBuffers[1] != nil && work->vertexCount <= 64) {
		const float *v = (const float *)(const void *)((const char *)[work->vertexBuffers[1] contents] +
							       work->vertexBufferOffsets[1]);
		float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };

		for (unsigned i = 0; i < work->vertexCount; i++, v += 12) {
			const float w = v[3] != 0 ? v[3] : 1;
			const float each[4] = { v[0] / w, v[1] / w, v[4], v[5] };

			for (unsigned k = 0; k < 4; k++) {
				lo[k] = MIN(lo[k], each[k]);
				hi[k] = MAX(hi[k], each[k]);
			}
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " at %.1f,%.1f..%.1f,%.1f t %.3f,%.3f..%.3f,%.3f",
				       (double)lo[0], (double)lo[1], (double)hi[0], (double)hi[1],
				       (double)lo[2], (double)lo[3], (double)hi[2], (double)hi[3]);
	}
	for (unsigned i = 0; i < work->textureCount && at + 80 < sizeof(line); i++) {
		id t = work->textures[i];

		if (t != nil && [t isKindOfClass:[HKMetalTexture class]]) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u %lux%lu", i, (unsigned long)[t width],
					       (unsigned long)[t height]);
			at += [t hkDescribeSurface:line + at size:sizeof(line) - at];
		}
	}
	rw_print("%s\n", line);
}

/* (WHAT A SYSTEM PIPELINE WAS GIVEN, above) */
static void
hk_note_draw(HKMetalWork *work)
{
	if (!hk_tracing()) {
		return;
	}
	static NSMutableDictionary *seen;
	static unsigned lines;
	NSString *label = [work->render label] ?: @"(none)";
	bool big = false;
	static NSMutableSet *textures_seen;
	static unsigned fills;

	for (unsigned i = 0; i < work->textureCount; i++) {
		id t = work->textures[i];

		big = big || (t != nil && [t width] * [t height] >= 400000);
	}
	@synchronized([HKMetalWork class]) {
		if (textures_seen == nil) {
			textures_seen = [[NSMutableSet alloc] init];
		}
		for (unsigned i = 0; i < work->textureCount; i++) {
			id t = work->textures[i];
			NSValue *key = t != nil ? [NSValue valueWithPointer:t] : nil;

			if (t != nil && [t width] >= 100 && [t height] >= 100 && ![textures_seen containsObject:key]) {
				[textures_seen addObject:key];
				rw_print("new texture t%u %lu %lux%lu usage %#lx in %s\n", i, (unsigned long)[t pixelFormat],
					 (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t usage],
					 [label UTF8String]);
			}
		}
		/* the words of the first vertices of a whole-attachment fill, and of what draws a ramp */
		bool ramp = false;

		for (unsigned i = 0; i < work->textureCount; i++) {
			ramp = ramp || (work->textures[i] != nil && [work->textures[i] pixelFormat] == 115);
		}
		/* and the matrix a vibrancy pipeline reads out of its constant buffer: half4 rows at 0x80 */
		if ([label rangeOfString:@"Bvcm"].location != NSNotFound && work->count > 1 && work->buffers[1] != nil) {
			static unsigned matrices;

			if (matrices++ < 4) {
				const __fp16 *h = (const __fp16 *)((const char *)[work->buffers[1] contents] +
								   work->bufferOffsets[1] + 0x80);
				rw_print("matrix %s at +%lu: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f\n",
					 [[label substringFromIndex:MIN([label length], 29u)] UTF8String],
					 (unsigned long)work->bufferOffsets[1], (double)h[0], (double)h[1], (double)h[2], (double)h[3],
					 (double)h[4], (double)h[5], (double)h[6], (double)h[7], (double)h[8], (double)h[9], (double)h[10],
					 (double)h[11], (double)h[12], (double)h[13], (double)h[14], (double)h[15], (double)h[16],
					 (double)h[17], (double)h[18], (double)h[19]);
			}
		}
		if (work->vertexBufferCount > 1 && work->vertexBuffers[1] != nil && work->vertexCount == 6 &&
		    fills < 24 && (ramp || [label hasPrefix:@"com.apple.coreanimation.draw.PBGRAXm_A2Xhf"])) {
			const uint32_t *w = (const uint32_t *)((const char *)[work->vertexBuffers[1] contents] +
							       work->vertexBufferOffsets[1]);
			fills++;
			rw_print("vertices %s: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 [[label substringFromIndex:MIN([label length], 29u)] UTF8String], w[0], w[1], w[2], w[3],
				 w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
		}
	}
	@synchronized([HKMetalWork class]) {
		unsigned n;
		static CFAbsoluteTime first;
		static unsigned settled;

		if (seen == nil) {
			seen = [[NSMutableDictionary alloc] init];
			first = CFAbsoluteTimeGetCurrent();
		}
		/* and, once the screen has settled, 150 draws in a row: a whole frame, in order */
		if (CFAbsoluteTimeGetCurrent() - first > 30 && settled < 150) {
			settled++;
		} else {
			n = [[seen objectForKey:label] unsignedIntValue];
			if (n >= (big ? 6u : 1u) || lines >= 300) {
				return;
			}
			[seen setObject:@(n + 1) forKey:label];
			lines++;
		}
	}
	char line[700];
	id target = work->colorCount > 0 ? work->color[0] : nil;
	size_t at = (size_t)snprintf(line, sizeof(line), "draw %s -> %lu %lux%lu, %u verts:",
				     [label hasPrefix:@"com.apple.coreanimation"]
					 ? [[label substringFromIndex:29] UTF8String] : [label UTF8String],
				     target != nil ? (unsigned long)[target pixelFormat] : 0ul,
				     target != nil ? (unsigned long)[target width] : 0ul,
				     target != nil ? (unsigned long)[target height] : 0ul, work->vertexCount);

	for (unsigned i = 0; i < work->textureCount && at + 48 < sizeof(line); i++) {
		id t = work->textures[i];

		if (t != nil) {
			MTLTextureSwizzleChannels sw = [t swizzle];

			at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u %lu %lux%lu l%lu s%u%u%u%u", i,
					       (unsigned long)[t pixelFormat], (unsigned long)[t width],
					       (unsigned long)[t height], (unsigned long)[t mipmapLevelCount],
					       sw.red, sw.green, sw.blue, sw.alpha);
		}
	}
	for (unsigned i = 0; i < work->count && at + 24 < sizeof(line); i++) {
		if (work->buffers[i] != nil) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " b%u+%lu", i,
					       (unsigned long)work->bufferOffsets[i]);
		}
	}
	rw_print("%s\n", line);
}

static void
hk_note_dispatch(HKMetalWork *work)
{
	if (!hk_tracing()) {
		return;
	}
	static unsigned lines;
	char line[500];
	NSString *label = [work->pipeline label];

	if (__atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) >= 80) {
		return;
	}
	size_t at = (size_t)snprintf(line, sizeof(line), "dispatch %s %ux%ux%u of %ux%ux%u:", label != nil ? [label UTF8String] : "(no label)",
				     work->groups[0], work->groups[1], work->groups[2], work->size[0], work->size[1], work->size[2]);

	/* what the dispatch was given: its buffers' first words (M5e, VideoToolbox's scale writes zeros) */
	for (unsigned i = 0; i < work->count && lines < 24; i++) {
		if (work->buffers[i] != nil && [work->buffers[i] contents] != NULL) {
			const uint32_t *w = (const uint32_t *)(const void *)((const char *)[work->buffers[i] contents] + work->bufferOffsets[i]);
			const size_t words = MIN(([work->buffers[i] length] - work->bufferOffsets[i]) / 4, 24u);
			char hex[300];
			size_t h = 0;

			for (size_t k = 0; k < words; k++) {
				h += (size_t)snprintf(hex + h, sizeof(hex) - h, " %08x", w[k]);
			}
			rw_print("  buffer %u, %lu bytes +%zu:%s\n", i, (unsigned long)[work->buffers[i] length],
				 work->bufferOffsets[i], hex);
		}
	}

	for (unsigned i = 0; i < work->textureCount && at + 40 < sizeof(line); i++) {
		id t = work->textures[i];

		if (t != nil) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u %lu %lux%lu", i,
					       (unsigned long)[t pixelFormat], (unsigned long)[t width],
					       (unsigned long)[t height]);
		}
	}
	rw_print("%s\n", line);
}

@implementation HKMetalRenderPipeline

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			       render:(struct rwa_render *)render
				 info:(const struct rwa_render_info *)info
			   descriptor:(id)descriptor
{
	/* the initialiser AGXA11FamilyRenderPipeline sends to super (0x181a294dc), and a real
	 * descriptor, for the reason the compute one learned at its own line 1504 */
	self = [super initWithDevice:device pipelineStateDescriptor:descriptor];
	if (self == nil) {
		mesa.render_destroy(render);
		return nil;
	}
	_owner = device;
	_render = render;
	_info = *info;
	if (info->generated_vertex_stage) {
		rw_print("this pipeline's vertex stage was generated, so what it "
		         "draws is the fragment shader over the whole attachment\n");
	}
	/* The first pipeline of this process whose shaders declare an ARGUMENT BUFFER, named
	 * once: every measurement this port has of argument buffers is on the host, and this
	 * line is how a boot says a pipeline with a set was built at all. The indices matter
	 * because the draw binds nothing of that set out of the buffer table -- it binds what
	 * the encoder recorded for whatever buffer is bound there. */
	if (info->vertex_argbuf_mask != 0 || info->fragment_argbuf_mask != 0) {
		RW_ONCE("a render pipeline declares argument buffers: the vertex stage at "
			"Metal buffer indices %#x and the fragment stage at %#x. Their members "
			"are bound from what an MTLArgumentEncoder recorded, and a buffer "
			"bound there that nobody encoded is refused by name\n",
			info->vertex_argbuf_mask, info->fragment_argbuf_mask);
	}
	return self;
}

- (void)dealloc
{
	if (_render != NULL) {
		mesa.render_destroy(_render);
	}
	[super dealloc];
}

- (struct rwa_render *)render { return _render; }
- (BOOL)writesDepth           { return _info.fragment_writes_depth != 0; }
- (unsigned)vertexStreamMask  { return _info.vertex_stream_mask; }
- (unsigned)vertexStreamCount { return _info.vertex_streams; }
- (unsigned)vertexElementCount { return _info.vertex_attribute_count; }
- (unsigned)vertexElementAttribute:(unsigned)index
{
	unsigned n = sizeof(_info.vertex_attribute_index) /
		     sizeof(_info.vertex_attribute_index[0]);

	/* ~0u and not 0 for an index past the end: attribute 0 is a real answer, and a
	 * caller that asked for element 5 of a pipeline with two would otherwise read the
	 * mapping as if it had five. */
	return index < _info.vertex_attribute_count && index < n
	    ? _info.vertex_attribute_index[index] : ~0u;
}
- (id)device                  { return _owner; }

/* The base class answers this one with `mov x0, #0; ret` (0x181b0eee0), and a zero would read
 * as "no threads"; the number is the one the compiler library reports for a compiled stage. */
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize      { return NO; }
- (NSUInteger)imageblockSampleLength        { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
	(void)dimensions;
	return 0;
}

/* The last two of <MTLRenderPipelineStateSPI> that nothing in this chain answered. Shader
 * debug information is what Metal's own debugger reads out of a pipeline built with it;
 * there is none here, and nil is the answer Apple's driver gives for a pipeline that was
 * built without it, so the caller already has this path. */
- (id)newVertexShaderDebugInfo   { return nil; }
- (id)newFragmentShaderDebugInfo { return nil; }

@end

@implementation HKMetalWork
/* Everything a work item names is retained when it is recorded, as Metal's command buffers
 * retain what they reference: a -setVertexBytes: buffer is autoreleased, and a texture an
 * application drops after encoding must still be there at -commit. */
- (void)dealloc
{
	[pipeline release];
	[render release];
	[depth release];
	[depthStencilState release];
	[indexBuffer release];
	for (unsigned i = 0; i < 8; i++) {
		[color[i] release];
	}
	for (unsigned i = 0; i < RW_MAX_BUFFERS; i++) {
		[vertexBuffers[i] release];
		[buffers[i] release];
		[textures[i] release];
		[samplers[i] release];
		[vertexTextures[i] release];
		[vertexSamplers[i] release];
	}
	[blitFrom release];
	[blitTo release];
	[event release];
	[super dealloc];
}
@end

/*
 * THE SLOT EVERY COMMAND BUFFER TAKES FROM THE QUEUE, AND WHY IT HAS TO BE GIVEN BACK.
 *
 * -[_MTLCommandBuffer initWithQueue:retainedReferences:synchronousDebugMode:] -- the
 * designated initialiser this driver's command buffer calls, because docs/metal.md 10.13
 * says to call the real one -- does this, at 0x181a1ea14 and on:
 *
 *     ldr  x8, [x19, _MTLCommandBuffer._queue]
 *     ldr  x0, [x8, _MTLCommandQueue._commandBufferSemaphore]
 *     mov  x1, #-1                       ; DISPATCH_TIME_FOREVER
 *     bl   dispatch_semaphore_wait
 *
 * and just before it sets _MTLCommandBuffer._needsCommandBufferSemaphoreSignal to 1. The
 * semaphore is made in -[_MTLCommandQueue initWithDevice:descriptor:] (0x181a1d1fc), which
 * reads maxCommandBufferCount off the descriptor (0x181a1d268) and refuses zero; the
 * default for MTLCommandQueueDescriptor is 64. Every ivar name above is the cache's own
 * (`ipsw dyld a2s`: OBJC_IVAR_$__MTLCommandQueue._commandBufferSemaphore,
 * OBJC_IVAR_$__MTLCommandBuffer._needsCommandBufferSemaphoreSignal), not a guess.
 *
 * So each command buffer takes one of 64 slots AND OWES A SIGNAL -- that ivar is Apple's
 * own name for the debt. In Apple's driver something on the completion path pays it. This
 * driver has no completion path at all: it overrides -commit and -waitUntilCompleted and
 * never calls -enqueueCommandBuffer:, -commitCommandBuffer:wake: or any of the rest, so
 * nothing in Metal will ever pay it for us. The 65th -commandBuffer then waits for ever.
 *
 * MEASURED, not reasoned: logs/114823-run.log. A test that made 64 command buffers (one
 * per shader of an 8x8 mosaic) blocked on the 65th inside -[MTLCommandQueue commandBuffer]
 * and stayed blocked for four minutes, with a heartbeat thread proving the process alive.
 * It is not a test bug: an application makes a command buffer per frame, so this driver
 * would hang anything that drew for a couple of seconds.
 *
 * The debt is paid at -commit, and that is a choice with a reason. Metal holds the slot
 * until the buffer completes; here -commit IS the completion -- the recorded work runs
 * inside it and -waitUntilCompleted only fences -- so holding it any longer would throttle
 * an application for a wait this driver does not have. A buffer that is never committed
 * pays at -dealloc instead, which is the case Apple's flag exists for.
 *
 * Both are done through the runtime by ivar NAME, and a name that is not there is said out
 * loud rather than worked around: on an iOS whose Metal keeps this differently, the right
 * answer is to find out, not to run with a queue that silently blocks at 64.
 */
static dispatch_semaphore_t
hk_queue_semaphore(id queue)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_commandBufferSemaphore")
				   : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no "
			         "_commandBufferSemaphore in this Metal, so the slot every "
			         "command buffer takes cannot be given back and this queue "
			         "will block on its maxCommandBufferCount'th buffer\n");
		}
	}
	return ivar != NULL ? (dispatch_semaphore_t)object_getIvar(queue, ivar) : nil;
}

/* Where a queue's completion queue lives: Apple's own _MTLCommandQueue._completionQueue,
 * found by NAME for the same reason the semaphore above is -- the base class already has
 * the getter for it, and a second copy in this file would mean a queue built from a
 * descriptor that carried one reading back as nil. A Metal without the ivar is said out
 * loud once and falls back to this driver's own field. */
static Ivar
hk_completion_ivar(void)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandQueue");

		looked = YES;
		ivar = base != Nil ? class_getInstanceVariable(base, "_completionQueue") : NULL;
		if (ivar == NULL) {
			rw_print("_MTLCommandQueue has no _completionQueue in this "
			         "Metal, so -setCompletionQueue: keeps the queue in this driver's "
			         "own field instead of the one Apple's getter reads\n");
		}
	}
	return ivar;
}

/* Apple's own "this buffer still owes a signal" flag, cleared when we pay, so that its
 * bookkeeping and ours cannot disagree about a debt that has been settled. */
static void
hk_clear_semaphore_debt(id commandBuffer)
{
	static Ivar ivar;
	static BOOL looked;

	if (!looked) {
		Class base = objc_getClass("_MTLCommandBuffer");

		looked = YES;
		ivar = base != Nil ?
		    class_getInstanceVariable(base, "_needsCommandBufferSemaphoreSignal") : NULL;
	}
	if (ivar != NULL) {
		*(unsigned char *)((char *)commandBuffer + ivar_getOffset(ivar)) = 0;
	}
}

@implementation HKMetalCommandQueue {
	HKMetalDevice *_owner;
	NSUInteger _outstanding;        /* buffers of this queue that still hold a slot */
	NSUInteger _made, _returned;
	/* What <MTLCommandQueueSPI> keeps about a queue and this driver cannot act on: the
	 * two priorities are remembered so that a caller reading back what it set is not
	 * lied to, and -setCompletionQueue: explains the third below. */
	NSUInteger _gpuPriority, _backgroundGPUPriority;
	dispatch_queue_t _completionQueue;   /* only used when Apple's own ivar is missing */
}

- (instancetype)initWithHKDevice:(HKMetalDevice *)device descriptor:(id)descriptor
{
	/* The initialiser IOGPUMetalCommandQueue itself sends to super (0x1ec16dabc). The
	 * descriptor is not optional: the base validates it at its own line 137 and the first
	 * version of this passed nil and was killed by that assertion. */
	self = [super initWithDevice:device descriptor:descriptor];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	/* Said at birth and once: this number is how many command buffers may exist at a
	 * time before -commandBuffer blocks, and until this driver returned the slot it was
	 * also how many this process could ever make. */
	rw_print("this queue takes %lu command buffers at a time "
	         "(maxCommandBufferCount), and each one waits on the queue's semaphore in "
	         "-[_MTLCommandBuffer initWithQueue:...]; this driver gives the slot back at "
	         "-commit%s\n", (unsigned long)[self maxCommandBufferCount],
	         hk_queue_semaphore(self) != nil ? "" :
	         ", EXCEPT that the semaphore was not found and it cannot");
	return self;
}

- (id)device { return _owner; }

/* What the queue itself thinks, for a test that wants the number rather than the
 * inference: how many command buffers of this queue still hold one of those slots. */
- (NSUInteger)hkOutstandingCommandBuffers { return _outstanding; }
- (NSUInteger)hkCommandBuffersMade        { return _made; }

- (void)hkTookSlot
{
	_outstanding++;
	_made++;
}

- (void)hkReturnedSlot
{
	if (_outstanding > 0) {
		_outstanding--;
	}
	_returned++;
}

- (id)commandBuffer
{
	return [[[HKMetalCommandBuffer alloc] initWithHKQueue:self] autorelease];
}

- (id)commandBufferWithUnretainedReferences
{
	return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary { }

/*
 * THE TWO METHODS THE SYSTEM NAMED, AND WHAT THEY REALLY ARE.
 *
 * With metal=1 and launchd running, backboardd and mediaserverd sent this queue
 * -setGPUPriority: 58 times and -setCompletionQueue: 30 times in a hundred seconds, and
 * every one of them killed the daemon that sent it (logs/130132-run.log). Neither is in any
 * public header: both are required members of <MTLCommandQueueSPI>, which is declared in
 * AGXMetalA11.bundle and therefore does not exist in any process on this phone -- so
 * objc_getProtocol cannot be asked what this class owes, and the list was read out of the
 * cache instead (tools/metal/protometh.py, tools/metal/owes.py).
 *
 * -setGPUPriority: (B24@0:8Q16 -- NSUInteger in, BOOL out) is NOT bookkeeping.
 * -[IOGPUMetalCommandQueue setGPUPriority:] (0x1ec174cb8) calls _validateGPUPriority
 * (0x1ec178f74) and then -_setGPUPriority:backgroundPriority: (0x1ec178eec), which packs
 * the foreground and background numbers into a three-word request and hands it to the
 * kernel's own command-queue object (_IOGPUCommandQueueSetPriorityAndBackground); only if
 * the kernel accepts does it write the two ivars that -getGPUPriority reads back. So the
 * number is read by the GPU's scheduler, and the ivar is just a cache of what the scheduler
 * agreed to. The validation is worth copying exactly: anything above 5 is refused and 3 is
 * refused for a foreground queue -- "priority (%d) is not valid for foreground" is the
 * cache's own string -- and it only REPORTS, it does not stop the call, which is why
 * Apple's BOOL is the kernel's answer and not the validator's.
 *
 * This driver has one ring and one priority. Every command buffer goes to the kernel's
 * single msm ring in the order it was committed, and there is no second queue to be ahead
 * of; so there is nothing here to tell, and the honest answer is to remember the number,
 * report it back, and say once that it changes nothing.
 */
- (BOOL)setGPUPriority:(NSUInteger)priority
{
	if (priority > 5 || priority == 3) {
		RW_ONCE("GPU priority %lu is outside what Apple's own "
			"_validateGPUPriority allows (0..5, and 3 is the background-only "
			"one); it is remembered as asked, like Apple's own path, which "
			"reports such a value and carries on\n", (unsigned long)priority);
	}
	RW_ONCE("a GPU priority of %lu was asked for; this driver has one ring "
		"and one priority, so the number is remembered and reported back but nothing "
		"schedules by it\n", (unsigned long)priority);
	_gpuPriority = priority;
	return YES;
}

/* The offset is a place WITHIN a priority band (Apple's second argument is an unsigned
 * short, S24 in the encoding). With one ring there is no band to be placed in. */
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	if (offset != 0) {
		RW_ONCE("a GPU priority offset of %u is ignored: this driver has "
			"one ring, so a place inside a priority band means nothing here\n",
			(unsigned)offset);
	}
	return [self setGPUPriority:priority];
}

/* The background priority is the one a queue gets while its process is not frontmost. Same
 * answer, kept separately so that the two reads do not lie about each other. */
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
	RW_ONCE("a background GPU priority of %lu was asked for; remembered, "
		"not honoured, for the same reason as the foreground one\n",
		(unsigned long)priority);
	_backgroundGPUPriority = priority;
	return YES;
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
	(void)offset;
	return [self setBackgroundGPUPriority:priority];
}

- (NSUInteger)getGPUPriority           { return _gpuPriority; }
- (NSUInteger)getBackgroundGPUPriority { return _backgroundGPUPriority; }

/*
 * -setCompletionQueue: (v24@0:8@16) names the dispatch queue on which this command queue
 * delivers what finishes. -[IOGPUMetalCommandQueue setCompletionQueue:] (0x1ec178e88)
 * releases the queue it held, retains the new one into _MTLCommandQueue._completionQueue --
 * the ivar the base class already has a getter for -- and re-targets the queue's own
 * dispatch source at it (0x1ec1765e8), so that every command buffer's completion handler
 * runs there instead of on whichever thread the kernel's notification arrived on. That is
 * exactly why backboardd sets one: a completion handler that runs on a daemon's render
 * thread is a completion handler that can stall a frame.
 *
 * So this one is SERVED, not declined: the queue is kept, and -[HKMetalCommandBuffer
 * commit] delivers the scheduled and completed handlers on it (see the note there -- before
 * this they ran inline, at the moment they were added, which is before the work they are
 * about). It is kept in Apple's ivar rather than in one of ours so that a queue built from
 * a descriptor that already carried a completion queue (there is a
 * -[MTLCommandQueueDescriptorInternal setCompletionQueue:]) reads back as what it is; the
 * retain is balanced by -[_MTLCommandQueue dealloc], the ivar's owner.
 */
- (void)setCompletionQueue:(dispatch_queue_t)queue
{
	Ivar ivar = hk_completion_ivar();
	dispatch_queue_t held = ivar != NULL ?
	    (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;

	if (held == queue) {
		return;
	}
	[queue retain];
	[held release];
	if (ivar != NULL) {
		object_setIvar(self, ivar, queue);
	} else {
		_completionQueue = queue;
	}
}

/* What this driver reads when it has something to deliver. Not an override of -completionQueue
 * (the base class's getter reads the same ivar): this is here for the Metal where that ivar
 * is not, so that one missing ivar does not quietly send every handler to the wrong thread. */
- (dispatch_queue_t)hkCompletionQueue
{
	Ivar ivar = hk_completion_ivar();

	return ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : _completionQueue;
}

- (void)dealloc
{
	/* Only the copy this driver keeps itself: the one in Apple's ivar belongs to
	 * -[_MTLCommandQueue dealloc], and releasing it twice would be worse than leaking
	 * one dispatch queue in the Metal where that dealloc does not. */
	[_completionQueue release];
	[super dealloc];
}

@end

/*
 * MTLCommandBufferStatus, AND WHAT A LOG FROM BEFORE THIS CHANGE SAYS.
 *
 * These are Metal's own numbers (MTLCommandBuffer.h): NotEnqueued 0, Enqueued 1, Committed 2,
 * Scheduled 3, Completed 4, Error 5. -[MTLCommandBuffer status] is how real code decides that a
 * buffer is done, so they are not cosmetic.
 *
 * Until 2026-10-21 this file answered 3 for committed, 4 for scheduled and 6 for completed -- and
 * 6 is not a status at all. Under those numbers an application comparing with
 * MTLCommandBufferStatusCompleted (4) saw completion the moment -commit returned, BEFORE the GPU
 * fence of -waitUntilCompleted was paid, and after the fence it saw a number Metal does not
 * define. Error was 5 by coincidence and has not moved.
 *
 * Every boot log under logs/ was written by the old numbering and none of them is going to be
 * regenerated, so those numbers have to be DATED rather than reinterpreted. That is what the
 * banner in -initWithHKQueue: below is for: it names the numbering of the log that carries
 * it, and it is printed out of these constants, so the line cannot drift from what -status
 * answers. A log without that line is older than this change, and in it "status 6" is this
 * completion, "status 4" is a buffer that was only submitted, and 2 and 3 do not occur at all.
 * That is the rule tools/air/metalcheck.py reads a log by, and the measurement that makes it
 * safe: over all 141 logs in logs/ the only statuses the test ever printed are 5 (128 times) and
 * 6 (990 times) -- never 4.
 *
 * One file more keys on the old numbers and is NOT part of this change: userland/src/
 * metal_drv_test.m compares [cmd status] with the literal 6 in five places (its lines 1200, 1526,
 * 2041, 2293, 2795) and prints the literal "status 6" in a sixth (2308). Until those move, a boot
 * with this driver has the test calling every completed buffer a failure: the sweep prints
 * "drvtest: drawall <name> status 4" with nothing after it and reads back no pixel, and the single
 * draw returns before its own readback. metalcheck.py names that shape when it sees it.
 */
enum {
	RW_CB_NOT_ENQUEUED = 0,
	RW_CB_ENQUEUED     = 1,
	RW_CB_COMMITTED    = 2,
	RW_CB_SCHEDULED    = 3,
	RW_CB_COMPLETED    = 4,
	RW_CB_ERROR        = 5,
};

static dispatch_queue_t
hk_fence_queue(void)
{
	static dispatch_queue_t queue;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		queue = dispatch_queue_create("hk.metal.completion", DISPATCH_QUEUE_SERIAL);
	});
	return queue;
}

@implementation HKMetalCommandBuffer {
	HKMetalCommandQueue *_queue;
	NSMutableArray *_work;
	NSError *_error;
	_Atomic(NSUInteger) _status;
	BOOL _owesSlot;                 /* this buffer holds one of the queue's slots */
	NSMutableArray *_scheduled, *_completed;   /* handler blocks, copied to the heap */
	struct rwa_fence *_fence;       /* this buffer's submission, from -commit; NULL before */
	NSUInteger _protectionOptions;  /* what the window server asked for; see -setProtectionOptions: */
	NSUInteger _next;               /* the work item -hkRun goes on from (an event's wait stops it) */
	NSMutableArray *_signals;       /* event signals run, made visible once this buffer is complete */
	NSMutableArray *_hkWatched;     /* (trace: a dispatch's textures, read back when complete: hkWatchReport) */
	NSCondition *_ran;              /* ... and whether all of it has run (-waitUntilCompleted waits for that) */
	BOOL _hasRun;
	NSMutableArray *_bytesChunks;   /* M5g: the chunks -hkBytes: used, the last one still filling */
	NSUInteger _bytesUsed;
	uint64_t _committedAt;          /* M5g: for hk_stats_say */
	NSUInteger _preparedTo;         /* M5g: the work items -hkPrepareDrawsFrom: has been over */
}

/*
 * The window server sends this to every command buffer it makes, and until it was answered the
 * boot of 22 October 01:51 ended it 78 times with
 *
 *   NSInvalidArgumentException: -[HKMetalCommandBuffer setProtectionOptions:]:
 *   unrecognized selector sent to instance
 *
 * -- an uncaught Objective-C exception out of CA::WindowServer::IOMFBServer::thread_body, which
 * is terminate() and not a refusal we could read.
 *
 * It was missed by owes.py for a reason worth keeping: that tool counts what the PROTOCOLS
 * require, and this pair is declared on a CLASS -- IOGPUMetalCommandBuffer, whose own
 * _protectionOptions ivar it is. Our command buffer descends from _MTLCommandBuffer and not
 * from IOGPU's, so nothing in the chain answers it and nothing in the protocol lists says it is
 * missing. Every selector Apple's own counterpart class declares is a selector a caller may
 * send, and the protocols are only part of that list.
 *
 * The value is kept and not honoured. Protected content means memory the GPU can read and the
 * processor cannot, which this port has nothing to provide: a buffer of ours is ordinary memory
 * on both sides. So a non-zero request says so once rather than being refused -- the window
 * server asks for it on buffers that carry no protected content at all, and refusing would stop
 * a composite that is in no danger.
 */
- (void)setProtectionOptions:(NSUInteger)options
{
	if (options != 0 && _protectionOptions != options) {
		RW_ONCE("a command buffer was asked for protection options %lu, and this "
			"driver has no protected memory to give: the request is kept and not honoured, "
			"because nothing it would protect is protected here either\n",
			(unsigned long)options);
	}
	_protectionOptions = options;
}

- (NSUInteger)protectionOptions
{
	return _protectionOptions;
}

- (instancetype)initWithHKQueue:(HKMetalCommandQueue *)queue
{
	/* what IOGPUMetalCommandBuffer sends to super, retained references and no debug mode.
	 * THIS CALL BLOCKS when the queue's semaphore is empty -- see the long note above
	 * HKMetalCommandQueue -- so by the time it returns, a slot is held and owed. */
	self = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];
	if (self == nil) {
		return nil;
	}
	_queue = [queue retain];
	_work = [[NSMutableArray alloc] init];
	/* The numbering this log is written in, said once per process and printed out of the
	 * constants that answer -status: a log that does NOT carry this line was written before
	 * the numbers became Metal's, and says committed 3, scheduled 4, completed 6 (see the
	 * note above this class). */
	RW_ONCE("MTLCommandBufferStatus: committed %u, scheduled %u, completed %u, "
		"error %u -- Metal's own numbers; a log without this line was written by the old "
		"ones (committed 3, scheduled 4, completed 6)\n",
		(unsigned)RW_CB_COMMITTED, (unsigned)RW_CB_SCHEDULED,
		(unsigned)RW_CB_COMPLETED, (unsigned)RW_CB_ERROR);
	_status = RW_CB_ENQUEUED;   /* nothing is pending yet */
	_signals = [[NSMutableArray alloc] init];
	_ran = [[NSCondition alloc] init];
	_owesSlot = YES;
	[queue hkTookSlot];
	return self;
}

/*
 * Give the queue's slot back, exactly once. Called at the end of -commit, where this
 * driver's work is already done, and at -dealloc for a buffer that was never committed --
 * which is the case Apple's _needsCommandBufferSemaphoreSignal exists for and the one that
 * would otherwise lose a slot for the life of the process.
 */
- (void)hkReturnSlot
{
	dispatch_semaphore_t semaphore;

	if (!_owesSlot) {
		return;
	}
	_owesSlot = NO;
	hk_clear_semaphore_debt(self);
	[_queue hkReturnedSlot];
	semaphore = hk_queue_semaphore(_queue);
	if (semaphore != nil) {
		dispatch_semaphore_signal(semaphore);
	}
}

- (id)hkBytes:(const void *)bytes length:(NSUInteger)length offset:(NSUInteger *)offset
{
	HKMetalDevice *device = [_queue device];

	atomic_fetch_add(&hk_stat.bytes_calls, 1);
	*offset = 0;
	/* (a Gallium library, which is the one without the counts, takes no offset everywhere) */
	if (length > RW_BYTES_MOST || mesa.stats_read == NULL) {
		return [[device newBufferWithBytes:bytes length:length options:0] autorelease];
	}
	NSUInteger at = (_bytesUsed + RW_BYTES_ALIGN - 1) & ~(NSUInteger)(RW_BYTES_ALIGN - 1);

	if ([_bytesChunks count] == 0 || at + length > RW_BYTES_CHUNK) {
		id chunk = [device hkTakeBytesChunk];

		if (chunk == nil) {
			return nil;
		}
		if (_bytesChunks == nil) {
			_bytesChunks = [[NSMutableArray alloc] init];
		}
		[_bytesChunks addObject:chunk];
		[chunk release];
		at = 0;
	}
	id chunk = [_bytesChunks lastObject];

	memcpy((uint8_t *)[chunk contents] + at, bytes, length);
	_bytesUsed = at + length;
	*offset = at;
	return chunk;
}

- (void)dealloc
{
	[self hkReturnSlot];
	if (_fence != NULL) {
		mesa.fence_release([[_queue device] mesaDevice], _fence);
	}
	[_bytesChunks release];    /* (given back at completion; if that was never asked, the library frees them when the GPU is done) */
	[_work release];
	[_signals release];
	[_ran release];
	[_error release];
	[_scheduled release];
	[_completed release];
	[_queue release];
	[super dealloc];
}

- (id)device       { return [_queue device]; }
- (id)commandQueue { return _queue; }
- (NSUInteger)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }

- (id)computeCommandEncoder
{
	return [[[HKMetalComputeEncoder alloc] initWithHKCommandBuffer:self]
		   autorelease];
}

- (id)renderCommandEncoderWithDescriptor:(id)descriptor
{
	if (descriptor == nil) {
		rw_print("a render pass needs a descriptor\n");
		return nil;
	}
	return [[[HKMetalRenderEncoder alloc] initWithHKCommandBuffer:self
								    descriptor:descriptor]
		   autorelease];
}

- (id)parallelRenderCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return nil;   /* one encoder at a time here; a parallel pass needs the split */
}

/* The rest of what MTLCommandBuffer requires of a driver. Each declines rather than being
 * absent, for the reason the device's own 44 were answered: a missing selector aborts the
 * caller, and these are the four that were still missing. */
- (id)blitCommandEncoder
{
	return [[[HKMetalBlitEncoder alloc] initWithHKCommandBuffer:self] autorelease];
}

/* MTLBlitPassDescriptor carries only counter sampling, which this driver has none of */
- (id)blitCommandEncoderWithDescriptor:(id)descriptor
{
	(void)descriptor;
	return [self blitCommandEncoder];
}
- (id)resourceStateCommandEncoder { return nil; }
- (id)accelerationStructureCommandEncoder { return nil; }
/*
 * Events, at their place among the encoders. A signal takes effect for later waits when the
 * buffer runs (the GPU's work is in order, so what follows it in the context runs after it) and
 * for the processor -- signaledValue, listeners -- once the buffer is complete. A wait that is
 * not reached stops this buffer there; the signal that reaches it runs the rest (-hkRun).
 */
- (void)hkEncodeEvent:(id)event value:(uint64_t)value op:(unsigned)op
{
	HKMetalWork *work;

	if (![event isKindOfClass:[HKMetalEvent class]]) {
		RW_ONCE("an event that is not this driver's was encoded; it is ignored\n");
		return;
	}
	work = [[HKMetalWork alloc] init];
	work->eventOp = op;
	work->event = [event retain];
	work->eventValue = value;
	[self recordWork:work];
	[work release];
}
- (void)encodeSignalEvent:(id)event value:(uint64_t)value { [self hkEncodeEvent:event value:value op:RW_EVENT_SIGNAL]; }
- (void)encodeWaitForEvent:(id)event value:(uint64_t)value { [self hkEncodeEvent:event value:value op:RW_EVENT_WAIT]; }

- (id)computeCommandEncoderWithDispatchType:(NSUInteger)type
{
	(void)type;   /* serial and concurrent dispatch are the same here: one queue, in order */
	return [self computeCommandEncoder];
}

- (void)recordWork:(HKMetalWork *)work
{
	[_work addObject:work];
}

- (void)enqueue { }

/* Metal's own rule, kept: the recorded dispatches run here and not when they were encoded. */
/* [M5d: rw_pipe_lock is further up now, and the texel calls take it.] */

/* An event's signal, run: the buffers it lets go on are added to `ready` (made if NULL). */
static NSMutableArray *
rw_signal(HKMetalWork *work, NSMutableArray *ready, NSMutableArray *signals)
{
	NSArray *now = [work->event hkSignalOrdered:work->eventValue];

	[signals addObject:work];
	if ([now count] == 0) {
		return ready;
	}
	if (ready == nil) {
		ready = [[NSMutableArray alloc] init];
	}
	[ready addObjectsFromArray:now];
	return ready;
}

/*
 * (M5g, ONE PASS FOR MANY DRAWS in air_vk.c.) The draws from here that go on over the same attachments, with no load
 * action and no barrier of the encoder's among them: what they sample is moved to where it can be sampled now, ahead of
 * the pass. Met at the draw that first samples it, each of those barriers would end the pass and begin another (a third of
 * the passes of Weather's frames, run 210414). What a draw does not in fact read costs a barrier, and nothing that
 * follows relies on the layout.
 */
- (void)hkPrepareDrawsFrom:(NSUInteger)from
{
	HKMetalWork *first = [_work objectAtIndex:from];
	struct rwa_texture *color[8] = { NULL };
	struct rwa_texture *depth = [first->depth mesaTexture];
	char why[256] = { 0 };

	_preparedTo = from + 1;
	if (mesa.texture_prepare == NULL) {
		return;
	}
	for (unsigned i = 0; i < first->colorCount; i++) {
		color[i] = [first->color[i] mesaTexture];
	}
	for (NSUInteger at = from; at < [_work count]; at++) {
		HKMetalWork *w = [_work objectAtIndex:at];

		if (w->render == nil || w->eventOp != 0 || w->blit != 0 || w->colorCount != first->colorCount ||
		    w->depth != first->depth || w->depthLevel != first->depthLevel || w->depthSlice != first->depthSlice) {
			break;
		}
		bool same = true;

		for (unsigned i = 0; i < first->colorCount; i++) {
			same = same && w->color[i] == first->color[i] && w->colorLevel[i] == first->colorLevel[i] &&
			       w->colorSlice[i] == first->colorSlice[i];
		}
		if (!same || (at != from && (w->freshPass || w->clears || w->attachmentClearMask != 0 || w->clearsDepth || w->clearsStencil))) {
			break;
		}
		for (unsigned i = 0; i < w->vertexTextureCount; i++) {
			if (mesa.texture_prepare([w->vertexTextures[i] mesaTexture], color, first->colorCount, depth, why, sizeof(why)) != 0) {
				return;
			}
		}
		for (unsigned i = 0; i < w->textureCount; i++) {
			if (mesa.texture_prepare([w->textures[i] mesaTexture], color, first->colorCount, depth, why, sizeof(why)) != 0) {
				return;
			}
		}
		_preparedTo = at + 1;
	}
}

- (void)commit
{
	/* (how long a process took to its first frame, from exec: what a shader built before the boot is to shorten, the owner's first open of an app) */
	if (atomic_fetch_add(&hk_commits, 1) == 0 && hk_stats_on()) {
		int name[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
		struct kinfo_proc info;
		size_t size = sizeof(info);
		struct timeval now;

		gettimeofday(&now, NULL);
		if (sysctl(name, 4, &info, &size, NULL, 0) == 0) {
			rw_print("first commit %lld ms after the process began\n", ((long long)now.tv_sec - info.kp_proc.p_starttime.tv_sec) * 1000 + ((long long)now.tv_usec - info.kp_proc.p_starttime.tv_usec) / 1000);
		}
	}
	_status = RW_CB_COMMITTED;
	_committedAt = hk_now_us();
	[self hkRun];
	const uint64_t run = hk_now_us() - _committedAt;

	/*
	 * (M5g, a probe: the file commit_sync.) The window server's -commit does not come back until the GPU has finished what it
	 * gave. If a frame reached the panel before the GPU was done with it, this is the boot where that cannot happen.
	 */
	static int commit_sync = -1;

	if (commit_sync < 0) {
		commit_sync = access("/hackios/share/probes/commit_sync", F_OK) == 0 && strcmp(getprogname(), "backboardd") == 0;
	}
	if (commit_sync) {
		[self waitUntilCompleted];
	}

	atomic_fetch_add(&hk_stat.commits, 1);
	atomic_fetch_add(&hk_stat.run_us, run);
	hk_stat_most(&hk_stat.run_longest, run);
	hk_stats_say();
}

- (void)hkRun
{
	char why[512] = { 0 };
	NSMutableArray *ready = nil;

	pthread_mutex_lock(&rw_pipe_lock);
	for (; _next < [_work count]; _next++) {
		HKMetalWork *work = [_work objectAtIndex:_next];

		if (work->eventOp == RW_EVENT_WAIT) {
			if ([work->event hkWait:self value:work->eventValue]) {
				/* the event holds this buffer now; the signal that reaches it calls -hkRun */
				_next++;
				pthread_mutex_unlock(&rw_pipe_lock);
				[self hkReturnSlot];
				return;
			}
			continue;
		}
		if (work->eventOp == RW_EVENT_SIGNAL) {
			ready = rw_signal(work, ready, _signals);
			continue;
		}
		if (work->blit != 0) {
			if (![self runBlit:work why:why size:sizeof(why)]) {
				goto done;
			}
			continue;
		}
		if (work->render != nil) {
			if (_next >= _preparedTo) {
				[self hkPrepareDrawsFrom:_next];
			}
			if (![self runDraw:work why:why size:sizeof(why)]) {
				goto done;
			}
			continue;
		}
		struct rwa_buffer *buffers[RW_MAX_BUFFERS] = { NULL };
		struct rwa_texture *textures[RW_MAX_BUFFERS] = { NULL };
		struct rwa_sampler *samplers[RW_MAX_BUFFERS] = { NULL };
		/* Metal's one [[buffer(n)]] table, passed BOTH ways: `buffers` is what is
		 * bound at each index, `table` is the same thing with Metal's own offset
		 * beside it, and the library takes from each side what that side owns -- an
		 * argument buffer's encoding offset and a buffer argument's own offset come
		 * from the table, and which an index is nobody but the shader knows. */
		struct rwa_buffer_bind table[RW_MAX_BUFFERS] = { { NULL, 0 } };

		for (unsigned i = 0; i < work->count; i++) {
			buffers[i] = [work->buffers[i] mesaBuffer];
			table[i].buffer = buffers[i];
			table[i].offset = work->bufferOffsets[i];
			if (buffers[i] == NULL && work->buffers[i] != nil) {
				rw_print("buffer %u of this dispatch did not come "
				         "from the compiler library, so it cannot be bound\n", i);
			}
		}
		for (unsigned i = 0; i < work->textureCount; i++) {
			textures[i] = [work->textures[i] mesaTexture];
		}
		for (unsigned i = 0; i < work->samplerCount; i++) {
			samplers[i] = [work->samplers[i] mesaSampler];
		}
		struct rwa_bindings bindings = {
			.buffers = buffers, .buffer_count = work->count,
			.textures = textures, .texture_count = work->textureCount,
			.samplers = samplers, .sampler_count = work->samplerCount,
		};

		/* rwa_dispatch_buffers when the library has it, which is the same call with
		 * Metal's offsets passed as well; a library too old to have it is used
		 * through rwa_dispatch, and then a kernel with a set is refused by name
		 * there and every offset is read as zero -- which is what an older library
		 * did with one in any case, out loud. */
		hk_note_dispatch(work);
		if (hk_tracing()) {
			[self hkWatch:work];
		}
		const CFAbsoluteTime dispatch_began = CFAbsoluteTimeGetCurrent();
		/* (WHAT A SYSTEM PIPELINE WAS GIVEN) a dispatch that does not come back says where it is:
		 * only with /hackios/share/probes/watchdog in the root, because this kills the process */
		static _Atomic(uint64_t) dispatch_serial;
		static _Atomic(uint64_t) dispatch_done;
		const uint64_t this_dispatch = ++dispatch_serial;
		pthread_t dispatching = pthread_self();

		if (access("/hackios/share/probes/watchdog", F_OK) == 0) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
				       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
				if (dispatch_done < this_dispatch) {
					rw_print("a dispatch has not come back in 20 s; where it is:\n");
					pthread_kill(dispatching, SIGTRAP);
				}
			});
		}
		int rc = mesa.dispatch_buffers != NULL
			    ? mesa.dispatch_buffers([work->pipeline compute], &bindings,
						    table, work->count, work->groups,
						    work->size, why, sizeof(why))
			    : mesa.dispatch([work->pipeline compute], &bindings, work->groups,
					    work->size, why, sizeof(why));
		dispatch_done = this_dispatch;
		if (CFAbsoluteTimeGetCurrent() - dispatch_began > 0.25) {
			rw_print("a dispatch took %.1f s: %s\n", CFAbsoluteTimeGetCurrent() - dispatch_began,
				 [[[work->pipeline label] description] UTF8String] ?: "(no label)");
		}
		if (rc != 0) {
			rw_print("the dispatch failed: %s\n", why);
			_error = [[NSError errorWithDomain:@"HKMetal" code:2
						  userInfo:@{ NSLocalizedDescriptionKey:
							      [NSString stringWithUTF8String:why] }]
				     retain];
			_status = RW_CB_ERROR;
			goto done;
		}
	}
	if (mesa.defer_waits != NULL &&
	    mesa.flush([[_queue device] mesaDevice], &_fence, why, sizeof(why)) != 0) {
		[self failWith:why];
		goto done;
	}
	_status = RW_CB_SCHEDULED;   /* submitted; the fence below is what makes it completed */
done:
	if (_status == RW_CB_ERROR) {
		/* a failed buffer's signals are given all the same, or whoever waits for them waits for ever */
		for (_next++; _next < [_work count]; _next++) {
			HKMetalWork *work = [_work objectAtIndex:_next];

			if (work->eventOp == RW_EVENT_SIGNAL) {
				ready = rw_signal(work, ready, _signals);
			}
		}
	}
	pthread_mutex_unlock(&rw_pipe_lock);
	[_ran lock];
	_hasRun = YES;
	[_ran broadcast];
	[_ran unlock];
	/* The queue's slot goes back here and on every way out of this method, a failed draw
	 * included: a buffer that errored is as finished as one that drew, and a slot kept
	 * because of an error would turn one bad draw into a queue that blocks for ever. */
	[self hkReturnSlot];
	/* and only then the handlers, so that a handler which makes the next command buffer
	 * is not waiting for a slot this one still holds */
	[self hkDeliverHandlers];
	/* and the buffers this one's signals let go on, after its own work is submitted */
	for (HKMetalCommandBuffer *buffer in ready) {
		[buffer hkRun];
	}
	[ready release];
}

/* A draw, built out of what the encoder recorded. Separate from the dispatch loop because the
 * two have different bindings -- a draw has them per stage -- and folding them together would
 * mean a struct whose fields mean different things depending on a flag. */
- (BOOL)runDraw:(HKMetalWork *)work why:(char *)why size:(size_t)size
{
	struct rwa_texture *color[8] = { NULL };
	struct rwa_buffer *vertex_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_vertex_stream vertex_streams[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer *fragment_buffers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_texture *fragment_textures[RW_MAX_BUFFERS] = { NULL };
	struct rwa_sampler *fragment_samplers[RW_MAX_BUFFERS] = { NULL };
	struct rwa_texture *vertex_textures[RW_MAX_BUFFERS] = { NULL };
	struct rwa_sampler *vertex_samplers[RW_MAX_BUFFERS] = { NULL };
	/* and Metal's own table for each stage: the same entries as the arrays above with
	 * the OFFSET beside each, which rwa_bindings has nowhere to put. One array for
	 * both of the things an offset means -- which encoding of an argument buffer, and
	 * which block of a buffer argument -- because Metal has one call behind both. */
	struct rwa_buffer_bind vertex_table[RW_MAX_BUFFERS] = { { NULL, 0 } };
	struct rwa_buffer_bind fragment_table[RW_MAX_BUFFERS] = { { NULL, 0 } };

	if (mesa.render_draw == NULL) {
		snprintf(why, size, "this compiler library can compute but not yet draw");
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->colorCount; i++) {
		color[i] = [work->color[i] mesaTexture];
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index
	 * the vertex descriptor fetches attributes from is a STREAM, with its offset, and
	 * every other index is a [[buffer(n)]] ARGUMENT of the vertex stage. The library
	 * refuses a descriptor that claims an index the module also reads as a buffer
	 * (air_metal.h), so the two sets cannot overlap and this split cannot drop a
	 * binding.
	 *
	 * The offset is honoured on the stream side and still unimplemented on the argument
	 * side, and that asymmetry is said out loud rather than left in a comment: the
	 * library takes a stream's offset as Metal's own `offset:` and rwa_bindings has
	 * nowhere to put one.
	 */
	unsigned stream_mask = [work->render vertexStreamMask];

	for (unsigned i = 0; i < work->vertexBufferCount; i++) {
		/* Metal's own offset, for whichever of the other two things index i is: an
		 * argument buffer (which encoding to read) or a buffer argument (which
		 * block). The library takes it from here and judges it there, where the
		 * shader's own address space is in hand; a stream's offset is the stream
		 * array's below and an index is never both. */
		if (!(stream_mask & (1u << i))) {
			vertex_table[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_table[i].offset = work->vertexBufferOffsets[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && (stream_mask & (1u << i))) {
			vertex_streams[i].buffer = [work->vertexBuffers[i] mesaBuffer];
			vertex_streams[i].offset = work->vertexBufferOffsets[i];
			if (work->vertexBuffers[i] != nil &&
			    vertex_streams[i].buffer == NULL) {
				snprintf(why, size, "the buffer bound as vertex stream %u did "
						    "not come from the compiler library, so "
						    "there is nothing to fetch vertices out "
						    "of", i);
				[self failWith:why];
				return NO;
			}
			continue;
		}
		vertex_buffers[i] = [work->vertexBuffers[i] mesaBuffer];
	}
	if ([work->render vertexStreamCount] > RW_MAX_BUFFERS) {
		snprintf(why, size, "this pipeline fetches from %u vertex stream slots and "
				    "this driver's table holds %u", [work->render
				    vertexStreamCount], RW_MAX_BUFFERS);
		[self failWith:why];
		return NO;
	}
	for (unsigned i = 0; i < work->count; i++) {
		fragment_buffers[i] = [work->buffers[i] mesaBuffer];
		fragment_table[i].buffer = fragment_buffers[i];
		fragment_table[i].offset = work->bufferOffsets[i];
		if (work->buffers[i] != nil && fragment_buffers[i] == NULL) {
			snprintf(why, size, "the buffer bound at fragment buffer index %u did "
					    "not come from the compiler library, so there is "
					    "nothing to bind", i);
			[self failWith:why];
			return NO;
		}
	}
	for (unsigned i = 0; i < work->textureCount; i++) {
		fragment_textures[i] = [work->textures[i] mesaTexture];
	}
	for (unsigned i = 0; i < work->samplerCount; i++) {
		fragment_samplers[i] = [work->samplers[i] mesaSampler];
	}
	for (unsigned i = 0; i < work->vertexTextureCount; i++) {
		vertex_textures[i] = [work->vertexTextures[i] mesaTexture];
	}
	for (unsigned i = 0; i < work->vertexSamplerCount; i++) {
		vertex_samplers[i] = [work->vertexSamplers[i] mesaSampler];
	}
	hk_note_draw(work);
	hk_note_frame_draw(work);
	struct rwa_draw draw = {
		.color = color,
		.color_count = work->colorCount,
		.clear_color = work->clears ? work->clearColor : NULL,
		/* The depth attachment, its two load actions, the state that was bound and
		 * the depth range of the viewport -- which is where the quad's own depth
		 * comes from, the generated vertex stage putting every corner at clip z = 0
		 * (-setViewport: has the whole of it). Without a viewport the contract's
		 * default [0, 1] applies and the quad sits at 0, the near plane. */
		.depth = [work->depth mesaTexture],
		.clear_depth = work->clearsDepth ? &work->clearDepth : NULL,
		.clear_stencil = work->clearsStencil ? &work->clearStencil : NULL,
		.depth_stencil = [work->depthStencilState mesaDepthStencil],
		.stencil_reference = { work->stencilReference[0], work->stencilReference[1] },
		.depth_range = work->hasDepthRange ? work->depthRange : NULL,
		.vertex = {
			.buffers = vertex_buffers, .buffer_count = work->vertexBufferCount,
			.textures = vertex_textures, .texture_count = work->vertexTextureCount,
			.samplers = vertex_samplers, .sampler_count = work->vertexSamplerCount,
		},
		/* and the streams the layout fetches from, at Metal's own indices with
		 * Metal's own offsets; the count is what the library needs covered, which
		 * is the highest index the pipeline fetches from plus one. */
		.vertex_streams = vertex_streams,
		.vertex_stream_count = [work->render vertexStreamCount],
		.fragment = {
			.buffers = fragment_buffers, .buffer_count = work->count,
			.textures = fragment_textures, .texture_count = work->textureCount,
			.samplers = fragment_samplers, .sampler_count = work->samplerCount,
		},
		.primitive = work->primitive,
		.vertex_start = work->vertexStart,
		.vertex_count = work->vertexCount,
		/* The index buffer, by the same rule as every other resource here: it has to
		 * be one of ours, because what the library needs is the Gallium resource
		 * behind it and a buffer from the kernel path (or from another driver) has
		 * none. -mesaBuffer is nil for such a buffer, and nil here would be a draw
		 * the library reads as non-indexed -- the vertices in the wrong order, which
		 * is the one outcome worse than an error. So it is caught where the mapping
		 * fails and not left to the library, which has no way to tell the two apart. */
		.index_buffer = [work->indexBuffer mesaBuffer],
		.index_type = work->indexType,
		.index_buffer_offset = work->indexBufferOffset,
		.base_vertex = work->baseVertex,
		.instance_count = work->instanceCount,
		.base_instance = work->baseInstance,
		/* Metal's own table for each stage, at Metal's own indices: the offsets the
		 * arrays above have nowhere to put. The library takes an argument buffer's
		 * whole binding from here and a buffer argument's offset only, so the two
		 * readings cannot disagree -- and where both name a buffer it checks that
		 * they name the same one. */
		.vertex_buffer_table = vertex_table,
		.vertex_buffer_table_count = work->vertexBufferCount,
		.fragment_buffer_table = fragment_table,
		.fragment_buffer_table_count = work->count,
		/* The encoder's own state (M5a, air_metal.h). */
		.blend_color = work->hasBlendColor ? work->blendColor : NULL,
		.cull_mode = work->cullMode,
		.front_winding = work->frontWinding,
		.viewport = work->hasViewportRect ? work->viewportRect : NULL,
		.scissor = work->hasScissorRect ? work->scissorRect : NULL,
		.color_clears = work->attachmentClears,
		.color_clear_mask = work->attachmentClearMask,
		/* where in each attachment (M5b, air_metal.h) */
		.color_level = work->colorLevel,
		.color_slice = work->colorSlice,
		.depth_level = work->depthLevel,
		.depth_slice = work->depthSlice,
		/* one pass for many draws (M5g, air_metal.h) */
		.fresh_pass = work->freshPass,
		.dont_care_mask = work->dontCareMask,
	};

	if (work->indexBuffer != nil && draw.index_buffer == NULL) {
		snprintf(why, size, "the index buffer of this draw did not come from the "
		         "compiler library, so there is nothing to read the indices out of");
		[self failWith:why];
		return NO;
	}

	if (mesa.render_draw([work->render render], &draw, why, size) != 0) {
		rw_print("the draw failed: %s\n", why);
		[self failWith:why];
		return NO;
	}
	/*
	 * (WHAT A SYSTEM PIPELINE WAS GIVEN) Once the screen has settled, what each draw left at two
	 * places in the attachment, for a picture that is wrong: the draw that changes it is the one
	 * to read. It waits for the GPU at every draw, so only for 240 of them, and only with
	 * metal_probe in the boot-args.
	 */
	{
		static int enabled = -1;
		static unsigned probes;
		static CFAbsoluteTime first;

		if (enabled < 0) {
			char args[1024] = "";
			size_t args_size = sizeof(args) - 1;

			enabled = sysctlbyname("kern.bootargs", args, &args_size, NULL, 0) == 0 &&
				  (strstr(args, "metal_probe") != NULL || access("/hackios/share/probes/metal_probe", F_OK) == 0) &&
				  getprogname() != NULL && strcmp(getprogname(), "backboardd") == 0;
			first = CFAbsoluteTimeGetCurrent();
		}
		id target = work->colorCount > 0 ? work->color[0] : nil;

		if (enabled && target != nil && [target width] == 1125 && probes < 1500) {
			struct rwa_fence *fence = NULL;
			struct rwa_device *device = [[_queue device] mesaDevice];
			char quiet[160] = { 0 };
			uint8_t a[4] = { 0 }, b[4] = { 0 };
			const unsigned at_a[3] = { 62, 660, 0 }, at_b[3] = { 1090, 122, 0 }, one[3] = { 1, 1, 1 };

			probes++;
			if (mesa.flush != NULL && mesa.flush(device, &fence, quiet, sizeof(quiet)) == 0 && fence != NULL) {
				mesa.fence_wait(device, fence, quiet, sizeof(quiet));
				mesa.fence_release(device, fence);
			}
			int ra = mesa.texture_read(color[0], 0, 0, at_a, one, a, 4, quiet, sizeof(quiet));
			int rb = mesa.texture_read(color[0], 0, 0, at_b, one, b, 4, quiet, sizeof(quiet));
			NSString *label = [work->render label] ?: @"(none)";

			static uint8_t was[12];
			uint8_t second[4] = { 0 }, tex[4] = { 0 };
			id second_target = work->colorCount > 1 ? work->color[1] : nil;
			id t3 = work->textureCount > 3 ? work->textures[3] : nil;

			if (second_target != nil && [second_target pixelFormat] == 80) {
				mesa.texture_read(color[1], 0, 0, at_a, one, second, 4, quiet, sizeof(quiet));
			}
			if (t3 != nil && [t3 pixelFormat] == 80) {
				const unsigned mid[3] = { (unsigned)[t3 width] / 2, (unsigned)[t3 height] / 2, 0 };

				mesa.texture_read([t3 mesaTexture], 0, 0, mid, one, tex, 4, quiet, sizeof(quiet));
			}
			if (memcmp(was, a, 4) != 0 || memcmp(was + 4, b, 4) != 0 || memcmp(was + 8, second, 4) != 0) {
				memcpy(was, a, 4);
				memcpy(was + 4, b, 4);
				memcpy(was + 8, second, 4);
				rw_print("probe %s verts %u t3 %lu %lux%lu mid %u %u %u %u: att0(bgra) %u %u %u %u top %u %u %u %u att1 fmt %lu: %u %u %u %u%s%s\n",
					 [[label substringFromIndex:MIN([label length], 29u)] UTF8String], work->vertexCount,
					 (unsigned long)[t3 pixelFormat], (unsigned long)[t3 width], (unsigned long)[t3 height],
					 tex[0], tex[1], tex[2], tex[3], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
					 (unsigned long)[second_target pixelFormat], second[0], second[1], second[2], second[3],
					 ra || rb ? " read failed: " : "", ra || rb ? quiet : "");
			}
		}
	}
	return YES;
}

/* A blit, in the command buffer's order, through the compiler library (air_metal.h). */
- (BOOL)runBlit:(HKMetalWork *)work why:(char *)why size:(size_t)size
{
	struct rwa_buffer *fromBuffer = NULL, *toBuffer = NULL;
	struct rwa_texture *fromTexture = NULL, *toTexture = NULL;
	int rc = -1;

	if ([work->blitFrom respondsToSelector:@selector(mesaBuffer)]) {
		fromBuffer = [work->blitFrom mesaBuffer];
	} else if ([work->blitFrom respondsToSelector:@selector(mesaTexture)]) {
		fromTexture = [work->blitFrom mesaTexture];
	}
	if ([work->blitTo respondsToSelector:@selector(mesaBuffer)]) {
		toBuffer = [work->blitTo mesaBuffer];
	} else if ([work->blitTo respondsToSelector:@selector(mesaTexture)]) {
		toTexture = [work->blitTo mesaTexture];
	}
	switch (work->blit) {
	case RW_BLIT_BUFFER:
		rc = mesa.copy_buffer == NULL ? -2 :
		     mesa.copy_buffer(fromBuffer, work->fromOffset, toBuffer, work->toOffset,
				      work->length, why, size);
		break;
	case RW_BLIT_FILL:
		rc = mesa.fill_buffer == NULL ? -2 :
		     mesa.fill_buffer(toBuffer, work->toOffset, work->length, work->fillValue, why,
				      size);
		break;
	case RW_BLIT_TEXTURE:
		rc = mesa.copy_texture == NULL ? -2 :
		     mesa.copy_texture(fromTexture, work->fromSlice, work->fromLevel,
				       work->fromOrigin, work->blitSize, toTexture, work->toSlice,
				       work->toLevel, work->toOrigin, why, size);
		break;
	case RW_BLIT_BUFFER_TO_TEXTURE:
		rc = mesa.copy_buffer_to_texture == NULL ? -2 :
		     mesa.copy_buffer_to_texture(fromBuffer, work->fromOffset, work->rowBytes,
						 work->imageBytes, work->blitSize, toTexture,
						 work->toSlice, work->toLevel, work->toOrigin, why,
						 size);
		break;
	case RW_BLIT_TEXTURE_TO_BUFFER:
		rc = mesa.copy_texture_to_buffer == NULL ? -2 :
		     mesa.copy_texture_to_buffer(fromTexture, work->fromSlice, work->fromLevel,
						 work->fromOrigin, work->blitSize, toBuffer,
						 work->toOffset, work->rowBytes, work->imageBytes, why,
						 size);
		break;
	case RW_BLIT_MIPMAPS:
		rc = mesa.generate_mipmaps == NULL ? -2 :
		     mesa.generate_mipmaps(toTexture, why, size);
		break;
	case RW_BLIT_CLEAR:
		if (mesa.clear_color == NULL || mesa.clear_depth_stencil == NULL) {
			rc = -2;
		} else if (work->clears) {
			double rgba[4] = { work->clearColor[0], work->clearColor[1], work->clearColor[2],
					   work->clearColor[3] };

			rc = mesa.clear_color(toTexture, rgba, why, size);
		} else {
			rc = mesa.clear_depth_stencil(toTexture,
						      work->clearsDepth ? &work->clearDepth : NULL,
						      work->clearsStencil ? &work->clearStencil : NULL,
						      why, size);
		}
		break;
	case RW_BLIT_RESOLVE:
		rc = mesa.resolve_texture == NULL ? -2 :
		     mesa.resolve_texture(fromTexture, toTexture, why, size);
		break;
	}
	if (rc == -2) {
		snprintf(why, size, "this compiler library has no blits (it is older than this driver)");
	}
	if (rc != 0) {
		rw_print("the blit failed: %s\n", why);
		[self failWith:why];
		return NO;
	}
	return YES;
}

- (void)failWith:(const char *)reason
{
	_error = [[NSError errorWithDomain:@"HKMetal" code:4
				  userInfo:@{ NSLocalizedDescriptionKey:
					      [NSString stringWithUTF8String:reason] }] retain];
	_status = RW_CB_ERROR;
}

/* The fence is the whole point: a dispatch that the GPU does not finish must not read as a
 * result. air_run learned that the expensive way (docs/air.md section 5). A buffer stopped at
 * an event's wait is waited for until the rest of it has run; its signals become visible after. */
- (void)waitUntilCompleted
{
	if (_status == RW_CB_COMMITTED) {
		[_ran lock];
		while (!_hasRun) {
			[_ran wait];
		}
		[_ran unlock];
	}
	[self hkWaitFence];
	[self hkDrawBack];
	[self hkSignalsDone];
	[self hkWatchReport];
	if (hk_stats_on()) {
		[self hkWatchFrame];
		[self hkCensus];
	}
}

/*
 * (M5e, VideoToolbox's scale writes zeros: where they begin.) With the trace on, the first dispatches' textures are read back
 * when their buffer is complete: the corner, the middle and the far corner, as this processor sees them through -getBytes:.
 */
- (void)hkWatch:(HKMetalWork *)work
{
	static _Atomic(unsigned) budget;

	for (unsigned i = 0; i < work->textureCount; i++) {
		if (work->textures[i] != nil && atomic_fetch_add(&budget, 1) < 40) {
			if (_hkWatched == nil) {
				_hkWatched = [[NSMutableArray alloc] init];
			}
			[_hkWatched addObject:@[ @(i), work->textures[i] ]];
		}
	}
}

- (void)hkWatchReport
{
	NSArray *watched;

	@synchronized (self) {
		watched = _hkWatched;
		_hkWatched = nil;
	}
	for (NSArray *one in watched) {
		id t = [one objectAtIndex:1];
		const NSUInteger format = [t pixelFormat], w = [t width], h = [t height];
		const size_t bytes = format == 125 ? 16 : format == 115 ? 8 : format == 53 || format == 80 || format == 70 ? 4 : 0;
		const NSUInteger at[3][2] = { { 0, 0 }, { w / 2, h / 2 }, { w - 1, h - 1 } };
		char line[220];
		size_t n = (size_t)snprintf(line, sizeof(line), "back t%u format %lu %lux%lu:", [[one objectAtIndex:0] unsignedIntValue],
					    (unsigned long)format, (unsigned long)w, (unsigned long)h);

		for (unsigned k = 0; bytes != 0 && k < 3; k++) {
			uint32_t texel[4] = { 0 };

			[t getBytes:texel bytesPerRow:bytes fromRegion:MTLRegionMake2D(at[k][0], at[k][1], 1, 1) mipmapLevel:0];
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %08x", texel[0]);
			for (unsigned q = 1; q * 4 < bytes; q++) {
				n += (size_t)snprintf(line + n, sizeof(line) - n, ",%08x", texel[q]);
			}
		}
		rw_print("%s\n", line);
	}
	[watched release];
}

- (void)hkSignalsDone
{
	NSMutableArray *signals;

	@synchronized (self) {
		signals = _signals;
		_signals = nil;
	}
	for (HKMetalWork *work in signals) {
		[work->event hkSignalVisible:work->eventValue];
	}
	[signals release];
}

/*
 * (M5g, the owner's photos of run 213407: the blurred backdrop black over the upper left of the screen, along the diagonal, in
 * steps as wide as a target of a sixteenth of the size.) With the file frame_watch, in the window server: after a frame with a
 * small target, what its targets hold in the middle of each half of the diagonal. Where the upper left is black and the lower
 * right is not, all of the frame's draws are said, a few times. A frame on the screen for 11 ms cannot be caught by a screenshot.
 *
 * (The owner: 「applibrary這裡沒有bg」, and the Maps widget in yellow.) With the file frame_map: what every target of a frame holds, as above, whatever its shape, and
 * its colour: in the window server one frame in every 300, or, with a program's name in that file, all of that program's frames. A pixel is said as R G B A.
 */
/* (DRAWN AND PUT BACK) */
- (void)hkDrawBack
{
	if (atomic_load(&hk_draw_backs) == 0) {
		return;
	}
	id done[8];
	unsigned count = 0;

	for (HKMetalWork *work in _work) {
		for (unsigned c = 0; work->render != nil && c < work->colorCount; c++) {
			id t = work->color[c];
			bool known = t == nil || ![t isKindOfClass:[HKMetalTexture class]] || ![t hkDrawsBack];

			for (unsigned i = 0; !known && i < count; i++) {
				known = done[i] == t;
			}
			if (!known && count < 8) {
				done[count++] = t;
				[t hkPutBackSoon];
			}
		}
		/* (a plane through a texture of its own: back before the work is said to be done, whoever made it waits for that) */
		for (unsigned t = 0; work->pipeline != nil && work->render == nil && t < work->textureCount; t++) {
			id texture = work->textures[t];

			if ([texture isKindOfClass:[HKMetalTexture class]] && [texture hkWritesPlaneBack]) {
				[texture hkPutBack];
			}
			/* (newBufferWithBytesNoCopy's) */
			id over = [texture isKindOfClass:[HKMetalTexture class]] ? [texture hkOverBuffer] : nil;

			if ([over respondsToSelector:@selector(hkWrappedBack)]) {
				[over hkWrappedBack];
			}
		}
		for (unsigned b = 0; work->pipeline != nil && work->render == nil && b < work->count; b++) {
			if ([work->buffers[b] respondsToSelector:@selector(hkWrappedBack)]) {
				[work->buffers[b] hkWrappedBack];
			}
		}
		for (unsigned c = 0; work->render != nil && c < work->colorCount; c++) {
			id t = work->color[c];
			id over = [t isKindOfClass:[HKMetalTexture class]] ? [t hkOverBuffer] : nil;

			if ([over respondsToSelector:@selector(hkWrappedBack)]) {
				[over hkWrappedBack];
			}
		}
	}
}

- (void)hkCensus
{
	/*
	 * (The owner: 「地圖在widget顯示黃色」, photo at 23:45: a flat yellow, in the Maps widget and in two bars of another.) With the file census, "XxY+XxY", in the window
	 * server: for 120 frames after the Darwin notification com.hackios.census, once per pipeline, source and target, each draw that covers one of those points of a target
	 * as wide as the screen or that reads anything but plain BGRA or something yellow: its label, where it is, each source (hkCensus:), and the first vertex past its position
	 * and texture coordinates. And what the target holds at the points when the frame is done.
	 */
	enum { POINTS = 8, SEEN = 1024 };
	static int on = -1, token;
	static float points[POINTS][2];
	static unsigned point_count, seen_count, numbered;
	static struct { void *pipeline, *source, *target; } seen[SEEN];
	static int left;

	if (on < 0) {
		char text[128] = "";
		int file = open("/hackios/share/probes/census", O_RDONLY);

		on = 0;
		if (file >= 0 && strcmp(getprogname(), "backboardd") == 0) {
			ssize_t got = read(file, text, sizeof(text) - 1);
			const char *at = text;
			int used = 0;

			text[got > 0 ? got : 0] = 0;
			while (point_count < POINTS && sscanf(at, "%fx%f%n", &points[point_count][0], &points[point_count][1], &used) == 2) {
				point_count++;
				at += used;
				at += *at == '+';
			}
			on = notify_register_check("com.hackios.census", &token) == NOTIFY_STATUS_OK;
		}
		if (file >= 0) {
			close(file);
		}
	}
	if (!on) {
		return;
	}
	int changed = 0;

	notify_check(token, &changed);
	if (changed && numbered > 0) {
		left = 120;
	}
	numbered++;
	if (left <= 0) {
		return;
	}
	left--;
	id screen = nil;
	char line[900];

	for (HKMetalWork *work in _work) {
		if (work->render == nil || work->colorCount == 0) {
			continue;
		}
		id target = work->color[0];
		const bool wide = [target width] >= 1000;

		screen = wide && [target height] >= 2000 ? target : screen;

		float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
		const float *v = work->vertexBufferCount > 1 && work->vertexBuffers[1] != nil
		    ? (const float *)(const void *)((const char *)[work->vertexBuffers[1] contents] + work->vertexBufferOffsets[1]) : NULL;
		const uint16_t *indices = work->indexBuffer != nil && work->indexType == 0
		    ? (const uint16_t *)(const void *)((const char *)[work->indexBuffer contents] + work->indexBufferOffset) : NULL;

		for (unsigned i = 0; v != NULL && i < work->vertexCount && i < 64; i++) {
			const float *one = v + 12 * (indices != NULL ? (unsigned)((int)indices[i] + work->baseVertex) : i);
			const float w = one[3] != 0 ? one[3] : 1;

			lo[0] = MIN(lo[0], one[0] / w);
			lo[1] = MIN(lo[1], one[1] / w);
			hi[0] = MAX(hi[0], one[0] / w);
			hi[1] = MAX(hi[1], one[1] / w);
		}
		int covers = -1;

		for (unsigned p = 0; wide && covers < 0 && p < point_count; p++) {
			const float x = points[p][0], y = points[p][1];
			const bool in_scissor = !work->hasScissorRect || (x >= work->scissorRect[0] && x < work->scissorRect[0] + work->scissorRect[2] &&
									  y >= work->scissorRect[1] && y < work->scissorRect[1] + work->scissorRect[3]);

			if (in_scissor && x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1]) {
				covers = (int)p;
			}
		}
		id first = nil;

		for (unsigned i = 0; first == nil && i < work->textureCount; i++) {
			first = i == 9 ? nil : work->textures[i];
		}
		bool known = false;

		for (unsigned i = 0; i < seen_count && !known; i++) {
			known = seen[i].pipeline == (__bridge void *)work->render && seen[i].source == (__bridge void *)first && seen[i].target == (__bridge void *)target;
		}
		if (known || seen_count >= SEEN) {
			continue;
		}
		seen[seen_count].pipeline = (__bridge void *)work->render;
		seen[seen_count].source = (__bridge void *)first;
		seen[seen_count++].target = (__bridge void *)target;

		NSString *label = [work->render label] ?: @"?";
		size_t at = (size_t)snprintf(line, sizeof(line), "census %u %s -> %lux%lu at %.1f,%.1f..%.1f,%.1f", numbered,
					     [label hasPrefix:@"com.apple.coreanimation."] ? [[label substringFromIndex:24] UTF8String] : [label UTF8String],
					     (unsigned long)[target width], (unsigned long)[target height], (double)lo[0], (double)lo[1], (double)hi[0], (double)hi[1]);
		bool odd = false;

		for (unsigned i = 0; i < work->textureCount && at + 120 < sizeof(line); i++) {
			id t = work->textures[i];

			if (t != nil && i != 9 && [t isKindOfClass:[HKMetalTexture class]]) {
				uint8_t centre[16];

				at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u", i);
				at += [t hkCensus:line + at size:sizeof(line) - at centre:centre];

				const NSUInteger format = [t pixelFormat];
				const bool bgra = format == 80 || format == 81;

				odd = odd || !bgra || (centre[2] >= 200 && centre[1] >= 200 && centre[0] <= 60);
			}
		}
		if (covers < 0 && !odd) {
			continue;
		}
		if (covers >= 0 && at + 40 < sizeof(line)) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " COVERS %u", (unsigned)covers);
		}
		if (v != NULL && at + 90 < sizeof(line)) {
			const float *one = v + 12 * (indices != NULL ? (unsigned)((int)indices[0] + work->baseVertex) : 0);

			at += (size_t)snprintf(line + at, sizeof(line) - at, " v %.3g %.3g %.3g %.3g %.3g %.3g", (double)one[6], (double)one[7], (double)one[8], (double)one[9], (double)one[10], (double)one[11]);
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
		hk_console_line(line, MIN(at, sizeof(line) - 1));
	}
	for (unsigned p = 0; screen != nil && p < point_count && numbered % 10 == 0; p++) {
		uint8_t pixel[16] = { 0 };

		[screen getBytes:pixel bytesPerRow:16 fromRegion:MTLRegionMake2D((NSUInteger)points[p][0], (NSUInteger)points[p][1], 1, 1) mipmapLevel:0];

		const size_t at = (size_t)snprintf(line, sizeof(line), "census %u the screen's target %p at %.0f,%.0f holds %u %u %u %u (B G R A)\n", numbered, screen, (double)points[p][0],
						   (double)points[p][1], pixel[0], pixel[1], pixel[2], pixel[3]);

		hk_console_line(line, MIN(at, sizeof(line) - 1));
	}
}

- (void)hkWatchFrame
{
	static int on = -1, mapping, every = 300;
	static _Atomic(unsigned) dumps, checked, frames;

	if (on < 0) {
		char wanted[64] = "";
		int file = open("/hackios/share/probes/frame_map", O_RDONLY);

		if (file >= 0) {
			ssize_t got = read(file, wanted, sizeof(wanted) - 1);

			close(file);
			while (got > 0 && (wanted[got - 1] == '\n' || wanted[got - 1] == ' ')) {
				got--;
			}
			wanted[got > 0 ? got : 0] = 0;
			mapping = strcmp(getprogname(), wanted[0] != 0 ? wanted : "backboardd") == 0;
			every = wanted[0] != 0 ? 1 : 300;
		}
		on = mapping || (access("/hackios/share/probes/frame_watch", F_OK) == 0 && strcmp(getprogname(), "backboardd") == 0);
	}
	if (!on || (!mapping && atomic_load(&dumps) >= 10) || (mapping && (atomic_fetch_add(&frames, 1) % (unsigned)every != 0 || atomic_load(&dumps) >= 40))) {
		return;
	}
	bool small = mapping;

	for (HKMetalWork *work in _work) {
		small = small || (work->render != nil && work->colorCount > 0 && [work->color[0] width] >> work->colorLevel[0] < 400);
	}
	if (!small) {
		return;
	}
	char line[640];
	id seen[24];
	NSUInteger seen_level[24];
	unsigned seen_count = 0;
	unsigned hits = 0;
	size_t at;

	/*
	 * Every target of the frame as it stands now, whole: the part of it that is not black, divided into 16 x 16, and whether
	 * that is a triangle with a straight side along both edges of a corner. Runs 214642 and 215146 asked less of more places
	 * and were set off by a hole for a detached wallpaper and by CoreAnimation rendering into a corner of its scratch targets:
	 * this asks only about the shape, wherever it is in the target.
	 */
	for (HKMetalWork *work in _work) {
		if (work->render == nil || work->colorCount == 0 || [work->color[0] pixelFormat] < 70 || [work->color[0] pixelFormat] > 81) {
			continue;
		}
		id t = work->color[0];
		const NSUInteger level = work->colorLevel[0];
		bool known = false;

		for (unsigned i = 0; i < seen_count; i++) {
			known = known || (seen[i] == t && seen_level[i] == level);
		}
		if (known || seen_count >= 24) {
			continue;
		}
		seen[seen_count] = t;
		seen_level[seen_count++] = level;

		const NSUInteger w = MAX([t width] >> level, 1), h = MAX([t height] >> level, 1);
		uint8_t *pixels = malloc(w * h * 4);

		if (pixels == NULL) {
			continue;
		}
		[t getBytes:pixels bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level];

		NSUInteger x0 = w, x1 = 0, y0 = h, y1 = 0;

		for (NSUInteger y = 0; y < h; y++) {
			const uint8_t *row = pixels + y * w * 4;

			for (NSUInteger x = 0; x < w; x++) {
				if (row[x * 4] >= 3 || row[x * 4 + 1] >= 3 || row[x * 4 + 2] >= 3) {
					x0 = MIN(x0, x);
					x1 = MAX(x1, x);
					y0 = MIN(y0, y);
					y1 = MAX(y1, y);
				}
			}
		}
		enum { G = 16 };
		bool cell[G][G] = { { false } };
		const char *which = NULL;
		char colour[80] = "";

		if (mapping && w > 0 && h > 0) {
			const uint8_t *middle = pixels + (h / 2) * w * 4 + (w / 2) * 4, *corner = pixels;
			const bool bgr = [t pixelFormat] == 80 || [t pixelFormat] == 81;

			snprintf(colour, sizeof(colour), " format %lu middle %u %u %u %u corner %u %u %u %u", (unsigned long)[t pixelFormat], middle[bgr ? 2 : 0], middle[1], middle[bgr ? 0 : 2],
				 middle[3], corner[bgr ? 2 : 0], corner[1], corner[bgr ? 0 : 2], corner[3]);
		}

		if (x1 > x0 + G && y1 > y0 + G) {
			for (unsigned r = 0; r < G; r++) {
				for (unsigned c = 0; c < G; c++) {
					const uint8_t *q = pixels + (y0 + (y1 - y0) * (2 * r + 1) / (2 * G)) * w * 4 + (x0 + (x1 - x0) * (2 * c + 1) / (2 * G)) * 4;

					cell[r][c] = q[0] >= 3 || q[1] >= 3 || q[2] >= 3;
				}
			}
			static const char *const names[4] = { "lower-left triangle", "lower-right triangle", "upper-left triangle", "upper-right triangle" };

			for (unsigned flip = 0; flip < 4 && which == NULL; flip++) {
				unsigned last = 0, first_count = 0;
				bool fits = true;

				for (unsigned r = 0; r < G && fits; r++) {
					unsigned count = 0, strays = 0;
					const unsigned rr = (flip & 2) ? G - 1 - r : r;

					for (unsigned c = 0; c < G; c++) {
						const bool here = cell[rr][(flip & 1) ? G - 1 - c : c];

						if (here && count == c) {
							count++;
						} else if (here) {
							strays++;
						}
					}
					fits = strays <= 1 && count + 1 >= last;
					last = MAX(last, count);
					first_count = r == 0 ? count : first_count;
				}
				if (fits && first_count <= 4 && last >= G - 4) {
					which = names[flip];
				}
			}
		}
		free(pixels);
		if (mapping && which == NULL) {
			which = "map";
		}
		if (which != NULL) {
			at = (size_t)snprintf(line, sizeof(line), "watch %u: %p %lux%lu[%lu] %s%s, the part at %lu,%lu %lux%lu:", atomic_load(&dumps), t, (unsigned long)w, (unsigned long)h,
					      (unsigned long)level, which, colour, (unsigned long)x0, (unsigned long)y0, (unsigned long)(x1 - x0 + 1), (unsigned long)(y1 - y0 + 1));
			for (unsigned r = 0; r < G && at + G + 4 < sizeof(line); r++) {
				line[at++] = ' ';
				for (unsigned c = 0; c < G; c++) {
					line[at++] = cell[r][c] ? '#' : '.';
				}
			}
			at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
			hk_console_line(line, MIN(at, sizeof(line) - 1));
			hits++;
		}
	}
	atomic_fetch_add(&checked, 1);
	if (hits == 0) {
		return;
	}
	unsigned number = atomic_fetch_add(&dumps, 1), index = 0;

	at = (size_t)snprintf(line, sizeof(line), "watch %u: %u targets of this shape, the %u'th frame looked at\n", number, hits, atomic_load(&checked));
	hk_console_line(line, MIN(at, sizeof(line) - 1));
	for (HKMetalWork *work in _work) {
		index++;
		if (work->blit != 0) {
			at = (size_t)snprintf(line, sizeof(line), "watch %u/%u blit %u to %lux%lu\n", number, index, work->blit,
					      (unsigned long)[work->blitTo width], (unsigned long)[work->blitTo height]);
			hk_console_line(line, MIN(at, sizeof(line) - 1));
			continue;
		}
		if (work->render == nil) {
			continue;
		}
		NSString *label = [work->render label] ?: @"?";

		at = (size_t)snprintf(line, sizeof(line), "watch %u/%u %s -> %lux%lu[%u] +%u %lux%lu prim %u v%u+%u i%u idx %d cull %u wind %u clear %u/%x dc %x fresh %d",
				      number, index, [label hasPrefix:@"com.apple.coreanimation."] ? [[label substringFromIndex:24] UTF8String] : [label UTF8String],
				      (unsigned long)[work->color[0] width], (unsigned long)[work->color[0] height], work->colorLevel[0], work->colorCount - 1,
				      (unsigned long)[work->color[1] width], (unsigned long)[work->color[1] height],
				      work->primitive, work->vertexStart, work->vertexCount, work->instanceCount, work->indexBuffer != nil,
				      work->cullMode, work->frontWinding, work->clears, work->attachmentClearMask, work->dontCareMask, work->freshPass);
		if (work->hasViewportRect && at + 50 < sizeof(line)) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " vp %g,%g %gx%g", (double)work->viewportRect[0], (double)work->viewportRect[1],
					       (double)work->viewportRect[2], (double)work->viewportRect[3]);
		}
		if (work->hasScissorRect && at + 40 < sizeof(line)) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " sc %u,%u %ux%u", work->scissorRect[0], work->scissorRect[1],
					       work->scissorRect[2], work->scissorRect[3]);
		}
		for (unsigned i = 0; i < work->textureCount && at + 40 < sizeof(line); i++) {
			if (work->textures[i] != nil && mapping && [work->textures[i] isKindOfClass:[HKMetalTexture class]]) {
				uint8_t centre[16];

				at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u", i);
				at += [work->textures[i] hkCensus:line + at size:sizeof(line) - at centre:centre];
			} else if (work->textures[i] != nil) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u %lux%lu/%lu", i, (unsigned long)[work->textures[i] width],
						       (unsigned long)[work->textures[i] height], (unsigned long)[work->textures[i] mipmapLevelCount]);
			}
		}
		if (mapping && work->clears && at + 60 < sizeof(line)) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " clears to %.3g %.3g %.3g %.3g", (double)work->clearColor[0], (double)work->clearColor[1],
					       (double)work->clearColor[2], (double)work->clearColor[3]);
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
		hk_console_line(line, MIN(at, sizeof(line) - 1));
		if (mapping && strcmp(getprogname(), "backboardd") != 0 && work->vertexBufferCount > 0 && work->vertexBuffers[0] != nil) {
			/* (a program of its own vertex format: the first of what its draw reads, as it is) */
			const float *f = (const float *)(const void *)((const char *)[work->vertexBuffers[0] contents] + work->vertexBufferOffsets[0]);

			at = (size_t)snprintf(line, sizeof(line), "watch %u/%u   buffer 0:", number, index);
			for (unsigned i = 0; i < 24 && at + 20 < sizeof(line); i++) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %.4g", (double)f[i]);
			}
			at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
			hk_console_line(line, MIN(at, sizeof(line) - 1));
		}
		if (work->vertexBufferCount > 1 && work->vertexBuffers[1] != nil && work->vertexCount <= 6) {
			const float *v = (const float *)(const void *)((const char *)[work->vertexBuffers[1] contents] + work->vertexBufferOffsets[1]);
			const uint16_t *indices = work->indexBuffer != nil && work->indexType == 0
			    ? (const uint16_t *)(const void *)((const char *)[work->indexBuffer contents] + work->indexBufferOffset) : NULL;

			at = (size_t)snprintf(line, sizeof(line), "watch %u/%u   at %zu ix %zu base %d:", number, index, work->vertexBufferOffsets[1],
					      work->indexBufferOffset, work->baseVertex);
			for (unsigned i = 0; indices != NULL && i < work->vertexCount; i++) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %u", indices[i]);
			}
			for (unsigned i = 0; i < 4 && i < work->vertexCount && at + 60 < sizeof(line); i++) {
				const float *one = v + 12 * (i + (indices != NULL ? work->baseVertex : 0));

				at += (size_t)snprintf(line + at, sizeof(line) - at, " (%.1f %.1f %.2f %.2f t %.3f %.3f)", (double)one[0], (double)one[1], (double)one[2],
						       (double)one[3], (double)one[4], (double)one[5]);
			}
			at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
			hk_console_line(line, MIN(at, sizeof(line) - 1));
		}
	}
}

/* (M5g) A command buffer that took long, from its commit to the GPU's end: it held up everybody on the one ring. What was in it. */
- (void)hkSaySlow:(uint64_t)took
{
	static _Atomic(unsigned) said;
	char line[420];
	size_t at;
	unsigned draws = 0, dispatches = 0, blits = 0, shown = 0;

	if (atomic_fetch_add(&said, 1) >= 40) {
		return;
	}
	for (HKMetalWork *work in _work) {
		draws += work->render != nil;
		blits += work->blit != 0;
		dispatches += work->pipeline != nil && work->render == nil && work->blit == 0;
	}
	at = (size_t)snprintf(line, sizeof(line), "hkmetal[%s %d]: %llu us from commit to done: %u draws, %u dispatches, %u blits:", getprogname(), getpid(),
			      (unsigned long long)took, draws, dispatches, blits);
	for (HKMetalWork *work in _work) {
		if (work->blit != 0 && at + 60 < sizeof(line)) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " blit kind %u to %lux%lu", work->blit, (unsigned long)[work->blitTo width],
					       (unsigned long)[work->blitTo height]);
		}
		if (work->pipeline == nil || work->render != nil || work->blit != 0 || shown++ >= 3 || at + 100 >= sizeof(line)) {
			continue;
		}
		at += (size_t)snprintf(line + at, sizeof(line) - at, " %s groups %lux%lu", [[[work->pipeline label] description] UTF8String] ?: "(no label)",
				       (unsigned long)work->groups[0], (unsigned long)work->groups[1]);
		for (unsigned i = 0; i < work->textureCount && at + 40 < sizeof(line); i++) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " t%u %lux%lu/%lu", i, (unsigned long)[work->textures[i] width],
					       (unsigned long)[work->textures[i] height], (unsigned long)[work->textures[i] pixelFormat]);
		}
	}
	at += (size_t)snprintf(line + at, sizeof(line) - at, "\n");
	hk_console_line(line, MIN(at, sizeof(line) - 1));
}

- (void)hkWaitFence
{
	@synchronized (self) {
		char why[512] = { 0 };

		if (_status == RW_CB_ERROR || _status == RW_CB_COMPLETED) {
			return;
		}
		struct rwa_device *device = [[_queue device] mesaDevice];
		int rc;

		if (_fence != NULL) {
			rc = mesa.fence_wait(device, _fence, why, sizeof(why));
		} else {
			pthread_mutex_lock(&rw_pipe_lock);
			rc = mesa.finish(device, why, sizeof(why));
			pthread_mutex_unlock(&rw_pipe_lock);
		}
		if (rc != 0) {
			rw_print("THE GPU DID NOT FINISH: %s\n", why);
			_error = [[NSError errorWithDomain:@"HKMetal" code:3
						  userInfo:@{ NSLocalizedDescriptionKey:
							      [NSString stringWithUTF8String:why] }] retain];
			_status = RW_CB_ERROR;
			return;
		}
		_status = RW_CB_COMPLETED;   /* the fence has been paid, and only now */
		if (_committedAt != 0) {
			const uint64_t took = hk_now_us() - _committedAt;

			atomic_fetch_add(&hk_stat.done, 1);
			atomic_fetch_add(&hk_stat.done_us, took);
			hk_stat_most(&hk_stat.done_longest, took);
			if (took > 50000 && hk_stats_on()) {
				[self hkSaySlow:took];
			}
		}
		if (_bytesChunks != nil) {
			[(HKMetalDevice *)[_queue device] hkGiveBackBytesChunks:_bytesChunks];
			[_bytesChunks release];
			_bytesChunks = nil;
		}
		/* ... and given back now, not at -dealloc. Nothing asks a completed buffer's fence
		 * anything again (the test above returns first), and the back end keeps the
		 * submission's command buffer with it: run 020900's --draw-all held 208 of each
		 * until its autorelease pool drained, which an application that keeps finished
		 * command buffers would hold for as long as it keeps them. A failed wait keeps
		 * its fence: the GPU may still be reading that command buffer, and -dealloc gives
		 * it back as before. Whatever is ever read OUT of the fence (GPUStartTime and
		 * GPUEndTime, when they come: M4) is read here, before this, into the buffer's own
		 * fields. */
		if (_fence != NULL) {
			mesa.fence_release(device, _fence);
			_fence = NULL;
		}
	}
}

- (void)waitUntilScheduled { }

/*
 * HANDLERS, AND WHERE THEY RUN.
 *
 * Both of these used to call the block the moment it was added -- which is before the work
 * the block is about, on whichever thread encoded it. That was defensible while the only
 * caller was this port's own test; it is not once backboardd is the caller, and it is the
 * other half of why the system sends -[MTLCommandQueueSPI setCompletionQueue:]: a daemon
 * says WHERE its completion handlers may run precisely because it must not have them on its
 * own thread.
 *
 * So handlers are kept and delivered where the work happens: -commit. A scheduled handler
 * is delivered as soon as the recorded work has been submitted, a completed handler after
 * the GPU fence has been paid for. A background waiter advances status and keeps the recorded
 * resources alive even when nobody registered a handler. A handler added after commit waits
 * for that same fence; scheduled is not completed.
 */
- (void)hkDeliver:(void (^)(id))handler
{
	dispatch_queue_t queue = [_queue hkCompletionQueue];

	if (queue != nil) {
		/* dispatch_async copies the block, which retains both this buffer and the
		 * handler it carries, so the buffer outlives its own completion. */
		dispatch_async(queue, ^{ handler(self); });
		return;
	}
	handler(self);
}

- (void)addScheduledHandler:(void (^)(id))handler
{
	if (handler == NULL) {
		return;
	}
	/* scheduled or further, an error included (3, 4, 5): the work this handler is about has
	 * already run, so it is delivered rather than kept */
	if (_status >= RW_CB_SCHEDULED) {
		[self hkDeliver:handler];
		return;
	}
	if (_scheduled == nil) {
		_scheduled = [[NSMutableArray alloc] init];
	}
	[_scheduled addObject:[[handler copy] autorelease]];
}

- (void)addCompletedHandler:(void (^)(id))handler
{
	NSUInteger status = _status;

	if (handler == NULL) {
		return;
	}
	if (status >= RW_CB_COMPLETED) {
		[self hkDeliver:handler];
		return;
	}
	if (status == RW_CB_SCHEDULED) {
		/* A handler registered after commit still owes the same GPU fence. */
		dispatch_async(hk_fence_queue(), ^{
			[self waitUntilCompleted];
			[self hkDeliver:handler];
		});
		return;
	}
	if (_completed == nil) {
		_completed = [[NSMutableArray alloc] init];
	}
	[_completed addObject:[[handler copy] autorelease]];
}

/* Called from the one place the work is done. The order is Metal's: scheduled first, then
 * the fence, then completed. */
- (void)hkDeliverHandlers
{
	NSArray *scheduled = _scheduled;
	NSArray *completed = _completed;

	_scheduled = nil;
	_completed = nil;
	for (id handler in scheduled) {
		[self hkDeliver:(void (^)(id))handler];
	}
	if (_status == RW_CB_SCHEDULED) {
		/* The fence is waited for off the committing thread, so that a renderer goes on
		 * encoding the next frame while the GPU draws this one. One serial queue: fences
		 * of one ring complete in order, so waiting in order costs nothing. */
		/* Even without handlers, keep recorded resources alive and advance status. */
		[self retain];
		dispatch_async(hk_fence_queue(), ^{
			[self waitUntilCompleted];
			for (id handler in completed) {
				[self hkDeliver:(void (^)(id))handler];
			}
			[completed release];
			[self release];
		});
		[scheduled release];
		return;
	}
	if ([completed count] > 0 && _status != RW_CB_ERROR) {
		/* A completed handler is a promise about the GPU, so it is made true before it
		 * is kept: -waitUntilCompleted is this driver's fence and sets the status the
		 * handler will read. */
		[self waitUntilCompleted];
	}
	for (id handler in completed) {
		[self hkDeliver:(void (^)(id))handler];
	}
	[scheduled release];
	[completed release];
	[self hkSignalsDone];   /* (a failed buffer: there is no fence to wait for) */
}

/* -commitWithDeadline: is an abstract entry of _MTLCommandBuffer, so inheriting it aborts
 * rather than defaulting. The deadline is for work that is worthless if it is late (a frame
 * that misses its vsync); Apple's drivers hand it to the GPU's scheduler. There is one
 * scheduler here and one priority, so the deadline cannot be honoured -- but the COMMIT
 * must happen, and a deadline that is merely unmet is a late frame while a commit that
 * never happens is a daemon waiting for ever. */
- (void)commitWithDeadline:(NSUInteger)deadline
{
	(void)deadline;
	RW_ONCE("a command buffer was committed with a deadline; this driver "
		"has one ring and no scheduler to tell, so the work is committed and the "
		"deadline is not promised\n");
	[self commit];
}

- (void)addPurgedResource:(id)resource { (void)resource; [self hkPurgedNote]; }
- (void)addPurgedHeap:(id)heap         { (void)heap; [self hkPurgedNote]; }

/* Metal records on a command buffer every resource of its that was purged while the buffer
 * referenced it, so that -commit can fail the buffer instead of reading pages that are
 * gone. Nothing of this driver's is ever volatile -- -setPurgeableState: answers
 * MTLPurgeableStateNonVolatile for every resource, which is what they are -- so a report
 * here means Metal and this driver disagree about a resource, and that is worth a line
 * rather than a silent buffer that fails later for no stated reason. */
- (void)hkPurgedNote
{
	RW_ONCE("Metal reported a purged resource on a command buffer, but "
		"nothing this driver allocates is volatile; the report is noted and the "
		"buffer is not failed\n");
}


@end

@implementation HKMetalRenderEncoder {
	HKMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder */
	HKMetalRenderPipeline *_pipeline;
	id _color[8];
	unsigned _colorCount;
	float _clearColor[4];
	BOOL _clears;
	/* every attachment's own load action and clear colour, for a pass with no draw (the
	 * draw contract clears with one colour, _clearColor, the first attachment's) */
	BOOL _clearsAttachment[8];
	float _attachmentClear[8][4];
	id _resolveTo[8];               /* each attachment's resolveTexture, if it asks for a resolve */
	/* M5b: where in each attachment, and where in its resolve texture: a level, and a slice or
	 * a depth plane (Metal keeps two numbers, of which a type uses one) */
	unsigned _colorLevel[8], _colorSlice[8];
	unsigned _resolveLevel[8], _resolveSlice[8];
	unsigned _depthLevel, _depthSlice;
	BOOL _drew;                     /* a draw has been recorded: it carried the pass's clears */
	BOOL _barrierPending;           /* M5g: a barrier of this encoder's stands before the next draw */
	unsigned _dontCare;             /* M5g: the load actions MTLLoadActionDontCare, for the first draw */
	id _depth;
	id _depthStencilState;
	float _clearDepth;
	unsigned _clearStencil;
	BOOL _clearsDepth, _clearsStencil;
	unsigned _stencilReference[2];
	float _depthRange[2];
	BOOL _hasDepthRange;
	/* the encoder's own state (M5a): Metal's defaults until a setter says otherwise, which
	 * is a zero, a NO, or MTLWindingClockwise and MTLCullModeNone, whose numbers are zero */
	float _blendColor[4];
	BOOL _hasBlendColor;
	unsigned _cullMode, _frontWinding;
	float _viewportRect[4];
	BOOL _hasViewportRect;
	unsigned _scissorRect[4];
	BOOL _hasScissorRect;
	id _vertexBuffers[RW_MAX_BUFFERS];
	NSUInteger _vertexBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentBuffers[RW_MAX_BUFFERS];
	NSUInteger _fragmentBufferOffsets[RW_MAX_BUFFERS];
	id _fragmentTextures[RW_MAX_BUFFERS];
	id _vertexTextures[RW_MAX_BUFFERS];       /* the vertex stage's, as the library has always taken them */
	id _vertexSamplers[RW_MAX_BUFFERS];
	unsigned _vertexTextureCount, _vertexSamplerCount;
	id _fragmentSamplers[RW_MAX_BUFFERS];
	unsigned _vertexBufferCount, _fragmentBufferCount;
	unsigned _fragmentTextureCount, _fragmentSamplerCount;
	BOOL _complained;
}

- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer
				  descriptor:(MTLRenderPassDescriptor *)descriptor
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;

	/* What the pass is drawing into, and whether it begins by clearing. Metal keeps this in
	 * the pass descriptor rather than in the pipeline, so it belongs to the encoder. */
	for (unsigned i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *attachment =
		    [[descriptor colorAttachments] objectAtIndexedSubscript:i];
		id texture = attachment != nil ? [attachment texture] : nil;

		if (texture == nil) {
			continue;
		}
		_color[i] = texture;
		_colorCount = i + 1;
		/* M5b: where in it. The Vulkan library renders there; the Gallium one renders
		 * level 0, slice 0 of a 2D texture whatever this says, and that is said here
		 * rather than read as a picture of the wrong level. */
		_colorLevel[i] = (unsigned)[attachment level];
		_colorSlice[i] = (unsigned)([attachment slice] + [attachment depthPlane]);
		_resolveLevel[i] = (unsigned)[attachment resolveLevel];
		_resolveSlice[i] = (unsigned)([attachment resolveSlice] +
					      [attachment resolveDepthPlane]);
		if (!mesa_is_vulkan && (_colorLevel[i] != 0 || _colorSlice[i] != 0)) {
			RW_ONCE("a pass renders into level %u, slice %u of an attachment, and the "
				"Gallium library renders into level 0 and slice 0\n",
				_colorLevel[i], _colorSlice[i]);
		}
		/*
		 * A resolve is the one thing a multisample pass asks for that this driver
		 * cannot do, and it is said out loud rather than ignored. The compiler
		 * library makes multisample attachments and renders into them
		 * (docs/metal.md 10.34), but it stores what it has and resolves nothing --
		 * so for an application the one-sample texture it means to READ is the
		 * resolveTexture, and that one would come back holding whatever it held
		 * before. Silence here would read as a pass that drew nothing. The same
		 * goes for MTLStoreActionMultisampleResolve on the depth attachment, which
		 * nothing has asked for yet.
		 */
		NSUInteger store = [attachment storeAction];

		/* a resolve is carried out at -endEncoding, after the pass's draws (RW_BLIT_RESOLVE) */
		if ((store == MTLStoreActionMultisampleResolve ||
		     store == MTLStoreActionStoreAndMultisampleResolve) &&
		    [attachment resolveTexture] != nil) {
			_resolveTo[i] = [attachment resolveTexture];
		}
		if ([attachment loadAction] == MTLLoadActionDontCare) {
			_dontCare |= 1u << i;
		}
		if ([attachment loadAction] == MTLLoadActionClear) {
			MTLClearColor c = [attachment clearColor];

			_clearsAttachment[i] = YES;
			_attachmentClear[i][0] = (float)c.red;
			_attachmentClear[i][1] = (float)c.green;
			_attachmentClear[i][2] = (float)c.blue;
			_attachmentClear[i][3] = (float)c.alpha;
		}
		if (i == 0 && [attachment loadAction] == MTLLoadActionClear) {
			MTLClearColor c = [attachment clearColor];

			_clearColor[0] = (float)c.red;
			_clearColor[1] = (float)c.green;
			_clearColor[2] = (float)c.blue;
			_clearColor[3] = (float)c.alpha;
			_clears = YES;
		}
	}
	/*
	 * The depth/stencil attachment, which is two descriptors over one texture: for
	 * MTLPixelFormatDepth32Float_Stencil8 an application sets the same texture on
	 * both, and for a depth-only format it sets only the first. So whichever of the
	 * two carries a texture is the attachment, and both carrying DIFFERENT textures is
	 * refused out loud -- a Gallium framebuffer has one zsbuf, and quietly taking the
	 * depth one would drop the stencil half of what was asked for.
	 *
	 * Each aspect keeps its own load action, because the one struct Metal has behind
	 * both descriptors holds clearDepth and clearStencil separately
	 * (MTLRenderPassAttachmentDescriptorPrivate) and a pass that clears the depth and
	 * loads the stencil is an ordinary pass.
	 */
	MTLRenderPassDepthAttachmentDescriptor *za = [descriptor depthAttachment];
	MTLRenderPassStencilAttachmentDescriptor *sa = [descriptor stencilAttachment];
	id ztex = [za texture];
	id stex = [sa texture];

	if (ztex != nil && stex != nil && ztex != stex) {
		rw_print("the depth attachment and the stencil attachment are two "
		         "different textures; one texture carries both aspects here, so this "
		         "pass is encoded with neither\n");
	} else {
		_depth = ztex != nil ? ztex : stex;
		/* M5b: where in it, from whichever descriptor carries it */
		if (ztex != nil) {
			_depthLevel = (unsigned)[za level];
			_depthSlice = (unsigned)([za slice] + [za depthPlane]);
		} else if (stex != nil) {
			_depthLevel = (unsigned)[sa level];
			_depthSlice = (unsigned)([sa slice] + [sa depthPlane]);
		}
		if (ztex != nil && [za loadAction] == MTLLoadActionDontCare) {
			_dontCare |= RWA_DONT_CARE_DEPTH;
		}
		if (stex != nil && [sa loadAction] == MTLLoadActionDontCare) {
			_dontCare |= RWA_DONT_CARE_STENCIL;
		}
		if (ztex != nil && [za loadAction] == MTLLoadActionClear) {
			_clearDepth = (float)[za clearDepth];
			_clearsDepth = YES;
		}
		if (stex != nil && [sa loadAction] == MTLLoadActionClear) {
			_clearStencil = (unsigned)[sa clearStencil];
			_clearsStencil = YES;
		}
	}
	/* M5b: layered rendering, [[render_target_array_index]] over this many slices, is in neither
	 * library and no shader of the corpus writes that index */
	if ([descriptor renderTargetArrayLength] > 1) {
		RW_ONCE("a pass asks for %lu layers of rendering (renderTargetArrayLength): every "
			"draw goes to the one slice each attachment names\n",
			(unsigned long)[descriptor renderTargetArrayLength]);
	}
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setRenderPipelineState:(id)state { _pipeline = state; }

/*
 * One table, two meanings, and the offset is no longer thrown away.
 *
 * Metal keeps vertex streams and the vertex stage's [[buffer(n)]] arguments in the SAME
 * table, and which an index is depends on the pipeline's vertex descriptor -- which may
 * not even be set yet when this is called. So the offset is RECORDED here and judged at
 * the draw: for a stream it is Metal's own `offset:` and goes to the library, and for a
 * buffer argument it is still unimplemented and the draw says so by name, having the
 * pipeline's own mask to tell the two apart. Dropping it here is what would draw the
 * wrong mesh out of a buffer that holds several.
 */
- (void)setVertexBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("vertex buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_vertexBuffers[index] = buffer;
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/*
 * And the fragment stage's table, which is now the same shape as the vertex one: the
 * offset is RECORDED and judged at the draw rather than refused here.
 *
 * It used to be refused on the spot, because the contract bound whole buffers and
 * ignoring an offset would read the wrong bytes quietly. An ARGUMENT BUFFER is bound
 * through this very call with an offset of its own -- Metal lets one allocation hold many
 * encodings and the offset is which one -- and whether index n is a set or a plain buffer
 * argument is the pipeline's answer and may not be known yet when this is called. So the
 * draw decides: a set takes the offset, a buffer argument with a non-zero offset is still
 * refused there, by name.
 */
- (void)setFragmentBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("fragment buffer %lu is past this driver's limit of %u\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		_complained = YES;
		return;
	}
	_fragmentBuffers[index] = buffer;
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	NSUInteger offset = 0;
	id buffer = [_commandBuffer hkBytes:bytes length:length offset:&offset];

	if (buffer != nil) {
		[self setVertexBuffer:buffer offset:offset atIndex:index];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	NSUInteger offset = 0;
	id buffer = [_commandBuffer hkBytes:bytes length:length offset:&offset];

	if (buffer != nil) {
		[self setFragmentBuffer:buffer offset:offset atIndex:index];
	}
}

- (void)setFragmentTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	if ([texture isKindOfClass:[HKMetalTexture class]]) {
		[texture hkFollowSurface];
	}
	_fragmentTextures[index] = texture;
	if (index + 1 > _fragmentTextureCount) {
		_fragmentTextureCount = (unsigned)index + 1;
	}
}

- (void)setFragmentSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentSamplers[index] = sampler;
	if (index + 1 > _fragmentSamplerCount) {
		_fragmentSamplerCount = (unsigned)index + 1;
	}
}

/*
 * The viewport, of which exactly one half is served and the other half is declined, and the
 * split is the contract's: rwa_draw carries znear and zfar and carries no rectangle, because
 * the library's draw always covers the whole attachment.
 *
 * The depth range half is not a convenience either. It is the only thing in Metal that moves
 * a quad in z through this driver -- the generated vertex stage puts all four corners at
 * clip z = 0, and Metal's NDC z starts at 0 on the near plane, so the window depth of what
 * is drawn is znear exactly. Without it every draw here would be at depth 0 and no depth
 * test could ever be measured on the device.
 *
 * The rectangle half says so once, as it did before: a viewport silently ignored draws the
 * right triangle in the wrong place.
 */
- (void)setViewport:(MTLViewport)viewport
{
	id first = _colorCount > 0 ? _color[0] : _depth;
	NSUInteger w = [first width], h = [first height];

	/* [M5a: on the Vulkan library the rectangle is real and goes with the draw (rwa_draw.viewport);
	 * this sentence is about the Gallium library, whose draw still covers the attachment.] */
	if ((viewport.originX != 0.0 || viewport.originY != 0.0 ||
	     (NSUInteger)viewport.width != w || (NSUInteger)viewport.height != h) &&
	    !_complained && !mesa_is_vulkan) {
		rw_print("a viewport rectangle is not implemented (asked for "
		         "%gx%g at %g,%g of a %lux%lu attachment); the draw covers the whole "
		         "attachment\n", viewport.width, viewport.height, viewport.originX,
		         viewport.originY, (unsigned long)w, (unsigned long)h);
		_complained = YES;
	}
	_viewportRect[0] = (float)viewport.originX;
	_viewportRect[1] = (float)viewport.originY;
	_viewportRect[2] = (float)viewport.width;
	_viewportRect[3] = (float)viewport.height;
	_hasViewportRect = YES;
	_depthRange[0] = (float)viewport.znear;
	_depthRange[1] = (float)viewport.zfar;
	_hasDepthRange = YES;
}
/* [M5a: these three were `{ (void)rect; }` and went nowhere. They are recorded now, as the
 * depth range is, and go with each draw; the Gallium library reads none of them, so a draw there
 * is what it always was, and the Vulkan one honours them (air_vk.c, THE DRAW PATH). The numbers
 * are the contract's 32 bits, a rectangle past them being one no attachment has. */
- (void)setScissorRect:(MTLScissorRect)rect
{
	_scissorRect[0] = rect.x > UINT32_MAX ? UINT32_MAX : (unsigned)rect.x;
	_scissorRect[1] = rect.y > UINT32_MAX ? UINT32_MAX : (unsigned)rect.y;
	_scissorRect[2] = rect.width > UINT32_MAX ? UINT32_MAX : (unsigned)rect.width;
	_scissorRect[3] = rect.height > UINT32_MAX ? UINT32_MAX : (unsigned)rect.height;
	_hasScissorRect = YES;
}
- (void)setCullMode:(NSUInteger)mode                  { _cullMode = (unsigned)mode; }
- (void)setFrontFacingWinding:(NSUInteger)winding     { _frontWinding = (unsigned)winding; }
- (void)setTriangleFillMode:(NSUInteger)mode          { (void)mode; }

/* The depth/stencil state, recorded like the pipeline state: nothing runs at encode time,
 * so what a draw tests against is whatever was bound when that draw was encoded. A state
 * from another driver -- or nil, which is Metal's "no test, no write" -- is passed on as
 * nil, and the library draws with its own zeroed state, which is the same thing. */
- (void)setDepthStencilState:(id)state
{
	if (state != nil && ![state isKindOfClass:[HKMetalDepthStencilState class]]) {
		rw_print("this depth/stencil state is a %s and not one of ours, so "
		         "it cannot be bound; this pass draws with no depth test\n",
		         class_getName(object_getClass(state)));
		state = nil;
	}
	_depthStencilState = state;
}
/* [M5a: recorded and sent with each draw, for the Vulkan library's vkCmdSetBlendConstants. The
 * Gallium library refuses the factors that read it when the pipeline is made, as it always has,
 * so what it draws does not change.] */
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	_blendColor[0] = r;
	_blendColor[1] = g;
	_blendColor[2] = b;
	_blendColor[3] = a;
	_hasBlendColor = YES;
}

- (void)recordDraw:(NSUInteger)primitive
       vertexStart:(NSUInteger)start
	     count:(NSUInteger)count
       indexBuffer:(id)indexBuffer
	 indexType:(NSUInteger)indexType
       indexOffset:(NSUInteger)indexOffset
	baseVertex:(NSInteger)baseVertex
     instanceCount:(NSUInteger)instances
      baseInstance:(NSUInteger)baseInstance
{
	HKMetalWork *work = [[HKMetalWork alloc] init];

	if (_pipeline == nil || (_colorCount == 0 && _depth == nil)) {
		rw_print("this draw has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "nothing to draw into");
		[work release];
		return;
	}
	/*
	 * What the contract carries in 32 bits and Metal hands over in 64. These are not
	 * limits of this driver's choosing: `index_bias` in Gallium is a signed int and
	 * instance counts there are unsigned, so a number that does not fit is a number
	 * that would be recorded as a DIFFERENT number -- a base vertex of 2^32 as 0, and
	 * an instance count of 2^32 + 3 as 3. Saying so is the only answer that is not a
	 * wrong picture.
	 */
	if (baseVertex < INT32_MIN || baseVertex > INT32_MAX || instances > UINT32_MAX ||
	    baseInstance > UINT32_MAX || count > UINT32_MAX || start > UINT32_MAX) {
		rw_print("this draw does not fit the contract's 32 bits (count %lu, "
		         "start %lu, base vertex %ld, %lu instances from %lu) and is dropped "
		         "rather than wrapped\n", (unsigned long)count, (unsigned long)start,
		         (long)baseVertex, (unsigned long)instances, (unsigned long)baseInstance);
		[work release];
		return;
	}
	/* Said at the draw and not at the pass, because it is the pipeline that knows:
	 * Metal drops a [[depth]] write when there is no depth attachment, and so does
	 * this -- but a caller measuring such a shader would otherwise be left reading a
	 * colour attachment that holds its clear value and nothing else. */
	if ([_pipeline writesDepth] && _depth == nil && !_complained) {
		rw_print("this pipeline's fragment shader stores to [[depth]] and "
		         "this pass has no depth attachment, so those writes go nowhere\n");
		_complained = YES;
	}
	work->render = [_pipeline retain];
	work->colorCount = _colorCount;
	for (unsigned i = 0; i < _colorCount; i++) {
		work->color[i] = [_color[i] retain];
		work->colorLevel[i] = _colorLevel[i];
		work->colorSlice[i] = _colorSlice[i];
	}
	work->depthLevel = _depthLevel;
	work->depthSlice = _depthSlice;
	work->freshPass = _barrierPending;
	work->dontCareMask = _drew ? 0 : _dontCare;
	_barrierPending = NO;
	work->clears = _clears;
	for (unsigned i = 0; i < 4; i++) {
		work->clearColor[i] = _clearColor[i];
	}
	work->depth = [_depth retain];
	work->depthStencilState = [_depthStencilState retain];
	work->clearDepth = _clearDepth;
	work->clearStencil = _clearStencil;
	work->clearsDepth = _clearsDepth;
	work->clearsStencil = _clearsStencil;
	work->stencilReference[0] = _stencilReference[0];
	work->stencilReference[1] = _stencilReference[1];
	work->depthRange[0] = _depthRange[0];
	work->depthRange[1] = _depthRange[1];
	work->hasDepthRange = _hasDepthRange;
	/* The encoder's own state as it stands at this draw (M5a), and each attachment's own
	 * clear, for the first draw of the pass: the library takes these and not the one colour
	 * below where it can. The first draw carries the load actions, as the comment at the
	 * end of this method says. */
	memcpy(work->blendColor, _blendColor, sizeof(work->blendColor));
	work->hasBlendColor = _hasBlendColor;
	work->cullMode = _cullMode;
	work->frontWinding = _frontWinding;
	memcpy(work->viewportRect, _viewportRect, sizeof(work->viewportRect));
	work->hasViewportRect = _hasViewportRect;
	memcpy(work->scissorRect, _scissorRect, sizeof(work->scissorRect));
	work->hasScissorRect = _hasScissorRect;
	for (unsigned i = 0; i < _colorCount && !_drew; i++) {
		if (_clearsAttachment[i]) {
			work->attachmentClearMask |= 1u << i;
			memcpy(work->attachmentClears[i], _attachmentClear[i],
			       sizeof(work->attachmentClears[i]));
		}
	}
	work->primitive = (unsigned)primitive;
	work->vertexStart = (unsigned)start;
	work->vertexCount = (unsigned)count;
	work->indexBuffer = [indexBuffer retain];
	work->indexType = (unsigned)indexType;
	work->indexBufferOffset = indexOffset;
	work->baseVertex = (int)baseVertex;
	work->instanceCount = (unsigned)instances;
	work->baseInstance = (unsigned)baseInstance;
	work->vertexBufferCount = _vertexBufferCount;
	for (unsigned i = 0; i < _vertexBufferCount; i++) {
		work->vertexBuffers[i] = [_vertexBuffers[i] retain];
		work->vertexBufferOffsets[i] = (size_t)_vertexBufferOffsets[i];
	}
	work->count = _fragmentBufferCount;
	for (unsigned i = 0; i < _fragmentBufferCount; i++) {
		work->buffers[i] = [_fragmentBuffers[i] retain];
		work->bufferOffsets[i] = (size_t)_fragmentBufferOffsets[i];
	}
	work->textureCount = _fragmentTextureCount;
	for (unsigned i = 0; i < _fragmentTextureCount; i++) {
		work->textures[i] = [_fragmentTextures[i] retain];
	}
	work->samplerCount = _fragmentSamplerCount;
	for (unsigned i = 0; i < _fragmentSamplerCount; i++) {
		work->samplers[i] = [_fragmentSamplers[i] retain];
	}
	work->vertexTextureCount = _vertexTextureCount;
	for (unsigned i = 0; i < _vertexTextureCount; i++) {
		work->vertexTextures[i] = [_vertexTextures[i] retain];
	}
	work->vertexSamplerCount = _vertexSamplerCount;
	for (unsigned i = 0; i < _vertexSamplerCount; i++) {
		work->vertexSamplers[i] = [_vertexSamplers[i] retain];
	}
	[_commandBuffer recordWork:work];
	[work release];
	/* The load action happens once, at the start of the pass: the first draw carried it and
	 * the ones after it load what the first one left. */
	_clears = _clearsDepth = _clearsStencil = NO;
	_drew = YES;
}

/*
 * M5b: one level and one slice of an attachment, as what a call that takes neither is given: the
 * clear and the resolve at either end of a pass. A texture that has only the one is itself, which
 * is every pass before this, and the only thing the Gallium library can do; there, asking for
 * another is said once and goes to level 0, where its draw would have gone too.
 */
- (id)hkOnly:(id)texture level:(unsigned)level slice:(unsigned)slice
{
	if (level == 0 && slice == 0 && [texture mipmapLevelCount] == 1 && [texture hkSlices] == 1 &&
	    [texture textureType] != MTLTextureType3D) {
		return texture;
	}
	id window = [texture hkWindowLevel:level slice:slice];

	if (window == nil) {
		RW_ONCE("a pass's clear or resolve is of level %u, slice %u, and this compiler "
			"library has no view to say that with: level 0 of the whole texture\n",
			level, slice);
		return texture;
	}
	return window;
}

/* -endEncoding's half of the pass: the clears of a pass that drew nothing, then the resolves. */
- (void)recordPassEnd
{
	if (!_drew) {
		for (unsigned i = 0; i < _colorCount; i++) {
			if (!_clearsAttachment[i] || _color[i] == nil) {
				continue;
			}
			HKMetalWork *work = [[[HKMetalWork alloc] init] autorelease];

			work->blit = RW_BLIT_CLEAR;
			work->blitTo = [[self hkOnly:_color[i] level:_colorLevel[i]
					       slice:_colorSlice[i]] retain];
			work->clears = YES;
			memcpy(work->clearColor, _attachmentClear[i], sizeof(work->clearColor));
			[_commandBuffer recordWork:work];
		}
		if (_depth != nil && (_clearsDepth || _clearsStencil)) {
			HKMetalWork *work = [[[HKMetalWork alloc] init] autorelease];

			work->blit = RW_BLIT_CLEAR;
			work->blitTo = [[self hkOnly:_depth level:_depthLevel
					       slice:_depthSlice] retain];
			work->clearsDepth = _clearsDepth;
			work->clearsStencil = _clearsStencil;
			work->clearDepth = _clearDepth;
			work->clearStencil = _clearStencil;
			[_commandBuffer recordWork:work];
		}
		_drew = YES;    /* (once, should -endEncoding come twice) */
	}
	for (unsigned i = 0; i < 8; i++) {
		if (_resolveTo[i] == nil || _color[i] == nil) {
			continue;
		}
		HKMetalWork *work = [[[HKMetalWork alloc] init] autorelease];

		work->blit = RW_BLIT_RESOLVE;
		work->blitFrom = [[self hkOnly:_color[i] level:_colorLevel[i]
					 slice:_colorSlice[i]] retain];
		work->blitTo = [[self hkOnly:_resolveTo[i] level:_resolveLevel[i]
				       slice:_resolveSlice[i]] retain];
		[_commandBuffer recordWork:work];
		_resolveTo[i] = nil;
	}
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:1 baseInstance:0];
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:0];
}

/*
 * The rest of what MTLRenderCommandEncoder requires. Each is here for the reason the
 * device's own 44 were: a selector that is absent aborts the caller, and an application's
 * encoder sends a good many of these without thinking about it.
 *
 * The division is deliberate. A form that can be served by the single-binding path forwards
 * to it. A form that cannot be served -- the INDIRECT draws, whose whole argument list is
 * read out of a buffer the GPU may be writing -- DECLINES out loud, because drawing the
 * wrong primitives is worse than drawing none. A form that is merely state we do not vary
 * yet (store actions: we always store; residency: everything is resident) is a no-op, and
 * that is said here rather than guessed at by the next reader.
 */
- (void)setVertexBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

- (void)setFragmentBuffers:(const id *)buffers
		   offsets:(const NSUInteger *)offsets
		 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}

/* The offset alone, which an application changes between draws out of one buffer. It is
 * the same table entry -setVertexBuffer:offset:atIndex: filled, so it is the same field:
 * for a stream the library honours it, and for a buffer argument the draw says it cannot
 * (it has the pipeline's mask and this method has not). */
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_vertexBufferOffsets[index] = offset;
	if (index + 1 > _vertexBufferCount) {
		_vertexBufferCount = (unsigned)index + 1;
	}
}

/* The same for the fragment stage, and now with the same answer as the vertex one: the
 * offset is recorded against the index already bound there, and the draw judges it. An
 * application that moves an argument buffer's offset between draws -- which is the
 * ordinary way to walk a table of encodings -- is doing nothing this driver has to
 * refuse. */
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_fragmentBufferOffsets[index] = offset;
	if (index + 1 > _fragmentBufferCount) {
		_fragmentBufferCount = (unsigned)index + 1;
	}
}

/*
 * A vertex stage's textures. These were dropped here, and on a map with its tiles VectorKit's ground, icons and labels read
 * theirs in the vertex stage: "the draw failed: DaVinci::ground_base_vertex reads texture 0 and 0 textures were bound", a
 * failed command buffer and no frame (the owner on Maps: 「有時候還會黑色」; run 084652). A blank grid never drew those.
 */
- (void)setVertexTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	if ([texture isKindOfClass:[HKMetalTexture class]]) {
		[texture hkFollowSurface];
	}
	_vertexTextures[index] = texture;
	if (index + 1 > _vertexTextureCount) {
		_vertexTextureCount = (unsigned)index + 1;
	}
}
- (void)setVertexTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexTexture:textures[i] atIndex:range.location + i];
	}
}
- (void)setVertexSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		return;
	}
	_vertexSamplers[index] = sampler;
	if (index + 1 > _vertexSamplerCount) {
		_vertexSamplerCount = (unsigned)index + 1;
	}
}
- (void)setVertexSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setVertexSamplerState:sampler atIndex:index];
}
- (void)setVertexSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] atIndex:range.location + i];
	}
}
- (void)setFragmentTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentTexture:textures[i] atIndex:range.location + i];
	}
}
- (void)setFragmentSamplerState:(id)sampler
		    lodMinClamp:(float)lodMin
		    lodMaxClamp:(float)lodMax
			atIndex:(NSUInteger)index
{
	(void)lodMin; (void)lodMax;
	[self setFragmentSamplerState:sampler atIndex:index];
}
- (void)setFragmentSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
	}
}

- (void)drawPrimitives:(NSUInteger)primitive
	   vertexStart:(NSUInteger)start
	   vertexCount:(NSUInteger)count
	 instanceCount:(NSUInteger)instances
	  baseInstance:(NSUInteger)base
{
	[self recordDraw:primitive vertexStart:start count:count indexBuffer:nil indexType:0
	     indexOffset:0 baseVertex:0 instanceCount:instances baseInstance:base];
}

/*
 * Indexed drawing, which is what every drawing call in a real application is. All three
 * direct forms are one call here, because that is what they are in Metal: the shorter two
 * are the longest with instanceCount 1, baseVertex 0 and baseInstance 0, and Metal's own
 * header documents them that way (the parameter lists are identical as far as they go).
 *
 * Nothing is checked here except what the LIBRARY cannot check: that there is an index
 * buffer at all, and that it is one of ours (its Gallium resource is what the draw needs,
 * and a buffer allocated on the kernel path has none -- -mesaBuffer is nil for it, and
 * nil in the draw would read as "not indexed", which is the vertices in the wrong order).
 * The index type, the offset against the index size, the count against the buffer's
 * length and the indices against the quad are all refused BY NAME in air_metal.c, and
 * that refusal reaches the application as the command buffer's NSError, with the numbers
 * in it. Two readers of the same rule is how the two drift apart.
 */
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
		   baseVertex:(NSInteger)baseVertex
		 baseInstance:(NSUInteger)baseInstance
{
	if (buffer == nil) {
		rw_print("an indexed draw with no index buffer is refused rather "
		         "than drawn as if the indices were 0, 1, 2, ...\n");
		return;
	}
	[self recordDraw:primitive vertexStart:0 count:count indexBuffer:buffer
		indexType:type indexOffset:offset baseVertex:baseVertex
	    instanceCount:instances baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset instanceCount:1
			 baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		   indexCount:(NSUInteger)count
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
		instanceCount:(NSUInteger)instances
{
	[self drawIndexedPrimitives:primitive indexCount:count indexType:type
			indexBuffer:buffer indexBufferOffset:offset
		      instanceCount:instances baseVertex:0 baseInstance:0];
}

/*
 * The INDIRECT forms, which were absent from this class altogether until now -- and
 * absent is the one answer that aborts the application, with our class's name on the
 * selector (10.1). They are the fourth -drawIndexedPrimitives: overload and the second
 * -drawPrimitives:, and what they ask for is the draw's whole argument list out of a
 * buffer: MTLDrawPrimitivesIndirectArguments and MTLDrawIndexedPrimitivesIndirect-
 * Arguments, five 32-bit words at an offset.
 *
 * They decline, and the reason is not that the words are hard to read -- the processor
 * can read them, this buffer is mapped. It is that the point of an indirect draw is a
 * count the GPU writes, and this library validates an indexed draw ON THE PROCESSOR
 * before it makes it: the index count against the buffer's length, the indices against
 * the quad. A count read here would be the count at encode time and not the one the GPU
 * will have written, so what came out would be a picture of the wrong number of
 * primitives -- and it would read as a result.
 *
 * Nothing in Metal makes this unreachable for an application, so this says so out loud
 * rather than drawing nothing silently.
 */
- (void)drawPrimitives:(NSUInteger)primitive
	indirectBuffer:(id)indirectBuffer
  indirectBufferOffset:(NSUInteger)offset
{
	rw_print("an indirect draw is not implemented (the argument list would "
	         "be read on the processor at encode time, and the GPU may not have written it "
	         "yet), so this draw is refused rather than drawn from the wrong count\n");
}
- (void)drawIndexedPrimitives:(NSUInteger)primitive
		    indexType:(NSUInteger)type
		  indexBuffer:(id)buffer
	    indexBufferOffset:(NSUInteger)offset
	       indirectBuffer:(id)indirectBuffer
	 indirectBufferOffset:(NSUInteger)indirectOffset
{
	[self drawPrimitives:primitive indirectBuffer:indirectBuffer
	       indirectBufferOffset:indirectOffset];
}

/* Raster state the contract does not carry yet. Collected here with one line each so that
 * the next person can see at a glance what the draw ignores. */
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count
{
	if (count > 1 && !_complained) {
		rw_print("%lu viewports are not implemented; the first one is "
		         "taken\n", (unsigned long)count);
		_complained = YES;
	}
	if (count >= 1 && viewports != NULL) {
		[self setViewport:viewports[0]];
	}
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count
{
	/* [M5a: the first of them, as -setViewports: takes the first viewport] */
	if (count >= 1 && rects != NULL) {
		[self setScissorRect:rects[0]];
	}
}
/* MTLDepthClipModeClip is what the library's rasterizer state does (depth_clip_near and
 * _far are both on); MTLDepthClipModeClamp, which keeps a primitive outside the depth range
 * and clamps its z instead, is a different picture and the contract has nowhere to ask for
 * it. Said once rather than ignored. */
- (void)setDepthClipMode:(NSUInteger)mode
{
	if (mode != MTLDepthClipModeClip && !_complained) {
		rw_print("MTLDepthClipModeClamp is not implemented; this pass clips "
		         "against the depth range instead of clamping to it\n");
		_complained = YES;
	}
}
- (void)setDepthBias:(float)bias slopeScale:(float)scale clamp:(float)clamp
{
	/* Neither back end carries it yet (M5d), and an application would otherwise draw wrong with no way to find out
	 * why: said once a process, and only when it asks for something other than none. */
	static int said;

	if ((bias != 0.0f || scale != 0.0f) && !__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) {
		rw_print("setDepthBias:%g slopeScale:%g clamp:%g is not carried to the GPU: depth bias is dropped\n",
		    (double)bias, (double)scale, (double)clamp);
	}
}
/* The stencil reference, which Metal keeps on the encoder and not in the state object, so
 * it is recorded with the draw like the clear colour. */
- (void)setStencilReferenceValue:(uint32_t)value
{
	_stencilReference[0] = value;
	_stencilReference[1] = value;
}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back
{
	_stencilReference[0] = front;
	_stencilReference[1] = back;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset
{
	(void)mode; (void)offset;
}
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const void *)mappings
{
	(void)count; (void)mappings;
}

/*
 * We always store what was drawn, so these record nothing; saying it here is cheaper than
 * leaving the next reader to wonder whether a discarded attachment would be honoured.
 *
 * That is now also true of the depth attachment, and there it is more than tidiness: the
 * only thing MTLStoreActionDontCare permits that storing does not is leaving the contents
 * undefined, so keeping them is a superset of every store action Metal has. A pass whose
 * depth the next pass reads back therefore works whichever action it asked for -- and the
 * depth test inside one pass never depended on this at all.
 */
- (void)setColorStoreAction:(NSUInteger)action atIndex:(NSUInteger)index
{
	(void)action; (void)index;
}
- (void)setDepthStoreAction:(NSUInteger)action                 { (void)action; }
- (void)setStencilStoreAction:(NSUInteger)action               { (void)action; }
- (void)setColorStoreActionOptions:(NSUInteger)options atIndex:(NSUInteger)index
{
	(void)options; (void)index;
}
- (void)setDepthStoreActionOptions:(NSUInteger)options         { (void)options; }
- (void)setStencilStoreActionOptions:(NSUInteger)options       { (void)options; }

/* Residency: every allocation here is resident for its lifetime, so there is nothing to
 * make resident. */
/*
 * RESIDENCY, which on this driver is nothing to arrange, and that is worth saying rather
 * than implementing in silence.
 *
 * In Metal these calls exist because an argument buffer holds raw handles: the GPU reads
 * them, so the resources they name have to be made resident separately. Here every member
 * of every set is bound explicitly from the encoder's record (air_metal.h), so a resource
 * that is encoded is bound and a resource that is not encoded is a refusal at the draw --
 * there is no third state for residency to be about. What the application still owes is
 * what Metal makes it owe: keeping the resource alive, and this driver turns a resource
 * destroyed after it was encoded into a sentence rather than a read of freed memory.
 */
- (void)useResource:(id)resource usage:(NSUInteger)usage stages:(NSUInteger)stages
{
	(void)resource; (void)usage; (void)stages;
}
- (void)useResource:(id)resource usage:(NSUInteger)usage
{
	(void)resource; (void)usage;
	RW_ONCE("-useResource: is a no-op here: every member of an argument "
		"buffer is bound explicitly from what the encoder recorded, so there is no "
		"residency to arrange and no handle for the GPU to follow\n");
}
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)useResources:(const id *)resources
	       count:(NSUInteger)count
	       usage:(NSUInteger)usage
	      stages:(NSUInteger)stages
{
	(void)resources; (void)count; (void)usage; (void)stages;
}
- (void)useHeap:(id)heap
{
	(void)heap;
	RW_ONCE("-useHeap: is a no-op here, for -useResource:'s reason, and "
		"this driver has no heaps to make resident in any case\n");
}
- (void)useHeap:(id)heap stages:(NSUInteger)stages             { (void)heap; (void)stages; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count     { (void)heaps; (void)count; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count stages:(NSUInteger)stages
{
	(void)heaps; (void)count; (void)stages;
}

/*
 * THE REST OF WHAT <MTLRenderCommandEncoder> AND <MTLRenderCommandEncoderSPI> REQUIRE.
 *
 * 125 + 34 required methods between them, read out of the cache rather than the runtime
 * (MTLRenderCommandEncoderSPI is declared in AGXMetalA11.bundle, so no process on this
 * phone has it; tools/metal/owes.py does the subtraction). 48 of them had no answer
 * anywhere in this chain before this section, and an answer is the difference between a
 * daemon that keeps running and one launchd restarts for ever.
 *
 * Each one either serves, or refuses and says so once. Nothing here claims.
 */

/* Served, by forwarding: Metal's own shorthands for what this encoder already does. */
- (void)setFragmentTexture:(id)texture
	   atTextureIndex:(NSUInteger)textureIndex
	     samplerState:(id)sampler
	   atSamplerIndex:(NSUInteger)samplerIndex
{
	[self setFragmentTexture:texture atIndex:textureIndex];
	[self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setFragmentSamplerStates:(const id *)samplers
		    lodMinClamps:(const float *)lodMin
		    lodMaxClamps:(const float *)lodMax
		       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setFragmentSamplerState:samplers[i] lodMinClamp:lodMin[i]
				  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
- (void)setVertexSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setVertexSamplerState:samplers[i] lodMinClamp:lodMin[i]
				lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}
/* The SPI spelling of -setDepthClipMode:, which already says what it cannot do. */
- (void)setDepthClipModeSPI:(NSUInteger)mode { [self setDepthClipMode:mode]; }

/*
 * Served by construction, which is a different thing from ignored.
 *
 * A barrier and a fence both say "do not start this until that has finished". Everything
 * this driver submits goes to the kernel's single msm ring in the order it was committed,
 * and a command buffer's recorded work runs inside one -commit; there is no second queue
 * and no reordering, so the ordering a barrier asks for is already the only ordering there
 * is. If that ever stops being true -- a second ring, or work submitted from -endEncoding
 * -- these stop being correct, which is why it is written down here and not assumed.
 */
/*
 * [M5g: and in the render encoder they are no longer nothing. The library's draws used to be a
 * Vulkan pass each with a barrier of their own; now they share a pass (air_vk.c, ONE PASS FOR
 * MANY DRAWS), and the next draw after one of these starts a new pass, which has the barrier.]
 */
- (void)memoryBarrierWithScope:(NSUInteger)scope
		   afterStages:(NSUInteger)after
		  beforeStages:(NSUInteger)before
{
	(void)scope; (void)after; (void)before;
	_barrierPending = YES;
}
- (void)memoryBarrierWithResources:(const id *)resources
			     count:(NSUInteger)count
		       afterStages:(NSUInteger)after
		      beforeStages:(NSUInteger)before
{
	(void)resources; (void)count; (void)after; (void)before;
	_barrierPending = YES;
}
- (void)textureBarrier { _barrierPending = YES; }
- (void)updateFence:(id)fence afterStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}
- (void)waitForFence:(id)fence beforeStages:(NSUInteger)stages
{
	(void)fence; (void)stages;
}

/* Answers about this pass, and they are answers rather than placeholders: the attachments
 * are real textures in real memory (nothing memoryless), and there is no tile pipeline at
 * all, so there is no tile size to report. Zero is the only honest number for a feature
 * that does not exist -- a plausible 32x32 would be a claim, and the device already answers
 * no to every tile-shading query, so a caller that asks this has already been told. */
- (BOOL)isMemorylessRender { return NO; }
- (NSUInteger)tileWidth    { return 0; }
- (NSUInteger)tileHeight   { return 0; }

/* Refused, each with its name in the log once.
 *
 * Tessellation: the library has no tessellator, so a patch draw records nothing. Said out
 * loud because a draw that silently does nothing is the one failure this driver has already
 * paid for twice (docs/metal.md 10.21, 10.26). */
- (void)hkNoTessellation
{
	RW_ONCE("tessellation is not implemented; this patch draw records "
		"nothing\n");
}
- (void)drawPatches:(NSUInteger)controlPoints
	 patchStart:(NSUInteger)start
	 patchCount:(NSUInteger)count
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
      instanceCount:(NSUInteger)instances
       baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)instances; (void)baseInstance;
	[self hkNoTessellation];
}
- (void)drawPatches:(NSUInteger)controlPoints
   patchIndexBuffer:(id)indexBuffer
patchIndexBufferOffset:(NSUInteger)offset
     indirectBuffer:(id)indirectBuffer
indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)indirectBuffer;
	(void)indirectOffset;
	[self hkNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
		patchStart:(NSUInteger)start
		patchCount:(NSUInteger)count
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	     instanceCount:(NSUInteger)instances
	      baseInstance:(NSUInteger)baseInstance
{
	(void)controlPoints; (void)start; (void)count; (void)indexBuffer; (void)offset;
	(void)controlPointBuffer; (void)controlPointOffset; (void)instances;
	(void)baseInstance;
	[self hkNoTessellation];
}
- (void)drawIndexedPatches:(NSUInteger)controlPoints
	  patchIndexBuffer:(id)indexBuffer
    patchIndexBufferOffset:(NSUInteger)offset
   controlPointIndexBuffer:(id)controlPointBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointOffset
	    indirectBuffer:(id)indirectBuffer
      indirectBufferOffset:(NSUInteger)indirectOffset
{
	(void)controlPoints; (void)indexBuffer; (void)offset; (void)controlPointBuffer;
	(void)controlPointOffset; (void)indirectBuffer; (void)indirectOffset;
	[self hkNoTessellation];
}
- (void)setTessellationFactorBuffer:(id)buffer
			     offset:(NSUInteger)offset
		     instanceStride:(NSUInteger)stride
{
	(void)buffer; (void)offset; (void)stride;
	[self hkNoTessellation];
}
- (void)setTessellationFactorScale:(float)scale
{
	(void)scale;
	[self hkNoTessellation];
}

/* Indirect command buffers: a buffer of commands the GPU reads for itself. The device
 * answers no to every indirect-command-buffer query, so this is a path a correct caller
 * does not take; the draws in it would be lost silently if it did. */
- (void)hkNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self hkNoIndirectCommands];
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	[self hkNoIndirectCommands];
}

/* Tile shading and imageblocks: a whole second pipeline kind (a tile function that runs over
 * the pass's imageblock), which this driver does not have. Every binding of it is refused
 * together, because one of them answering and the rest not would be worse than all of them
 * refusing: a tile dispatch would then run with half its bindings. */
- (void)hkNoTileShading
{
	RW_ONCE("tile shading and imageblocks are not implemented; this tile "
		"binding or dispatch is dropped\n");
}
- (void)dispatchThreadsPerTile:(MTLSize)size
{
	(void)size;
	[self hkNoTileShading];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length
			    offset:(NSUInteger)offset
			   atIndex:(NSUInteger)index
{
	(void)length; (void)offset; (void)index;
	[self hkNoTileShading];
}
- (void)setTileBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hkNoTileShading];
}
- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hkNoTileShading];
}
- (void)setTileBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hkNoTileShading];
}
- (void)setTileBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hkNoTileShading];
}
- (void)setTileTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hkNoTileShading];
}
- (void)setTileTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hkNoTileShading];
}
- (void)setTileSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hkNoTileShading];
}
- (void)setTileSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hkNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hkNoTileShading];
}
- (void)setTileSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hkNoTileShading];
}

/* Function tables: pointers to shader functions, which an AIR module of ours never names
 * and the device answers no to (no ray tracing, no function pointers). A table bound here
 * would be a table a shader cannot call. */
- (void)hkNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVertexVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setVertexVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setFragmentVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setTileVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setTileVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setVertexIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setFragmentIntersectionFunctionTables:(const id *)tables
			      withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setTileIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setTileIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}

/* A line width other than 1 would change the picture, so it is said; the rasterizer this
 * driver drives has one width. */
- (void)setLineWidth:(float)width
{
	if (width != 1.0f) {
		RW_ONCE("a line width of %g is not implemented; lines are one "
			"pixel wide\n", (double)width);
	}
}

/* Vertex amplification: one vertex shader run writing several positions (what stereo
 * rendering uses). The device answers 1 to -supportsVertexAmplificationCount:, so the only
 * mode a correct caller sets is the one that amplifies nothing. */
- (void)setVertexAmplificationMode:(NSUInteger)mode value:(NSUInteger)value
{
	if (value > 1) {
		RW_ONCE("vertex amplification to %lu views is not implemented; "
			"this pass draws one\n", (unsigned long)value);
	}
	(void)mode;
}

/* Counter sampling: the device has no counter sets (every query answers nil or zero), so
 * there is no sample buffer this could write into. */
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
}

/*
 * MESH AND OBJECT STAGE BINDINGS, AND WHY THEY ARE HERE AT ALL.
 *
 * These 26 are not missing from the chain: _MTLCommandEncoder has an entry for every one of
 * them. Each entry is one of Apple's ABSTRACT methods -- a body that calls
 * doesNotRecognizeSelector: -- and that is the trap docs/metal.md 10.1 was written about:
 * `respondsToSelector:` answers YES, the method list lists it, and the abort when it is
 * called names the RECEIVER, so the log would read "-[HKMetalRenderEncoder
 * setMeshTexture:atIndex:]: unrecognized selector" and send the next reader looking for a
 * method that is not the problem. An entry in a method table is not behaviour, and the only
 * way to tell these apart from working methods is to read the bodies out of the cache
 * (tools/metal/protometh.py --verdicts, which found exactly 28 of them under this driver's
 * classes: these, -commitWithDeadline: and -setThreadgroupPackingDisabled:).
 *
 * A mesh pipeline is two stages before the fragment one -- an object stage that decides how
 * much geometry to make and a mesh stage that makes it. This driver compiles a vertex and a
 * fragment function and nothing else, and the device answers no when asked whether mesh
 * shaders are supported, so there is no stage for any of this to bind to. They are refused
 * together for the same reason the tile bindings are: half of a stage's bindings arriving
 * would be worse than none.
 */
- (void)hkNoMeshStages
{
	RW_ONCE("mesh and object shader stages are not implemented (the device "
		"answers no to them); this binding is dropped\n");
}
- (void)setObjectBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectBuffers:(const id *)buffers
		 offsets:(const NSUInteger *)offsets
	       withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hkNoMeshStages];
}
- (void)setObjectBytes:(const void *)bytes
		length:(NSUInteger)length
	       atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hkNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectSamplerState:(id)sampler
		  lodMinClamp:(float)lodMin
		  lodMaxClamp:(float)lodMax
		      atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hkNoMeshStages];
}
- (void)setObjectSamplerStates:(const id *)samplers
		  lodMinClamps:(const float *)lodMin
		  lodMaxClamps:(const float *)lodMax
		     withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hkNoMeshStages];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoMeshStages];
}
- (void)setObjectIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)buffer; (void)offset; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	(void)offset; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshBuffers:(const id *)buffers
	       offsets:(const NSUInteger *)offsets
	     withRange:(NSRange)range
{
	(void)buffers; (void)offsets; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)bytes; (void)length; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshTexture:(id)texture atIndex:(NSUInteger)index
{
	(void)texture; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshTextures:(const id *)textures withRange:(NSRange)range
{
	(void)textures; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	(void)sampler; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshSamplerState:(id)sampler
		lodMinClamp:(float)lodMin
		lodMaxClamp:(float)lodMax
		    atIndex:(NSUInteger)index
{
	(void)sampler; (void)lodMin; (void)lodMax; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	(void)samplers; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshSamplerStates:(const id *)samplers
		lodMinClamps:(const float *)lodMin
		lodMaxClamps:(const float *)lodMax
		   withRange:(NSRange)range
{
	(void)samplers; (void)lodMin; (void)lodMax; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoMeshStages];
}
- (void)setMeshIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoMeshStages];
}
/*
 * The last four of the mesh group are OPTIONAL members of <MTLRenderCommandEncoderSPI>, and
 * optional is the worse case rather than the lighter one: an optional method is the one
 * Apple asks about with respondsToSelector: before sending it, and an abstract entry in
 * _MTLCommandEncoder makes that question answer YES. So the probe passes, the call goes
 * out, and the abort names our class for a method we were never asked to have. Overriding
 * them turns the yes into a yes that is true.
 */
- (void)setMeshIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoMeshStages];
}
- (void)setMeshAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self hkNoMeshStages];
}
- (void)setObjectAccelerationStructure:(id)structure atBufferIndex:(NSUInteger)index
{
	(void)structure; (void)index;
	[self hkNoMeshStages];
}

/* A split handler is called when Metal splits one render pass into several submissions,
 * which this driver never does: a pass is one draw list and one submission. */
- (void)addSplitHandler:(void (^)(id, NSUInteger, NSUInteger))handler
{
	(void)handler;
	RW_ONCE("a render-pass split handler was installed; this driver never "
		"splits a pass, so it will not be called\n");
}

/*
 * Ending a pass is Apple's bookkeeping, and this override used to skip it.
 * `_MTLCommandEncoder` RETAINS the command buffer in -initWithCommandBuffer: -- the
 * designated initialiser this driver sends to super -- and releases it in exactly one place,
 * the tail of its own -endEncoding (0x181a1f4a8: `objc_release(_commandBuffer);
 * _commandBuffer = nil`). An override that does nothing leaves that ivar set, and Apple's
 * -dealloc checks it:
 *
 *   0x181a1e5f4  cbz x8, +124      ; _commandBuffer nil -> nothing to say
 *   0x181a1e610  MTLReportFailure(0, "-[_MTLCommandEncoder dealloc]", 131,
 *                                 @"Command encoder released without endEncoding")
 *
 * and MTLReportFailure here is an abort(). That is the whole of logs/214841-run.log: that
 * line 86 times and 43 aborts of backboardd, one every ten seconds, all from
 * CA::OGL::MetalContext::warmup_shaders -- which for each attachment combination it warms
 * opens a render encoder, ends it immediately, and pops the autorelease pool it made it in
 * (0x188476304, 0x188476308, then the pop whose return address is +1064). So every warmed
 * pass cost one abort. It also leaked a retain on every command buffer any encoder touched,
 * which is the quieter half of the same mistake.
 *
 * So the superclass is called, and what that runs for us is measured rather than hoped --
 * every step is Apple's own code, and the long middle of the function is skipped:
 *
 *   [_commandBuffer executeSynchronizationNotifications:1]   real; our list is empty
 *   [_commandBuffer setCurrentCommandEncoder:nil]            real; an ivar write
 *   _isProgressTrackingEncoder is 0, so no short cut through, and then
 *   [_commandBuffer errorOptions] with bit 0 clear jumps straight to the tail
 *   objc_release(_commandBuffer); _commandBuffer = nil
 *
 * Calling it twice is safe: the second time every message goes to a nil ivar and the release
 * is a release of nil. The one branch not taken is framework-assisted error tracking
 * (`errorOptions` bit 0, MTLCommandBufferErrorOptionEncoderExecutionStatus): it would build
 * an `_MTLCommandBufferEncoderInfo` and ask this encoder -getDriverUniqueID, which is a
 * one-instruction forward to -globalTraceObjectID. Nothing here turns that option on, and if
 * something ever does, that selector is where to look.
 *
 * What this deliberately does NOT do is seal the encoder. Metal calls encoding after
 * -endEncoding undefined, and this driver would still record it; refusing that is separate
 * work and nothing has asked for it.
 */
- (void)endEncoding
{
	[self recordPassEnd];
	[super endEncoding];
}

@end

@implementation HKMetalComputeEncoder {
	HKMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
	HKMetalComputePipeline *_pipeline;
	id _buffers[RW_MAX_BUFFERS];
	NSUInteger _offsets[RW_MAX_BUFFERS];
	id _textures[RW_MAX_BUFFERS];
	id _samplers[RW_MAX_BUFFERS];
	unsigned _highest, _highestTexture, _highestSampler;
	BOOL _complained;
}

- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer
{
	/* the initialiser IOGPUMetalCommandEncoder sends to super (0x1ec171d14 -> 0x181a1ee6c) */
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (void)setComputePipelineState:(id)state
{
	_pipeline = state;
}

- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* The offset is RECORDED and judged at the dispatch, as the render encoder's is.
	 * An argument buffer is bound through this call with an offset of its own -- which
	 * encoding in that buffer to read -- and a plain buffer argument with one is an
	 * application packing several blocks into an allocation, which is served: the
	 * library takes a `constant` binding at any offset (uploading the block when the
	 * GPU cannot fetch from that offset) and refuses a `device` one the GPU cannot
	 * fetch from, by name, where the shader's own address space is in hand. */
	_buffers[index] = buffer;
	_offsets[index] = offset;
	if (index + 1 > _highest) {
		_highest = (unsigned)index + 1;
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("texture index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	if ([texture isKindOfClass:[HKMetalTexture class]]) {
		[texture hkFollowSurface];
	}
	_textures[index] = texture;
	if (index + 1 > _highestTexture) {
		_highestTexture = (unsigned)index + 1;
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("sampler index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	_samplers[index] = sampler;
	if (index + 1 > _highestSampler) {
		_highestSampler = (unsigned)index + 1;
	}
}

/* Metal lets a binding narrow the sampler's lod range. The compiler library takes the range
 * when the sampler is made, so honouring it here would need a second sampler object; until
 * it does, saying nothing would silently sample the wrong level. */
- (void)setSamplerState:(id)sampler
	    lodMinClamp:(float)lodMin
	    lodMaxClamp:(float)lodMax
		atIndex:(NSUInteger)index
{
	if (!_complained && (lodMin != 0.0f || lodMax < 1000.0f)) {
		rw_print("a per-binding lod clamp (%g..%g) is not implemented; the "
		         "sampler's own range is used instead\n", (double)lodMin, (double)lodMax);
		_complained = YES;
	}
	[self setSamplerState:sampler atIndex:index];
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/* Metal's own shorthand for small constant data; it is a buffer here, because that is what
 * the shader's binding is. */
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	id buffer = [[self device] newBufferWithBytes:bytes length:length options:0];

	if (buffer != nil) {
		[self setBuffer:[buffer autorelease] offset:0 atIndex:index];
	}
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	HKMetalWork *work = [[HKMetalWork alloc] init];

	if (_pipeline == nil || _complained) {
		rw_print("this dispatch has %s and is dropped\n",
		         _pipeline == nil ? "no pipeline state" : "a binding this driver refused");
		[work release];
		return;
	}
	work->pipeline = [_pipeline retain];
	work->count = _highest;
	work->textureCount = _highestTexture;
	work->samplerCount = _highestSampler;
	for (unsigned i = 0; i < _highest; i++) {
		work->buffers[i] = [_buffers[i] retain];
		work->bufferOffsets[i] = (size_t)_offsets[i];
	}
	for (unsigned i = 0; i < _highestTexture; i++) {
		work->textures[i] = [_textures[i] retain];
	}
	for (unsigned i = 0; i < _highestSampler; i++) {
		work->samplers[i] = [_samplers[i] retain];
	}
	work->groups[0] = (unsigned)groups.width;
	work->groups[1] = (unsigned)groups.height;
	work->groups[2] = (unsigned)groups.depth;
	work->size[0] = (unsigned)size.width;
	work->size[1] = (unsigned)size.height;
	work->size[2] = (unsigned)size.depth;
	[_commandBuffer recordWork:work];
	[work release];
}

/* Metal counts threads here and threadgroups above; the division is Metal's own, and a
 * grid that does not divide evenly is refused rather than rounded, because rounding runs
 * threads the application did not ask for. */
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)size
{
	MTLSize groups;

	if (size.width == 0 || size.height == 0 || size.depth == 0 ||
	    threads.width % size.width != 0 || threads.height % size.height != 0 ||
	    threads.depth % size.depth != 0) {
		rw_print("%lux%lux%lu threads do not divide into groups of "
		         "%lux%lux%lu, and this driver does not have the partial-group support "
		         "that would need\n", (unsigned long)threads.width,
		         (unsigned long)threads.height, (unsigned long)threads.depth,
		         (unsigned long)size.width, (unsigned long)size.height,
		         (unsigned long)size.depth);
		return;
	}
	groups.width = threads.width / size.width;
	groups.height = threads.height / size.height;
	groups.depth = threads.depth / size.depth;
	[self dispatchThreadgroups:groups threadsPerThreadgroup:size];
}

- (void)useResource:(id)resource usage:(NSUInteger)usage { (void)resource; (void)usage; }
- (void)useResources:(const id *)resources count:(NSUInteger)count usage:(NSUInteger)usage
{
	(void)resources; (void)count; (void)usage;
}
- (void)memoryBarrierWithScope:(NSUInteger)scope { (void)scope; }
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	(void)length; (void)index;   /* only what the shader declares is given, for now */
}

/*
 * THE REST OF WHAT <MTLComputeCommandEncoder> AND <MTLComputeCommandEncoderSPI> REQUIRE:
 * 20 methods that had no answer in this chain, each of which was an abort waiting for the
 * first daemon to encode a kernel. Counted by tools/metal/owes.py off the cache, because
 * MTLComputeCommandEncoderSPI is declared in AGXMetalA11.bundle and is in no process here.
 */

/* Served, by forwarding to the singular form -- which is where this driver's refusal of a
 * non-zero buffer offset already lives, so a plural bind inherits it instead of slipping
 * past it. */
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= RW_MAX_BUFFERS) {
		rw_print("buffer index %lu is past this driver's %d\n",
		         (unsigned long)index, RW_MAX_BUFFERS);
		return;
	}
	/* Metal's rule is that this moves a buffer that is already bound at that index, so
	 * with nothing bound there is nothing to move -- and binding nil here would add an
	 * empty slot to the dispatch's count and make it complain about a buffer nobody
	 * asked for. */
	if (_buffers[index] != nil) {
		[self setBuffer:_buffers[index] offset:offset atIndex:index];
	}
}
- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
	}
}
- (void)setSamplerStates:(const id *)samplers
	    lodMinClamps:(const float *)lodMin
	    lodMaxClamps:(const float *)lodMax
	       withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] lodMinClamp:lodMin[i]
			  lodMaxClamp:lodMax[i] atIndex:range.location + i];
	}
}

/* Served by construction: one ring, one submission per command buffer, in commit order, so
 * the ordering a barrier or a fence asks for is the only ordering there is. Residency is
 * the same answer as on the render encoder: every allocation is resident for its lifetime. */
- (void)memoryBarrierWithResources:(const id *)resources count:(NSUInteger)count
{
	(void)resources; (void)count;
}
- (void)updateFence:(id)fence  { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }
- (void)useHeap:(id)heap       { (void)heap; }
- (void)useHeaps:(const id *)heaps count:(NSUInteger)count { (void)heaps; (void)count; }
- (void)sampleCountersInBuffer:(id)buffer
		  atSampleIndex:(NSUInteger)index
		    withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;   /* the device has no counter sets */
}

/* Declined: there is no debug information for a kernel of ours. nil is what Metal's own
 * driver returns when a pipeline was not built with it, so the caller has this path. */
- (id)newKernelDebugInfo { return nil; }

/* Declined, and each says its name once. An indirect dispatch reads its grid out of GPU
 * memory, which means the size is not known when the command is recorded -- this driver
 * records sizes, so there is nothing to record. A dispatch that vanishes without a word is
 * the worst outcome available, hence the line. */
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer
			  indirectBufferOffset:(NSUInteger)offset
			 threadsPerThreadgroup:(MTLSize)size
{
	(void)buffer; (void)offset; (void)size;
	RW_ONCE("an indirect dispatch is not implemented (the grid lives in GPU "
		"memory and this driver records the grid); this dispatch is dropped\n");
}
- (void)executeCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
- (void)executeCommandsInBuffer:(id)buffer
		 indirectBuffer:(id)indirectBuffer
	   indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)indirectBuffer; (void)offset;
	RW_ONCE("indirect command buffers are not implemented; the commands in "
		"this one are not executed\n");
}
/* The stage-in region is for a kernel whose arguments arrive as [[stage_in]] -- the
 * non-uniform-grid feature. Nothing in the AIR this driver compiles declares one. */
- (void)hkNoStageIn
{
	RW_ONCE("a kernel stage-in region is not implemented; the dispatch "
		"covers the whole grid it was given\n");
}
- (void)setStageInRegion:(MTLRegion)region
{
	(void)region;
	[self hkNoStageIn];
}
- (void)setStageInRegionWithIndirectBuffer:(id)buffer
		      indirectBufferOffset:(NSUInteger)offset
{
	(void)buffer; (void)offset;
	[self hkNoStageIn];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
	(void)width; (void)height;
	RW_ONCE("imageblocks are not implemented; this kernel's imageblock "
		"size is ignored\n");
}
/* An entry for this exists in _MTLCommandEncoder and it is one of Apple's abstract ones
 * (tools/metal/protometh.py --verdicts), so inheriting it is an abort rather than a
 * default. It is a hint about how the hardware packs threadgroups together; the compiler
 * library decides that for itself and there is nothing here to turn off. */
- (void)setThreadgroupPackingDisabled:(BOOL)disabled { (void)disabled; }

- (void)hkNoFunctionTables
{
	RW_ONCE("visible and intersection function tables are not implemented; "
		"this binding is dropped\n");
}
- (void)setVisibleFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setVisibleFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}
- (void)setIntersectionFunctionTable:(id)table atBufferIndex:(NSUInteger)index
{
	(void)table; (void)index;
	[self hkNoFunctionTables];
}
- (void)setIntersectionFunctionTables:(const id *)tables withBufferRange:(NSRange)range
{
	(void)tables; (void)range;
	[self hkNoFunctionTables];
}

/* The same as the render encoder's, for the same measured reason: the superclass retains the
 * command buffer in the initialiser and releases it only here, and its -dealloc aborts on an
 * encoder released without this. Nothing has deallocated a compute encoder of ours on the
 * phone yet -- the one that aborted 43 times was the render one, out of warmup_shaders -- so
 * this half is the same fix made before it is measured, not after. */
- (void)endEncoding
{
	[super endEncoding];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLBlitCommandEncoder. Like the other two encoders it RECORDS, and -commit carries the
 * blits out in order with the draws and dispatches around them (-runBlit:), through the
 * compiler library's six blit calls (air_metal.h): buffer and texture copies on the GPU,
 * buffer <-> texture through synchronised maps, fills, and mipmap generation.
 *
 * What is here and does nothing, and why that is the truth rather than a stub: the
 * synchronize and optimize calls (every resource of this driver is one piece of memory
 * both sides see, already in the layout the GPU uses), fences (one queue, executed in
 * order: a fence can never be waited on out of order), and counters (this driver
 * samples none). What is refused by name: indirect command buffers and texture access
 * counters, which this driver does not make.
 */
@implementation HKMetalBlitEncoder {
	HKMetalCommandBuffer *_commandBuffer;   /* not retained: it owns the encoder's life */
}

+ (void)initialize
{
	if (self != [HKMetalBlitEncoder class]) {
		return;
	}
	Protocol *p = objc_getProtocol("MTLBlitCommandEncoder");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLBlitCommandEncoder) refused\n");
	}
}

- (instancetype)initWithHKCommandBuffer:(HKMetalCommandBuffer *)commandBuffer
{
	self = [super initWithCommandBuffer:commandBuffer];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = commandBuffer;
	return self;
}

- (id)device { return [_commandBuffer device]; }

- (HKMetalWork *)blit:(unsigned)kind from:(id)from to:(id)to
{
	HKMetalWork *work = [[[HKMetalWork alloc] init] autorelease];

	work->blit = kind;
	work->blitFrom = [from retain];
	work->blitTo = [to retain];
	[_commandBuffer recordWork:work];
	return work;
}

static void
rw_origin(unsigned out[3], MTLOrigin o)
{
	out[0] = (unsigned)o.x;
	out[1] = (unsigned)o.y;
	out[2] = (unsigned)o.z;
}

static void
rw_size(unsigned out[3], MTLSize s)
{
	out[0] = (unsigned)s.width;
	out[1] = (unsigned)s.height;
	out[2] = (unsigned)s.depth;
}

- (void)copyFromBuffer:(id)sourceBuffer sourceOffset:(NSUInteger)sourceOffset
	      toBuffer:(id)destinationBuffer destinationOffset:(NSUInteger)destinationOffset
		  size:(NSUInteger)size
{
	HKMetalWork *work = [self blit:RW_BLIT_BUFFER from:sourceBuffer to:destinationBuffer];

	work->fromOffset = sourceOffset;
	work->toOffset = destinationOffset;
	work->length = size;
}

- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value
{
	HKMetalWork *work = [self blit:RW_BLIT_FILL from:nil to:buffer];

	work->toOffset = range.location;
	work->length = range.length;
	work->fillValue = value;
}

- (void)copyFromBuffer:(id)sourceBuffer sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize
	     toTexture:(id)destinationTexture destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin
{
	HKMetalWork *work = [self blit:RW_BLIT_BUFFER_TO_TEXTURE from:sourceBuffer
					 to:destinationTexture];

	work->fromOffset = sourceOffset;
	work->rowBytes = sourceBytesPerRow;
	work->imageBytes = sourceBytesPerImage;
	rw_size(work->blitSize, sourceSize);
	work->toSlice = (unsigned)destinationSlice;
	work->toLevel = (unsigned)destinationLevel;
	rw_origin(work->toOrigin, destinationOrigin);
}

/* the options are for PVRTC and depth/stencil planes, neither of which this driver makes */
- (void)copyFromBuffer:(id)sourceBuffer sourceOffset:(NSUInteger)sourceOffset
     sourceBytesPerRow:(NSUInteger)sourceBytesPerRow
   sourceBytesPerImage:(NSUInteger)sourceBytesPerImage sourceSize:(MTLSize)sourceSize
	     toTexture:(id)destinationTexture destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin options:(NSUInteger)options
{
	(void)options;
	[self copyFromBuffer:sourceBuffer sourceOffset:sourceOffset sourceBytesPerRow:sourceBytesPerRow
	 sourceBytesPerImage:sourceBytesPerImage sourceSize:sourceSize toTexture:destinationTexture
	    destinationSlice:destinationSlice destinationLevel:destinationLevel
	   destinationOrigin:destinationOrigin];
}

- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice
	    sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin
	     sourceSize:(MTLSize)sourceSize toBuffer:(id)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset
 destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
destinationBytesPerImage:(NSUInteger)destinationBytesPerImage
{
	HKMetalWork *work = [self blit:RW_BLIT_TEXTURE_TO_BUFFER from:sourceTexture
					 to:destinationBuffer];

	work->fromSlice = (unsigned)sourceSlice;
	work->fromLevel = (unsigned)sourceLevel;
	rw_origin(work->fromOrigin, sourceOrigin);
	rw_size(work->blitSize, sourceSize);
	work->toOffset = destinationOffset;
	work->rowBytes = destinationBytesPerRow;
	work->imageBytes = destinationBytesPerImage;
}

- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice
	    sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin
	     sourceSize:(MTLSize)sourceSize toBuffer:(id)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset
 destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
destinationBytesPerImage:(NSUInteger)destinationBytesPerImage options:(NSUInteger)options
{
	(void)options;
	[self copyFromTexture:sourceTexture sourceSlice:sourceSlice sourceLevel:sourceLevel
		 sourceOrigin:sourceOrigin sourceSize:sourceSize toBuffer:destinationBuffer
	    destinationOffset:destinationOffset destinationBytesPerRow:destinationBytesPerRow
     destinationBytesPerImage:destinationBytesPerImage];
}

- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice
	    sourceLevel:(NSUInteger)sourceLevel sourceOrigin:(MTLOrigin)sourceOrigin
	     sourceSize:(MTLSize)sourceSize toTexture:(id)destinationTexture
       destinationSlice:(NSUInteger)destinationSlice
       destinationLevel:(NSUInteger)destinationLevel
      destinationOrigin:(MTLOrigin)destinationOrigin
{
	HKMetalWork *work = [self blit:RW_BLIT_TEXTURE from:sourceTexture to:destinationTexture];

	work->fromSlice = (unsigned)sourceSlice;
	work->fromLevel = (unsigned)sourceLevel;
	rw_origin(work->fromOrigin, sourceOrigin);
	rw_size(work->blitSize, sourceSize);
	work->toSlice = (unsigned)destinationSlice;
	work->toLevel = (unsigned)destinationLevel;
	rw_origin(work->toOrigin, destinationOrigin);
}

/* Whole slices and levels: one region copy per (slice, level), each level's own size. */
- (void)copyFromTexture:(id)sourceTexture sourceSlice:(NSUInteger)sourceSlice
	    sourceLevel:(NSUInteger)sourceLevel toTexture:(id)destinationTexture
       destinationSlice:(NSUInteger)destinationSlice
       destinationLevel:(NSUInteger)destinationLevel sliceCount:(NSUInteger)sliceCount
	     levelCount:(NSUInteger)levelCount
{
	id<MTLTexture> tex = sourceTexture;

	for (NSUInteger l = 0; l < levelCount; l++) {
		NSUInteger level = sourceLevel + l;
		MTLSize size = MTLSizeMake(MAX([tex width] >> level, 1),
					   MAX([tex height] >> level, 1),
					   [tex textureType] == MTLTextureType3D
					       ? MAX([tex depth] >> level, 1) : 1);

		for (NSUInteger s = 0; s < sliceCount; s++) {
			[self copyFromTexture:sourceTexture sourceSlice:sourceSlice + s
				  sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:size toTexture:destinationTexture
			     destinationSlice:destinationSlice + s
			     destinationLevel:destinationLevel + l
			    destinationOrigin:MTLOriginMake(0, 0, 0)];
		}
	}
}

- (void)copyFromTexture:(id)sourceTexture toTexture:(id)destinationTexture
{
	id<MTLTexture> tex = sourceTexture;
	NSUInteger slices = [tex arrayLength] * ([tex textureType] == MTLTextureTypeCube ||
						 [tex textureType] == MTLTextureTypeCubeArray ? 6 : 1);

	[self copyFromTexture:sourceTexture sourceSlice:0 sourceLevel:0
		    toTexture:destinationTexture destinationSlice:0 destinationLevel:0
		   sliceCount:slices
		   levelCount:MIN([tex mipmapLevelCount],
				  [(id<MTLTexture>)destinationTexture mipmapLevelCount])];
}

- (void)generateMipmapsForTexture:(id)texture
{
	[self blit:RW_BLIT_MIPMAPS from:nil to:texture];
}

/* one piece of memory both sides see, in the GPU's own layout: nothing to synchronise */
- (void)synchronizeResource:(id)resource { (void)resource; }
- (void)synchronizeTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForGPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForGPUAccess:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}
- (void)optimizeContentsForCPUAccess:(id)texture { (void)texture; }
- (void)optimizeContentsForCPUAccess:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level
{
	(void)texture; (void)slice; (void)level;
}

/* one queue, run in order: what a fence orders is already ordered */
- (void)updateFence:(id)fence { (void)fence; }
- (void)waitForFence:(id)fence { (void)fence; }

- (void)sampleCountersInBuffer:(id)buffer atSampleIndex:(NSUInteger)index withBarrier:(BOOL)barrier
{
	(void)buffer; (void)index; (void)barrier;
	RW_ONCE("counter sampling is not implemented: nothing is sampled\n");
}
- (void)resolveCounters:(id)buffer inRange:(NSRange)range destinationBuffer:(id)destination
      destinationOffset:(NSUInteger)offset
{
	(void)buffer; (void)range; (void)destination; (void)offset;
	RW_ONCE("counter sampling is not implemented: nothing is resolved\n");
}

- (void)hkNoIndirectCommands
{
	RW_ONCE("indirect command buffers are not implemented; this blit is dropped\n");
}
- (void)resetCommandsInBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self hkNoIndirectCommands];
}
- (void)copyIndirectCommandBuffer:(id)source sourceRange:(NSRange)range
			destination:(id)destination destinationIndex:(NSUInteger)index
{
	(void)source; (void)range; (void)destination; (void)index;
	[self hkNoIndirectCommands];
}
- (void)optimizeIndirectCommandBuffer:(id)buffer withRange:(NSRange)range
{
	(void)buffer; (void)range;
	[self hkNoIndirectCommands];
}
- (void)getTextureAccessCounters:(id)texture region:(MTLRegion)region mipLevel:(NSUInteger)level
			   slice:(NSUInteger)slice resetCounters:(BOOL)reset
		  countersBuffer:(id)buffer countersBufferOffset:(NSUInteger)offset
{
	(void)texture; (void)region; (void)level; (void)slice; (void)reset; (void)buffer;
	(void)offset;
	RW_ONCE("texture access counters are not implemented\n");
}
- (void)resetTextureAccessCounters:(id)texture region:(MTLRegion)region mipLevel:(NSUInteger)level
			     slice:(NSUInteger)slice
{
	(void)texture; (void)region; (void)level; (void)slice;
}

/* the superclass retains the command buffer in the initialiser and releases it here, as for
 * the other two encoders */
- (void)endEncoding
{
	[super endEncoding];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * MTLArgumentEncoder, which on this driver is a RECORDER and not a writer.
 *
 * Metal's encoder writes one 8-byte handle per member into a plain buffer, and those
 * handles are an Apple GPU's descriptors: a Gallium driver, which binds by index, cannot
 * read them. It also does not need to -- the AIR frontend resolves every member of every
 * set to a binding while translating (docs/air.md 8.2) -- so each -setBuffer:atIndex:
 * here RECORDS (resource, Metal id) against (the destination buffer, the destination
 * offset) in the compiler library, and the draw binds every member the pipeline's shaders
 * declare from those records. The buffer's contents are never read, and an argument
 * buffer the application filled by any other means is REFUSED BY NAME at the draw.
 *
 * Two things are deliberate about this object:
 *
 *   - IT HOLDS NO RECORDS ITSELF. Its state is (buffer, offset), and the records live
 *     with the destination buffer in the library, because that is what Metal's own object
 *     model allows: one buffer holds several encodings at several offsets, each reused
 *     across many draws, and an application may release the encoder and go on drawing
 *     with what it encoded.
 *   - -setArgumentBuffer:offset: DOES NOT CLEAR the encoding at that offset, which is
 *     Metal's behaviour: the bytes an earlier encoding wrote there are still there, so a
 *     second encoding that sets three of five members leaves the other two as they were.
 *
 * The layout comes from the module (`air.indirect_buffer`), so `encodedLength` and
 * `alignment` are Apple's own numbers -- 8 bytes per member the set declares and 8 --
 * which matters because the application allocates its buffer from them and then encodes
 * at multiples of them.
 */
@implementation HKMetalArgumentEncoder {
	HKMetalDevice *_owner;         /* not retained: the device owns everything here */
	NSString *_label;
	struct rwa_argbuf_layout _layout;
	BOOL _fromModule;                   /* the layout came from a shader, not descriptors */
	HKMetalBuffer *_destination;   /* what -setArgumentBuffer:offset: named */
	NSUInteger _offset;
	BOOL _complained;
}

/* Conformance declared at runtime, as the device's MTLDeviceSPI is and for the same
 * reason: an application (or Metal's own validation) may ask conformsToProtocol: before
 * it sends anything, and the protocol is only in the process once Metal is. Every method
 * it requires is answered below -- some of them by declining out loud, which is an answer
 * and not an absence. */
+ (void)load
{
	Protocol *p = objc_getProtocol("MTLArgumentEncoder");

	if (p != NULL && !class_conformsToProtocol(self, p) && !class_addProtocol(self, p)) {
		rw_print("class_addProtocol(MTLArgumentEncoder) refused\n");
	}
}

- (instancetype)initWithHKDevice:(HKMetalDevice *)device
			       layout:(const struct rwa_argbuf_layout *)layout
			   fromModule:(BOOL)fromModule
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	_layout = *layout;
	_fromModule = fromModule;
	return self;
}

- (void)dealloc
{
	[_label release];
	[_destination release];
	[super dealloc];
}

- (id)device       { return _owner; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label
{
	[_label release];
	_label = [label copy];
}

/* The two numbers an application allocates from, and they are the module's own: 8 bytes
 * per member the set declares, used by this shader or not (air_metal.h). A length of ours
 * that was too small would have the application overlapping its own encodings. */
- (NSUInteger)encodedLength { return _layout.encoded_length; }
- (NSUInteger)alignment     { return _layout.alignment; }

/* Not part of MTLArgumentEncoder: what this encoder is for, so a test can read back the
 * mapping between Metal's [[id(n)]] and the index the GPU is programmed with instead of
 * trusting it. Metal's own pipeline reflection would report the same thing. */
- (NSUInteger)hkMemberCount { return _layout.member_count; }
- (NSUInteger)hkMetalIndex  { return _layout.metal_index; }
- (BOOL)hkMember:(NSUInteger)i id:(NSUInteger *)mid kind:(NSUInteger *)kind
		index:(NSUInteger *)index
{
	if (i >= _layout.member_count) {
		return NO;
	}
	if (mid != NULL) {
		*mid = _layout.members[i].id;
	}
	if (kind != NULL) {
		*kind = _layout.members[i].kind;
	}
	if (index != NULL) {
		*index = _layout.members[i].index;
	}
	return YES;
}

/*
 * The destination. Metal checks the offset against the buffer's length and the encoder's
 * alignment, and so does this -- an application that encodes at an unaligned offset has
 * allocated its buffer from something other than -encodedLength, and every record made
 * afterwards would be keyed by an offset the draw will never look up.
 */
- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset
{
	if ([buffer isKindOfClass:[HKMetalBuffer class]]) {
		[(HKMetalBuffer *)buffer hkNeverKeep];
	}
	if (buffer != nil && ![buffer isKindOfClass:[HKMetalBuffer class]]) {
		rw_print("the argument buffer is a %s and not one of ours, so "
		         "nothing can be encoded into it\n",
		         class_getName(object_getClass(buffer)));
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && [buffer mesaBuffer] == NULL) {
		rw_print("the argument buffer did not come from the compiler "
		         "library, so nothing can be encoded into it\n");
		[_destination release];
		_destination = nil;
		return;
	}
	if (buffer != nil && _layout.alignment != 0 && offset % _layout.alignment != 0) {
		rw_print("an argument buffer offset of %lu is not a multiple of "
		         "this encoder's alignment (%u), so the draw would look for the "
		         "encoding somewhere else\n", (unsigned long)offset, _layout.alignment);
		[_destination release];
		_destination = nil;
		return;
	}
	/* Retained, unlike the other back-pointers in this file, and for a measured
	 * reason: the records live in the compiler library WITH that buffer, so an
	 * application that released it while this encoder still pointed at it would have
	 * rwa_buffer_destroy free the records (and scrub them out of everyone else's) and
	 * leave this object holding a freed MTLBuffer to send -mesaBuffer to. Metal's own
	 * encoder outliving its argument buffer is an application's mistake; a crash in
	 * the driver is ours. */
	[buffer retain];
	[_destination release];
	_destination = buffer;
	_offset = offset;
}

/* Metal's array form: the same destination with an element's worth of offset added. The
 * arithmetic is Metal's own -- encodedLength per element -- and it is here rather than
 * declined because an application that encodes an ARRAY of sets is doing nothing this
 * driver cannot record. */
- (void)setArgumentBuffer:(id)buffer startOffset:(NSUInteger)offset
	     arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:offset + element * _layout.encoded_length];
}

/* Whether this encoder is in a state to record, said once per site: an id past the set's
 * own members is the mistake worth naming, because Metal would write a handle into the
 * buffer and the shader would read a descriptor that is not there. */
- (BOOL)hkReady:(NSUInteger)index for:(const char *)what
{
	if (_destination == nil) {
		if (!_complained) {
			rw_print("%s was encoded at id %lu with no argument buffer "
			         "set (Metal's -setArgumentBuffer:offset: has to name one "
			         "first)\n", what, (unsigned long)index);
			_complained = YES;
		}
		return NO;
	}
	if (index >= _layout.member_count) {
		/* Which of the two layouts this is matters to whoever reads the line: a
		 * shader's set is the shader's business and an application cannot change
		 * it, while a set described with MTLArgumentDescriptors is the
		 * application's own and the id it just used is one it did not describe. */
		rw_print("%s was encoded at id %lu of %s that declares %u member%s\n", what,
		         (unsigned long)index,
		         _fromModule ? "a set a shader declares" : "an encoder this "
		                       "application described",
		         _layout.member_count, _layout.member_count == 1 ? "" : "s");
		return NO;
	}
	return YES;
}

/*
 * The three that record. The KIND is the call, as it is in Metal; a member encoded with
 * the wrong one is refused at the draw, by name, where the shader's own declaration is in
 * hand -- not here, because one buffer may be encoded for a set this driver has never
 * seen a shader for.
 *
 * The library's sentence is passed through rather than replaced: it names the id and what
 * went wrong with it.
 */
- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self hkReady:index for:"a buffer"]) {
		return;
	}
	if (buffer != nil && ![buffer isKindOfClass:[HKMetalBuffer class]]) {
		rw_print("the buffer encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(buffer)));
		return;
	}
	if (mesa.argbuf_encode_buffer([_destination mesaBuffer], _offset, (unsigned)index,
				      [buffer mesaBuffer], offset, why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setBuffers:(const id *)buffers
	   offsets:(const NSUInteger *)offsets
	 withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setBuffer:buffers[i] offset:offsets ? offsets[i] : 0
			atIndex:range.location + i];
	}
}

- (void)setTexture:(id)texture atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self hkReady:index for:"a texture"]) {
		return;
	}
	if (texture != nil && ![texture isKindOfClass:[HKMetalTexture class]]) {
		rw_print("the texture encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(texture)));
		return;
	}
	if (mesa.argbuf_encode_texture([_destination mesaBuffer], _offset, (unsigned)index,
				       [texture mesaTexture], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setTextures:(const id *)textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setTexture:textures[i] atIndex:range.location + i];
	}
}

- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index
{
	char why[512] = { 0 };

	if (![self hkReady:index for:"a sampler"]) {
		return;
	}
	if (sampler != nil && ![sampler isKindOfClass:[HKMetalSampler class]]) {
		rw_print("the sampler encoded at id %lu is a %s and not one of "
		         "ours\n", (unsigned long)index,
		         class_getName(object_getClass(sampler)));
		return;
	}
	if (mesa.argbuf_encode_sampler([_destination mesaBuffer], _offset, (unsigned)index,
				       [sampler mesaSampler], why, sizeof(why)) != 0) {
		rw_print("%s\n", why);
	}
}

- (void)setSamplerStates:(const id *)samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++) {
		[self setSamplerState:samplers[i] atIndex:range.location + i];
	}
}

/*
 * -constantDataAtIndex: is where an application writes a VALUE member -- an `int` or a
 * `float4` inside the set rather than a descriptor -- and it is answered with the real
 * bytes of the argument buffer at that member's place, which is what Metal answers.
 *
 * Nothing of this driver ever reads those bytes, and that is said here because it is the
 * kind of silence that costs a day: a shader that reads a value out of a set is refused
 * when it is TRANSLATED ("a value read out of the set instead of a descriptor",
 * docs/air.md 8.2.1), so an application writing here is writing into memory no shader of
 * ours will be compiled to read. Answering a real pointer is still the right answer --
 * the write lands in the application's own buffer and nothing is corrupted -- while
 * answering NULL would be a crash in the application's memcpy.
 */
- (void *)constantDataAtIndex:(NSUInteger)index
{
	if (![self hkReady:index for:"constant data"]) {
		return NULL;
	}
	void *bytes = [_destination contents];

	if (bytes == NULL) {
		return NULL;
	}
	RW_ONCE("-constantDataAtIndex: hands back the argument buffer's own "
		"bytes, and nothing in this driver reads them: a shader that reads a VALUE "
		"out of an argument buffer is refused when it is translated, because every "
		"member this frontend binds is a descriptor\n");
	return (char *)bytes + _offset + 8 * index;
}

/*
 * A SET INSIDE A SET, declined by name. The frontend refuses a nested `air.indirect_buffer`
 * when it translates the shader -- one module of Resident Evil 4's 7 620 has one, the
 * kernel that encodes draws on the GPU -- so an encoder for one would be an object whose
 * records no pipeline could ever ask for.
 */
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
	rw_print("-newArgumentEncoderForBufferAtIndex:%lu asks for an encoder for "
	         "a set INSIDE a set, and this port's frontend refuses a nested argument buffer "
	         "when it translates the shader, so there is nothing to encode for\n",
	         (unsigned long)index);
	return nil;
}

/*
 * The members that are not a buffer, a texture or a sampler. Every one of them is declined
 * BY NAME and for the same reason: the frontend refuses such a member when it translates
 * the shader ("a member that is not a buffer, a texture or a sampler"), so a record here
 * would be a record no draw could use -- and a silent no-op would leave the application
 * believing its set was encoded.
 */
- (void)hkNoSuchMember:(const char *)what atIndex:(NSUInteger)index
{
	rw_print("%s was encoded at id %lu of an argument buffer, and this port "
	         "binds a buffer, a texture or a sampler member and refuses the rest when the "
	         "shader is translated; this encoding is dropped\n", what, (unsigned long)index);
}

- (void)setRenderPipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self hkNoSuchMember:"a render pipeline state" atIndex:index];
}
- (void)setRenderPipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self hkNoSuchMember:"render pipeline states" atIndex:range.location];
}
- (void)setComputePipelineState:(id)state atIndex:(NSUInteger)index
{
	(void)state;
	[self hkNoSuchMember:"a compute pipeline state" atIndex:index];
}
- (void)setComputePipelineStates:(const id *)states withRange:(NSRange)range
{
	(void)states;
	[self hkNoSuchMember:"compute pipeline states" atIndex:range.location];
}
- (void)setIndirectCommandBuffer:(id)buffer atIndex:(NSUInteger)index
{
	(void)buffer;
	[self hkNoSuchMember:"an indirect command buffer" atIndex:index];
}
- (void)setIndirectCommandBuffers:(const id *)buffers withRange:(NSRange)range
{
	(void)buffers;
	[self hkNoSuchMember:"indirect command buffers" atIndex:range.location];
}
- (void)setAccelerationStructure:(id)structure atIndex:(NSUInteger)index
{
	(void)structure;
	[self hkNoSuchMember:"an acceleration structure" atIndex:index];
}
- (void)setVisibleFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self hkNoSuchMember:"a visible function table" atIndex:index];
}
- (void)setVisibleFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self hkNoSuchMember:"visible function tables" atIndex:range.location];
}
- (void)setIntersectionFunctionTable:(id)table atIndex:(NSUInteger)index
{
	(void)table;
	[self hkNoSuchMember:"an intersection function table" atIndex:index];
}
- (void)setIntersectionFunctionTables:(const id *)tables withRange:(NSRange)range
{
	(void)tables;
	[self hkNoSuchMember:"intersection function tables" atIndex:range.location];
}

@end

/*
 * ---------------------------------------------------------------------------------------
 * -[MTLFunction newArgumentEncoderWithBufferIndex:], which is a method of METAL'S OWN
 * class and is therefore installed at RUNTIME here rather than written as a category.
 *
 * Two decisions, and the first one is measured. Metal's own implementation cannot work on
 * this port:
 *
 *   -[_MTLFunction newArgumentEncoderWithBufferIndex:] (0x181aef7f4) forwards to
 *   ...reflection:binaryArchives: (0x181aef85c), which asks
 *   -[_MTLFunction reflectionWithOptions:binaryArchives:] (0x181aef458) for the
 *   function's reflection -- and that one calls -compiler, which on this port is
 *   -[_MTLDevice compiler] (0x181aac128), a two-instruction stub answering nil
 *   (docs/metal.md 10.7: everything behind Metal's own compiler is nil here). The
 *   builder then walks `[reflection arguments]`, finds a count of zero, and takes its
 *   failure path (0x181aef924):
 *
 *     MTLReportFailure(0, "-[_MTLFunction newArgumentEncoderWithBufferIndex:reflection:"
 *                      "functionReflection:]", 0x22fc,
 *                      @"bufferIndex %lu does not identify an argument buffer")
 *
 *   and returns nil.
 *
 * So the choice is not between Apple's implementation and ours; it is between nil and an
 * encoder. And what Metal was missing is what this port HAS: -[_MTLFunction bitcodeData]
 * works (docs/metal.md 10.12, checked over all eleven functions of AV1SW.metallib), and
 * the frontend reads the set's layout straight out of those bytes. Resident Evil 4 sends
 * this selector; so does any application that writes `[[id(n)]]` structs in MSL instead
 * of building MTLArgumentDescriptors.
 *
 * The second decision is HOW, and a category is the obvious way and the wrong one: a
 * category on `_MTLFunction` makes this dylib import `_OBJC_CLASS_$__MTLFunction`, and a
 * private class symbol that does not bind is a dylib that does not LOAD -- which would
 * take the whole driver down for every Metal client in the process, not just this
 * selector. The one private class symbol this driver does import,
 * `_OBJC_CLASS_$__MTLDevice`, is imported by IOGPU.framework too and has been measured
 * loading on the phone; `ipsw dyld symaddr -b` finds nothing in the cache that imports
 * _MTLFunction's, so nothing says it would bind. class_replaceMethod needs no symbol at
 * all, and it is the same move ensure_spi already makes for MTLDeviceSPI.
 *
 * What it costs, said plainly: the method is replaced for every MTLFunction in the
 * process, Apple's daemons included. Nothing is lost by that -- the implementation it
 * replaces returns nil for all of them, for the reason above -- and nothing else of
 * _MTLFunction is touched.
 */
static id
rw_function_argument_encoder(id self, SEL cmd, NSUInteger index)
{
	id device = ((id (*)(id, SEL))objc_msgSend)(self, sel_getUid("device"));

	(void)cmd;
	if (![device isKindOfClass:[HKMetalDevice class]]) {
		rw_print("-newArgumentEncoderWithBufferIndex: on a function whose device is "
		         "a %s and not ours\n", class_getName(object_getClass(device)));
		return nil;
	}
	return [(HKMetalDevice *)device hkArgumentEncoderForFunction:self
								  bufferIndex:index];
}

/* The same with Metal's out-parameter, which is answered nil: a reflection object needs
 * Metal's own compiler, and every pipeline path of this driver already says so. */
static id
rw_function_argument_encoder_reflection(id self, SEL cmd, NSUInteger index, id *reflection)
{
	if (reflection != NULL) {
		*reflection = nil;
	}
	return rw_function_argument_encoder(self, cmd, index);
}

/* Installed once per process, from ensure_spi -- which runs at +load (Metal loaded us, so
 * Metal is there) and again before the first device is made (a library can also be
 * brought in by hand, before Metal). A process without Metal's class simply has no
 * MTLFunction to answer for. */
static void
ensure_function_encoder(void)
{
	static BOOL installed;
	Class fn = objc_lookUpClass("_MTLFunction");

	if (installed || fn == NULL) {
		return;
	}
	installed = YES;
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:"),
			    (IMP)rw_function_argument_encoder, "@@:Q");
	class_replaceMethod(fn, sel_getUid("newArgumentEncoderWithBufferIndex:reflection:"),
			    (IMP)rw_function_argument_encoder_reflection, "@@:Q^@");
}

/* ---- heaps, fences, events -------------------------------------------------------------- */

static void
rw_conform(Class cls, const char *name)
{
	Protocol *p = objc_getProtocol(name);

	if (p != NULL && !class_conformsToProtocol(cls, p) && !class_addProtocol(cls, p)) {
		rw_print("class_addProtocol(%s) refused\n", name);
	}
}

@implementation HKMetalResource
- (id)heap               { return _heap; }
- (NSUInteger)heapOffset { return _heapOffset; }
- (BOOL)isAliasable      { return _aliasable; }
/* Its size goes back to the heap's account at once, as Metal's does; its memory stays its own
 * until it is released, because nothing here hands one allocation to two resources. */
- (void)makeAliasable
{
	if (_heap != nil && !_aliasable) {
		_aliasable = YES;
		[_heap hkGiveBack:_heapSize];
	}
}
- (void)hkPlaceInHeap:(HKMetalHeap *)heap size:(NSUInteger)size offset:(NSUInteger)offset
{
	_heap = [heap retain];
	_heapSize = size;
	_heapOffset = offset;
}
- (void)dealloc
{
	if (_heap != nil) {
		if (!_aliasable) {
			[_heap hkGiveBack:_heapSize];
		}
		[_heap release];
	}
	[super dealloc];
}
@end

@implementation HKMetalFence {
	HKMetalDevice *_owner;
}
+ (void)load { rw_conform(self, "MTLFence"); }
- (instancetype)initWithHKDevice:(HKMetalDevice *)device
{
	self = [super init];
	_owner = device;
	return self;
}
- (id)device { return _owner; }
@end

/*
 * MTLEvent and MTLSharedEvent, one class. Two values: `ordered` is where the GPU's work has got
 * in the order it is submitted -- what a command buffer's wait is judged by -- and `signaled` is
 * what the processor may see, which a command buffer's signal reaches only once that buffer is
 * complete. The processor's own -setSignaledValue: is both at once.
 */
@implementation HKMetalEvent {
	HKMetalDevice *_owner;
	BOOL _shared;
	pthread_mutex_t _lock;
	uint64_t _ordered, _signaled;
	NSMutableArray *_waiting;       /* [command buffer, value] */
	NSMutableArray *_listeners;     /* [value, block, queue] */
}
+ (void)load
{
	rw_conform(self, "MTLEvent");
	rw_conform(self, "MTLSharedEvent");
}
- (instancetype)initWithHKDevice:(HKMetalDevice *)device shared:(BOOL)shared
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_owner = device;
	_shared = shared;
	pthread_mutex_init(&_lock, NULL);
	_waiting = [[NSMutableArray alloc] init];
	_listeners = [[NSMutableArray alloc] init];
	return self;
}
- (void)dealloc
{
	[_waiting release];
	[_listeners release];
	pthread_mutex_destroy(&_lock);
	[super dealloc];
}
- (id)device { return _owner; }

- (BOOL)hkWait:(HKMetalCommandBuffer *)buffer value:(uint64_t)value
{
	BOOL wait;

	pthread_mutex_lock(&_lock);
	wait = _ordered < value;
	if (wait) {
		[_waiting addObject:@[ buffer, @(value) ]];
	}
	pthread_mutex_unlock(&_lock);
	return wait;
}

- (NSArray *)hkSignalOrdered:(uint64_t)value
{
	NSMutableArray *ready = [NSMutableArray array];
	NSMutableArray *still = [NSMutableArray array];

	pthread_mutex_lock(&_lock);
	if (value > _ordered) {
		_ordered = value;
	}
	for (NSArray *w in _waiting) {
		if ([[w objectAtIndex:1] unsignedLongLongValue] <= _ordered) {
			[ready addObject:[w objectAtIndex:0]];
		} else {
			[still addObject:w];
		}
	}
	[_waiting setArray:still];
	pthread_mutex_unlock(&_lock);
	return ready;
}

- (void)hkSignalVisible:(uint64_t)value
{
	NSMutableArray *fire = [NSMutableArray array];
	NSMutableArray *still = [NSMutableArray array];
	uint64_t now;

	pthread_mutex_lock(&_lock);
	if (value > _signaled) {
		_signaled = value;
	}
	now = _signaled;
	for (NSArray *l in _listeners) {
		[([[l objectAtIndex:0] unsignedLongLongValue] <= now ? fire : still) addObject:l];
	}
	[_listeners setArray:still];
	pthread_mutex_unlock(&_lock);
	for (NSArray *l in fire) {
		void (^block)(id, uint64_t) = [l objectAtIndex:1];

		[self retain];
		dispatch_async([l objectAtIndex:2], ^{
			block(self, now);
			[self release];
		});
	}
}

- (uint64_t)signaledValue
{
	uint64_t value;

	pthread_mutex_lock(&_lock);
	value = _signaled;
	pthread_mutex_unlock(&_lock);
	return value;
}

- (void)setSignaledValue:(uint64_t)value
{
	NSArray *ready = [self hkSignalOrdered:value];

	[self hkSignalVisible:value];
	for (HKMetalCommandBuffer *buffer in ready) {
		[buffer hkRun];
	}
}

- (void)notifyListener:(id)listener atValue:(uint64_t)value block:(void (^)(id, uint64_t))block
{
	dispatch_queue_t queue = [listener respondsToSelector:@selector(dispatchQueue)] ? [listener dispatchQueue] : nil;

	if (block == NULL) {
		return;
	}
	pthread_mutex_lock(&_lock);
	[_listeners addObject:@[ @(value), [[block copy] autorelease],
	    queue != nil ? queue : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0) ]];
	pthread_mutex_unlock(&_lock);
	[self hkSignalVisible:0];       /* (one already reached fires at once) */
}

/* shortcut: looks once a millisecond; a condition variable if anything waits on this for long */
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
	for (uint64_t waited = 0; [self signaledValue] < value; waited++) {
		if (waited >= milliseconds) {
			return NO;
		}
		usleep(1000);
	}
	return YES;
}

/* a handle is for another process (XPC); nothing of this driver's crosses processes */
- (id)newSharedEventHandle
{
	RW_ONCE("a shared event's handle for another process was asked for; there is none\n");
	return nil;
}
@end

/*
 * MTLHeap as an account: Metal's promise is that a heap's resources fit in its size, and that
 * the sizes the device answers (-heapBufferSizeAndAlignWithLength:...) are what they take. Each
 * resource is still its own allocation, which Metal allows -- aliasable resources may share
 * memory, they need not -- and costs only the saving a real suballocator would make. A
 * placement heap's resources go where the caller says and count nothing against it.
 */
@implementation HKMetalHeap {
	HKMetalDevice *_owner;
	pthread_mutex_t _lock;
	NSUInteger _size, _used, _storageMode, _cpuCacheMode, _hazardTrackingMode, _type;
}
+ (void)load { rw_conform(self, "MTLHeap"); }
- (instancetype)initWithHKDevice:(HKMetalDevice *)device descriptor:(MTLHeapDescriptor *)descriptor
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	if ([descriptor size] == 0) {
		[self release];
		return nil;
	}
	_owner = device;
	pthread_mutex_init(&_lock, NULL);
	_size = rw_heap_round([descriptor size]);
	_storageMode = [descriptor storageMode];
	_cpuCacheMode = [descriptor cpuCacheMode];
	_hazardTrackingMode = [descriptor hazardTrackingMode];
	_type = [descriptor respondsToSelector:@selector(type)] ? (NSUInteger)[descriptor type] : 0;
	return self;
}
- (void)dealloc
{
	pthread_mutex_destroy(&_lock);
	[super dealloc];
}
- (id)device                     { return _owner; }
- (NSUInteger)size               { return _size; }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)storageMode        { return _storageMode; }
- (NSUInteger)cpuCacheMode       { return _cpuCacheMode; }
- (NSUInteger)hazardTrackingMode { return _hazardTrackingMode; }
- (NSUInteger)resourceOptions    { return _cpuCacheMode | _storageMode << 4 | _hazardTrackingMode << 8; }
- (NSInteger)type                { return (NSInteger)_type; }
- (NSUInteger)setPurgeableState:(NSUInteger)state { (void)state; return 2; }   /* never volatile, as the resources */
- (NSUInteger)usedSize
{
	NSUInteger used;

	pthread_mutex_lock(&_lock);
	used = _used;
	pthread_mutex_unlock(&_lock);
	return used;
}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
	NSUInteger left = _size - [self usedSize];

	return alignment != 0 && (alignment & (alignment - 1)) == 0 ? left & ~(alignment - 1) : left;
}
- (BOOL)hkTake:(NSUInteger)size
{
	BOOL fits;

	pthread_mutex_lock(&_lock);
	fits = _used + size <= _size;
	if (fits) {
		_used += size;
	}
	pthread_mutex_unlock(&_lock);
	return fits;
}
- (void)hkGiveBack:(NSUInteger)size
{
	pthread_mutex_lock(&_lock);
	_used -= size <= _used ? size : _used;
	pthread_mutex_unlock(&_lock);
}
/* `offset`: NSNotFound for an automatic heap's resource, else where a placement heap's goes */
- (id)hkPlace:(HKMetalResource *)resource size:(NSUInteger)size offset:(NSUInteger)offset
{
	BOOL placed = offset != NSNotFound;

	if (placed ? offset > _size || size > _size - offset : ![self hkTake:size]) {
		[resource release];
		return nil;
	}
	if (resource == nil) {
		if (!placed) {
			[self hkGiveBack:size];
		}
		return nil;
	}
	[resource hkPlaceInHeap:self size:placed ? 0 : size offset:placed ? offset : 0];
	return resource;
}
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options
{
	return [self hkPlace:[_owner newBufferWithLength:length options:options] size:rw_heap_round(length)
		      offset:NSNotFound];
}
- (id)newBufferWithLength:(NSUInteger)length options:(NSUInteger)options offset:(NSUInteger)offset
{
	return [self hkPlace:[_owner newBufferWithLength:length options:options] size:rw_heap_round(length)
		      offset:offset];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	return [self hkPlace:[_owner newTextureWithDescriptor:descriptor]
			size:rw_heap_round(rw_texture_bytes(descriptor)) offset:NSNotFound];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
{
	return [self hkPlace:[_owner newTextureWithDescriptor:descriptor]
			size:rw_heap_round(rw_texture_bytes(descriptor)) offset:offset];
}
@end
