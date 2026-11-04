/*
 * The moving pictures by the GPU (the OnePlus 8's Adreno 650), not the processor: camera_isp.c's preview,
 * the same picture, in OpenGL ES 3 through the image's own Mesa (/hackios/lib/libOSMesa.8.dylib, freedreno --
 * the window server's library, see docs/subsystems/gpu.md), taken into mediaserverd with dlopen. At 30 frames a
 * second the processor's renderer held the big cores at 95 C (runs 104112, 105351) and cost 90-140 ms of
 * processor time a picture (runs 111039, 111605), the little cores' share of it most of the wait.
 *
 * A frame, three steps on the GPU and two copies:
 *   - the frame's RAW10 bytes (four pixels in five bytes) up into a texture of bytes (R8UI, a line of the
 *     frame a line of it);
 *   - "cells": each 2x2 cell of the sensor unpacked into a texel of R, G (the mean of the two greens), B,
 *     above the black level (RGBA16F: halves and 10 bits exact), as camera_isp.c's cell();
 *   - "picture": each pixel of the picture between the four cells around it (render_line()'s magnified
 *     path, its coordinates to the float; the nearest cell where the picture is not bigger than the cells,
 *     as its other path), through the colour matrix with the gains (isp_preview()), the same 4096-step
 *     gamma table, and BT.601 Y and CbCr in nv12_pair()'s integers -- packed four bytes to a texel of an
 *     RGBA8UI picture: the Y lines, four pixels a texel, then the CbCr lines under them;
 *   - and its lines read back into the buffer's two planes (glReadPixels with the planes' strides).
 * The processor is left the frame's statistics (AE, white balance), the copy up and the copy back.
 *
 * Any failure -- no context, a software renderer, a shader that does not build, a frame buffer that is not
 * complete, an error after a picture, or a picture slower than LATE_NS -- says why and leaves the pictures
 * to the processor for the rest of the process's life.
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "camera_gpu.h"

void hk_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define say hk_camera_say

#define LATE_NS         50000000ull     /* LATE_IN_A_ROW pictures slower than this: the GPU is given up (supervisor, 2026-11-04) */
#define LATE_IN_A_ROW   5               /* (one alone, 97 ms, gave it up for a whole run: run 120049) */

/*
 * Boot-arg campattern=1: grey diagonal stripes instead of the camera's picture, 16 pixels further each picture -- two
 * pictures mixed in one shown (bands while the phone moved, runs 110059, 112756) break the stripes where they meet,
 * which a screenshot shows whether or not anything moves in front of the camera.
 */
static int pattern = -1;

/* GL/osmesa.h and GLES3/gl3.h, as far as they are used here */
#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
#define GL_NO_ERROR                     0
#define GL_TRIANGLES                    0x0004
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RENDERER                     0x1f01
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_CLAMP_TO_EDGE                0x812f
#define GL_UNPACK_ALIGNMENT             0x0cf5
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_TEXTURE0                     0x84c0
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER                  0x8d40
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_RGBA16F                      0x881a
#define GL_RGBA32F                      0x8814
#define GL_RGBA                         0x1908
#define GL_FLOAT                        0x1406
#define GL_R8UI                         0x8232
#define GL_RGBA8UI                      0x8d7c
#define GL_RED_INTEGER                  0x8d94
#define GL_RGBA_INTEGER                 0x8d99

typedef struct osmesa_context *OSMesaContext;

static struct {
	OSMesaContext   (*CreateContextAttribs)(const int *attributes, OSMesaContext share);
	unsigned char   (*MakeCurrent)(OSMesaContext context, void *buffer, unsigned type, int width, int height);
	const unsigned char *(*GetString)(unsigned name);
	unsigned        (*GetError)(void);
	void            (*GenTextures)(int n, unsigned *names);
	void            (*DeleteTextures)(int n, const unsigned *names);
	void            (*BindTexture)(unsigned target, unsigned texture);
	void            (*TexStorage2D)(unsigned target, int levels, unsigned format, int width, int height);
	void            (*TexSubImage2D)(unsigned target, int level, int x, int y, int width, int height, unsigned format,
	    unsigned type, const void *pixels);
	void            (*TexParameteri)(unsigned target, unsigned name, int value);
	void            (*PixelStorei)(unsigned name, int value);
	void            (*ActiveTexture)(unsigned unit);
	void            (*GenFramebuffers)(int n, unsigned *names);
	void            (*BindFramebuffer)(unsigned target, unsigned framebuffer);
	void            (*FramebufferTexture2D)(unsigned target, unsigned attachment, unsigned textarget, unsigned texture,
	    int level);
	unsigned        (*CheckFramebufferStatus)(unsigned target);
	void            (*Viewport)(int x, int y, int width, int height);
	unsigned        (*CreateShader)(unsigned type);
	void            (*ShaderSource)(unsigned shader, int count, const char *const *text, const int *lengths);
	void            (*CompileShader)(unsigned shader);
	void            (*GetShaderiv)(unsigned shader, unsigned name, int *value);
	void            (*GetShaderInfoLog)(unsigned shader, int size, int *length, char *text);
	unsigned        (*CreateProgram)(void);
	void            (*AttachShader)(unsigned program, unsigned shader);
	void            (*LinkProgram)(unsigned program);
	void            (*GetProgramiv)(unsigned program, unsigned name, int *value);
	void            (*GetProgramInfoLog)(unsigned program, int size, int *length, char *text);
	void            (*UseProgram)(unsigned program);
	int             (*GetUniformLocation)(unsigned program, const char *name);
	void            (*Uniform1i)(int location, int value);
	void            (*Uniform4f)(int location, float x, float y, float z, float w);
	void            (*Uniform2f)(int location, float x, float y);
	void            (*Uniform3f)(int location, float x, float y, float z);
	void            (*UniformMatrix3fv)(int location, int count, unsigned char transpose, const float *value);
	void            (*GenVertexArrays)(int n, unsigned *names);
	void            (*BindVertexArray)(unsigned array);
	void            (*DrawArrays)(unsigned mode, int first, int count);
	void            (*ReadPixels)(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
} gl;

static const struct {
	const char *name;
	size_t offset;
} gl_names[] = {
	{ "OSMesaCreateContextAttribs", offsetof(__typeof__(gl), CreateContextAttribs) },
	{ "OSMesaMakeCurrent", offsetof(__typeof__(gl), MakeCurrent) },
	{ "glGetString", offsetof(__typeof__(gl), GetString) }, { "glGetError", offsetof(__typeof__(gl), GetError) },
	{ "glGenTextures", offsetof(__typeof__(gl), GenTextures) }, { "glDeleteTextures", offsetof(__typeof__(gl), DeleteTextures) },
	{ "glBindTexture", offsetof(__typeof__(gl), BindTexture) }, { "glTexStorage2D", offsetof(__typeof__(gl), TexStorage2D) },
	{ "glTexSubImage2D", offsetof(__typeof__(gl), TexSubImage2D) }, { "glTexParameteri", offsetof(__typeof__(gl), TexParameteri) },
	{ "glPixelStorei", offsetof(__typeof__(gl), PixelStorei) }, { "glActiveTexture", offsetof(__typeof__(gl), ActiveTexture) },
	{ "glGenFramebuffers", offsetof(__typeof__(gl), GenFramebuffers) },
	{ "glBindFramebuffer", offsetof(__typeof__(gl), BindFramebuffer) },
	{ "glFramebufferTexture2D", offsetof(__typeof__(gl), FramebufferTexture2D) },
	{ "glCheckFramebufferStatus", offsetof(__typeof__(gl), CheckFramebufferStatus) },
	{ "glViewport", offsetof(__typeof__(gl), Viewport) }, { "glCreateShader", offsetof(__typeof__(gl), CreateShader) },
	{ "glShaderSource", offsetof(__typeof__(gl), ShaderSource) }, { "glCompileShader", offsetof(__typeof__(gl), CompileShader) },
	{ "glGetShaderiv", offsetof(__typeof__(gl), GetShaderiv) }, { "glGetShaderInfoLog", offsetof(__typeof__(gl), GetShaderInfoLog) },
	{ "glCreateProgram", offsetof(__typeof__(gl), CreateProgram) }, { "glAttachShader", offsetof(__typeof__(gl), AttachShader) },
	{ "glLinkProgram", offsetof(__typeof__(gl), LinkProgram) }, { "glGetProgramiv", offsetof(__typeof__(gl), GetProgramiv) },
	{ "glGetProgramInfoLog", offsetof(__typeof__(gl), GetProgramInfoLog) }, { "glUseProgram", offsetof(__typeof__(gl), UseProgram) },
	{ "glGetUniformLocation", offsetof(__typeof__(gl), GetUniformLocation) }, { "glUniform1i", offsetof(__typeof__(gl), Uniform1i) },
	{ "glUniform4f", offsetof(__typeof__(gl), Uniform4f) }, { "glUniformMatrix3fv", offsetof(__typeof__(gl), UniformMatrix3fv) },
	{ "glUniform2f", offsetof(__typeof__(gl), Uniform2f) }, { "glUniform3f", offsetof(__typeof__(gl), Uniform3f) },
	{ "glGenVertexArrays", offsetof(__typeof__(gl), GenVertexArrays) }, { "glBindVertexArray", offsetof(__typeof__(gl), BindVertexArray) },
	{ "glDrawArrays", offsetof(__typeof__(gl), DrawArrays) }, { "glReadPixels", offsetof(__typeof__(gl), ReadPixels) },
};

/* one triangle over the whole target, its corners from gl_VertexID (no buffers) */
static const char vertex_text[] =
    "#version 300 es\n"
    "void main() {\n"
    "  gl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
    "}\n";

/*
 * camera_isp.c's cell(): a texel a 2x2 cell, R, the greens' mean, B above black (64), none below 0; and, with the
 * lens shading on, times its gains there -- lsc_at()'s: the 17 x 13 grid (a texel a point, R G B) laid over the
 * part of the sensor the frame is (window: left, top, width, height), between its points bilinear; last is the
 * last cell's place (cells - 1), the far side of the frame. Its alpha the least a saturated cell comes to there after
 * the white balance (wb), WHITE (959) times the least of the gains times wb: the picture clips at it (pixel()'s clip).
 */
static const char cells_text[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp usampler2D raw;\n"
    "uniform highp sampler2D lsc;\n"
    "uniform int red, blue, shading;\n"
    "uniform vec4 window;\n"
    "uniform vec2 last;\n"
    "uniform vec3 wb;\n"
    "out vec4 cell;\n"
    "vec3 gains(ivec2 c) {\n"
    "  vec2 g = (window.xy + vec2(c) / max(last, vec2(1.0)) * window.zw) * vec2(16.0, 12.0);\n"
    "  ivec2 p = clamp(ivec2(g), ivec2(0), ivec2(15, 11));\n"
    "  vec2 f = g - vec2(p);\n"
    "  vec3 a = texelFetch(lsc, p, 0).rgb, b = texelFetch(lsc, p + ivec2(1, 0), 0).rgb;\n"
    "  vec3 d = texelFetch(lsc, p + ivec2(0, 1), 0).rgb, e = texelFetch(lsc, p + ivec2(1, 1), 0).rgb;\n"
    "  return (a * (1.0 - f.x) + b * f.x) * (1.0 - f.y) + (d * (1.0 - f.x) + e * f.x) * f.y;\n"
    "}\n"
    "int at(int x, int y) {\n"
    "  int g = (x >> 2) * 5, i = x & 3;\n"
    "  int msb = int(texelFetch(raw, ivec2(g + i, y), 0).r), lsb = int(texelFetch(raw, ivec2(g + 4, y), 0).r);\n"
    "  return max(((msb << 2) | ((lsb >> (2 * i)) & 3)) - 64, 0);\n"
    "}\n"
    "void main() {\n"
    "  ivec2 c = ivec2(gl_FragCoord.xy) * 2;\n"
    "  int v[4] = int[4](at(c.x, c.y), at(c.x + 1, c.y), at(c.x, c.y + 1), at(c.x + 1, c.y + 1));\n"
    "  int g = v[0] + v[1] + v[2] + v[3] - v[red] - v[blue];\n"
    "  vec3 k = shading != 0 ? gains(ivec2(gl_FragCoord.xy)) : vec3(1.0);\n"
    "  cell = vec4(vec3(float(v[red]), float(g) * 0.5, float(v[blue])) * k, 959.0 * min(min(k.r * wb.r, k.g * wb.g), k.b * wb.b));\n"
    "}\n";

/*
 * The picture, packed: line y < height of the target is the picture's line y, its texel x the Y of pixels
 * 4x .. 4x + 3; line height + j is the CbCr of the picture's lines 2j and 2j + 1, texel x those of pixels
 * 4x, 4x + 1 and 4x + 2, 4x + 3. render_line(), pixel() and nv12_pair() of camera_isp.c, step for step.
 */
static const char picture_text[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D cells;\n"
    "uniform highp usampler2D lut;\n"
    "uniform int width, height, turned, video, magnified, pattern;\n"
    "uniform vec4 crop;\n"      /* x0, y0, cw, ch, in cells */
    "uniform mat3 m;\n"
    "uniform vec3 wb;\n"        /* (the white balance, then the clip at the cell's alpha, then m: pixel()'s) */
    "out uvec4 bytes;\n"
    "ivec3 rgb(int x, int y) {\n"
    "  if (pattern >= 0) {\n"     /* (campattern=1: stripes moving 16 pixels a picture, see below) */
    "    return ((x + y + pattern * 16) & 32) != 0 ? ivec3(190) : ivec3(70);\n"
    "  }\n"
    "  ivec2 n = textureSize(cells, 0);\n"
    "  vec4 c;\n"
    "  if (turned != 0) { x = width - 1 - x; y = height - 1 - y; }\n"
    "  if (magnified != 0) {\n"
    "    float fx = crop.x + (float(x) + 0.5) * crop.z / float(width) - 0.5;\n"
    "    float fy = crop.y + (float(y) + 0.5) * crop.w / float(height) - 0.5;\n"
    "    int lo = max(int(crop.x) - 1, 0), hi = min(int(crop.x + crop.z), n.x - 1);\n"
    "    int cx0 = clamp(fx < 0.0 ? 0 : int(fx), lo, hi), cx1 = min(cx0 + 1, hi);\n"
    "    int cy0 = min(fy < 0.0 ? 0 : int(fy), n.y - 1), cy1 = min(cy0 + 1, n.y - 1);\n"
    "    float tx = fx < float(cx0) ? 0.0 : fx - float(cx0), ty = fy < float(cy0) ? 0.0 : fy - float(cy0);\n"
    "    vec4 a = texelFetch(cells, ivec2(cx0, cy0), 0), b = texelFetch(cells, ivec2(cx1, cy0), 0);\n"
    "    vec4 d = texelFetch(cells, ivec2(cx0, cy1), 0), e = texelFetch(cells, ivec2(cx1, cy1), 0);\n"
    "    c = (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (d * (1.0 - tx) + e * tx) * ty;\n"
    "  } else {\n"
    "    int step = (int(crop.z) << 16) / width;\n"      /* (render_line()'s 16.16 steps across) */
    "    int cx = ((int(crop.x) << 16) + x * step) >> 16, cy = int(crop.y) + y * int(crop.w) / height;\n"
    "    c = texelFetch(cells, ivec2(cx, cy), 0);\n"
    "  }\n"
    "  ivec3 i = clamp(ivec3(floor(m * min(c.rgb * wb, vec3(c.a)) + 0.5)), ivec3(0), ivec3(4095));\n"
    "  return ivec3(int(texelFetch(lut, ivec2(i.r, 0), 0).r), int(texelFetch(lut, ivec2(i.g, 0), 0).r),\n"
    "      int(texelFetch(lut, ivec2(i.b, 0), 0).r));\n"
    "}\n"
    "int luma(ivec3 p) {\n"
    "  return video != 0 ? (16829 * p.r + 33039 * p.g + 6416 * p.b + (16 << 16) + (1 << 15)) >> 16 :\n"
    "      (19595 * p.r + 38470 * p.g + 7471 * p.b + (1 << 15)) >> 16;\n"
    "}\n"
    "ivec2 chroma(ivec3 s) {\n"
    "  ivec3 k = video != 0 ? ivec3(-9714, -19070, 28784) : ivec3(-11058, -21710, 32768);\n"
    "  ivec3 l = video != 0 ? ivec3(28784, -24103, -4681) : ivec3(32768, -27439, -5329);\n"
    "  int cb = (k.r * s.r + k.g * s.g + k.b * s.b + (128 << 18) + (1 << 17) - 1) >> 18;\n"
    "  int cr = (l.r * s.r + l.g * s.g + l.b * s.b + (128 << 18) + (1 << 17) - 1) >> 18;\n"
    "  return clamp(ivec2(cb, cr), ivec2(0), ivec2(255));\n"
    "}\n"
    "void main() {\n"
    "  ivec2 t = ivec2(gl_FragCoord.xy);\n"
    "  int x = t.x * 4;\n"
    "  if (t.y < height) {\n"
    "    bytes = uvec4(luma(rgb(x, t.y)), luma(rgb(x + 1, t.y)), luma(rgb(x + 2, t.y)), luma(rgb(x + 3, t.y)));\n"
    "  } else {\n"
    "    int y = (t.y - height) * 2;\n"
    "    ivec2 a = chroma(rgb(x, y) + rgb(x + 1, y) + rgb(x, y + 1) + rgb(x + 1, y + 1));\n"
    "    ivec2 b = chroma(rgb(x + 2, y) + rgb(x + 3, y) + rgb(x + 2, y + 1) + rgb(x + 3, y + 1));\n"
    "    bytes = uvec4(a, b);\n"
    "  }\n"
    "}\n";

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool given_up, made;
static OSMesaContext context;
static pthread_t current;               /* the thread the context is current on (when `bound') */
static bool bound;
static unsigned char window[16 * 16 * 4];
static unsigned cells_program, picture_program, vertices;
static unsigned raw_texture, cells_texture, picture_texture, lut_texture, lsc_texture, cells_fb, picture_fb;
static uint32_t raw_w, raw_h, cells_w, cells_h, picture_w, picture_h;
static bool remade;                     /* this picture's textures made anew (a new size): its time not counted */
static char renderer[96];

static void
give_up(const char *why)
{
	given_up = true;
	say("the GPU's pictures given up: %s; the processor makes them from now on", why);
}

static unsigned
shader(unsigned type, const char *text)
{
	unsigned s = gl.CreateShader(type);
	int ok = 0;

	gl.ShaderSource(s, 1, &text, NULL);
	gl.CompileShader(s);
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[400] = "";

		gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
		say("gpu: a %s shader does not build: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		return 0;
	}
	return s;
}

static unsigned
program(const char *fragment)
{
	unsigned v = shader(GL_VERTEX_SHADER, vertex_text), f = shader(GL_FRAGMENT_SHADER, fragment), p;
	int ok = 0;

	if (v == 0 || f == 0) {
		return 0;
	}
	p = gl.CreateProgram();
	gl.AttachShader(p, v);
	gl.AttachShader(p, f);
	gl.LinkProgram(p);
	gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[400] = "";

		gl.GetProgramInfoLog(p, sizeof(log), NULL, log);
		say("gpu: a program does not link: %s", log);
		return 0;
	}
	return p;
}

static void
texture_params(void)
{
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* a texture of (w, h) in `format' (made anew when the size changes), and, with fb, a frame buffer drawing into it */
static bool
target(unsigned *texture, unsigned *fb, uint32_t *have_w, uint32_t *have_h, uint32_t w, uint32_t h, unsigned format,
    const char *what)
{
	if (*texture != 0 && *have_w == w && *have_h == h) {
		return true;
	}
	if (*texture != 0) {
		gl.DeleteTextures(1, texture);
	}
	remade = true;
	gl.GenTextures(1, texture);
	gl.BindTexture(GL_TEXTURE_2D, *texture);
	gl.TexStorage2D(GL_TEXTURE_2D, 1, format, (int)w, (int)h);
	texture_params();
	*have_w = w;
	*have_h = h;
	if (fb != NULL) {
		if (*fb == 0) {
			gl.GenFramebuffers(1, fb);
		}
		gl.BindFramebuffer(GL_FRAMEBUFFER, *fb);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
		if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			say("gpu: the %s (%ux%u) cannot be drawn into", what, w, h);
			return false;
		}
	}
	return gl.GetError() == GL_NO_ERROR;
}

/* the library, a context, the programs and the gamma table: once */
static bool
make(const struct isp *isp)
{
	static const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 3,
		OSMESA_CONTEXT_MINOR_VERSION, 0, 0
	};
	void *library = dlopen("/hackios/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_LOCAL);
	const char *name;

	made = true;
	if (library == NULL) {
		give_up("no Mesa in the image (/hackios/lib/libOSMesa.8.dylib)");
		return false;
	}
	for (size_t i = 0; i < sizeof(gl_names) / sizeof(gl_names[0]); i++) {
		void *function = dlsym(library, gl_names[i].name);

		if (function == NULL) {
			char why[96];

			snprintf(why, sizeof(why), "Mesa has no %s", gl_names[i].name);
			give_up(why);
			return false;
		}
		memcpy((char *)&gl + gl_names[i].offset, &function, sizeof(function));
	}
	if ((context = gl.CreateContextAttribs(attributes, NULL)) == NULL ||
	    !gl.MakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		give_up("no OpenGL ES 3 context");
		return false;
	}
	current = pthread_self();
	bound = true;
	name = (const char *)gl.GetString(GL_RENDERER);
	snprintf(renderer, sizeof(renderer), "%s", name != NULL ? name : "?");
	if (name == NULL || strstr(name, "softpipe") != NULL || strstr(name, "llvmpipe") != NULL) {
		give_up("Mesa has no GPU here (a software renderer)");
		return false;
	}
	if ((cells_program = program(cells_text)) == 0 || (picture_program = program(picture_text)) == 0) {
		give_up("its shaders do not build");
		return false;
	}
	gl.GenVertexArrays(1, &vertices);
	gl.GenTextures(1, &lut_texture);
	gl.BindTexture(GL_TEXTURE_2D, lut_texture);
	gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, 4096, 1);
	texture_params();
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4096, 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE, isp->gamma);
	if (gl.GetError() != GL_NO_ERROR) {
		give_up("its gamma table cannot be made");
		return false;
	}
	gl.GenTextures(1, &lsc_texture);
	gl.BindTexture(GL_TEXTURE_2D, lsc_texture);
	gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, LSC_W, LSC_H);
	texture_params();
	if (gl.GetError() != GL_NO_ERROR) {
		give_up("its lens shading's grid cannot be made");
		return false;
	}
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "campattern=1") != NULL) {
			pattern = 0;
			say("campattern=1: the GPU draws moving stripes, not the camera's picture");
		}
	}
	say("the moving pictures by the GPU (%s, OpenGL ES 3 of Mesa in this process)", renderer);
	return true;
}

static uint64_t
ns(uint64_t t)
{
	static mach_timebase_info_data_t timebase;

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return t * timebase.numer / timebase.denom;
}

bool
gpu_render_nv12(const struct isp *isp, const struct hk_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, bool video_range, uint64_t *took)
{
	struct isp_preview pv;
	uint64_t began = mach_absolute_time();
	float m[9];
	bool done = false;

	if (width % 4 != 0 || height % 2 != 0 || y_stride % 4 != 0 || uv_stride % 4 != 0 || f->format != 1 ||
	    !isp_preview(isp, f, width, height, &pv)) {
		return false;
	}
	pthread_mutex_lock(&lock);
	remade = !made;
	if (given_up || (!made && !make(isp))) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (!bound || !pthread_equal(current, pthread_self())) {
		if (!gl.MakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
			give_up("its context cannot be made this thread's");
			pthread_mutex_unlock(&lock);
			return false;
		}
		current = pthread_self();
		bound = true;
	}
	/* the frame's bytes up */
	gl.ActiveTexture(GL_TEXTURE0);
	if (!target(&raw_texture, NULL, &raw_w, &raw_h, f->stride, f->height, GL_R8UI, "frame")) {
		give_up("the frame's texture cannot be made");
		goto out;
	}
	gl.BindTexture(GL_TEXTURE_2D, raw_texture);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)f->stride, (int)f->height, GL_RED_INTEGER, GL_UNSIGNED_BYTE, raw);
	/* the cells */
	gl.ActiveTexture(GL_TEXTURE0 + 1);
	if (!target(&cells_texture, &cells_fb, &cells_w, &cells_h, pv.cells_w, pv.cells_h, GL_RGBA16F, "cells")) {
		give_up("its cells cannot be drawn");
		goto out;
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, cells_fb);
	gl.Viewport(0, 0, (int)pv.cells_w, (int)pv.cells_h);
	gl.UseProgram(cells_program);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, raw_texture);
	gl.Uniform1i(gl.GetUniformLocation(cells_program, "raw"), 0);
	gl.Uniform1i(gl.GetUniformLocation(cells_program, "red"), pv.red);
	gl.Uniform1i(gl.GetUniformLocation(cells_program, "blue"), pv.blue);
	gl.Uniform1i(gl.GetUniformLocation(cells_program, "shading"), pv.shading);
	gl.Uniform3f(gl.GetUniformLocation(cells_program, "wb"), pv.wb[0], pv.wb[1], pv.wb[2]);
	if (pv.shading) {
		/* the grid as it is for this frame (isp_tune() moves it with the light): a texel a point */
		float grid[LSC_H][LSC_W][4];

		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				for (int c = 0; c < 3; c++) {
					grid[gy][gx][c] = pv.lsc->gain[c][gy][gx];
				}
				grid[gy][gx][3] = 1;
			}
		}
		gl.ActiveTexture(GL_TEXTURE0 + 4);
		gl.BindTexture(GL_TEXTURE_2D, lsc_texture);
		gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
		gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LSC_W, LSC_H, GL_RGBA, GL_FLOAT, grid);
		gl.Uniform1i(gl.GetUniformLocation(cells_program, "lsc"), 4);
		gl.Uniform4f(gl.GetUniformLocation(cells_program, "window"), pv.window[0], pv.window[1], pv.window[2], pv.window[3]);
		gl.Uniform2f(gl.GetUniformLocation(cells_program, "last"), (float)(pv.cells_w - 1), (float)(pv.cells_h - 1));
		gl.ActiveTexture(GL_TEXTURE0);
	}
	gl.BindVertexArray(vertices);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	/* the picture */
	gl.ActiveTexture(GL_TEXTURE0 + 3);
	if (!target(&picture_texture, &picture_fb, &picture_w, &picture_h, width / 4, height + height / 2, GL_RGBA8UI, "picture")) {
		give_up("its picture cannot be drawn");
		goto out;
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, picture_fb);
	gl.Viewport(0, 0, (int)(width / 4), (int)(height + height / 2));
	gl.UseProgram(picture_program);
	gl.ActiveTexture(GL_TEXTURE0 + 1);
	gl.BindTexture(GL_TEXTURE_2D, cells_texture);
	gl.ActiveTexture(GL_TEXTURE0 + 2);
	gl.BindTexture(GL_TEXTURE_2D, lut_texture);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "cells"), 1);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "lut"), 2);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "width"), (int)width);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "height"), (int)height);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "turned"), isp->turned);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "video"), video_range);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "magnified"), pv.cw < width && pv.ch < height);
	gl.Uniform1i(gl.GetUniformLocation(picture_program, "pattern"), pattern < 0 ? -1 : pattern++ % 4096);
	gl.Uniform4f(gl.GetUniformLocation(picture_program, "crop"), (float)pv.x0, (float)pv.y0, (float)pv.cw, (float)pv.ch);
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			m[j * 3 + c] = pv.m[c][j];      /* (by columns) */
		}
	}
	gl.UniformMatrix3fv(gl.GetUniformLocation(picture_program, "m"), 1, 0, m);
	gl.Uniform3f(gl.GetUniformLocation(picture_program, "wb"), pv.wb[0], pv.wb[1], pv.wb[2]);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	/* its lines back, into the planes */
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, (int)(y_stride / 4));
	gl.ReadPixels(0, 0, (int)(width / 4), (int)height, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, y);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, (int)(uv_stride / 4));
	gl.ReadPixels(0, (int)height, (int)(width / 4), (int)(height / 2), GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, uv);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	{
		unsigned error = gl.GetError();
		uint64_t spent = ns(mach_absolute_time() - began);
		static unsigned late;           /* pictures in a row slower than LATE_NS */

		if (error != GL_NO_ERROR) {
			char why[64];

			snprintf(why, sizeof(why), "OpenGL error 0x%x", error);
			give_up(why);
		} else if (spent > LATE_NS && !remade && ++late >= LATE_IN_A_ROW) {
			char why[64];

			snprintf(why, sizeof(why), "%u pictures in a row over %llu ms (the last %llu ms)", late,
			    LATE_NS / 1000000, (unsigned long long)(spent / 1000000));
			give_up(why);
		} else {
			if (spent > LATE_NS && !remade) {
				say("gpu: a picture took %llu ms (%u in a row)", (unsigned long long)(spent / 1000000), late);
			} else {
				late = 0;
			}
			done = true;
			*took = spent;
		}
	}
out:
	pthread_mutex_unlock(&lock);
	return done;
}

void
gpu_thread_done(void)
{
	pthread_mutex_lock(&lock);
	if (bound && pthread_equal(current, pthread_self())) {
		gl.MakeCurrent(NULL, NULL, 0, 0, 0);
		bound = false;
	}
	pthread_mutex_unlock(&lock);
}

const char *
gpu_renderer(void)
{
	return renderer;
}
