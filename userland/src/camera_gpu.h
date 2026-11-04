/*
 * The moving pictures made by the phone's GPU instead of the processor (camera_gpu.c): the same
 * picture as camera_isp.c's isp_render_nv12(), by OpenGL ES 3 through the image's Mesa (freedreno).
 */
#ifndef CAMERA_GPU_H
#define CAMERA_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera_isp.h"

/*
 * The picture of frame f into an NV12 buffer's two planes (y: height lines of width bytes, uv: height / 2
 * lines), on the calling thread (the GPU's context is made its own). False: not done -- no GPU, a
 * picture it does not do (lens shading, a width not a multiple of 4, an odd height), or it has failed
 * or been too slow once (then never again in this process, said why); the processor's renderer is to
 * do it. `took': the time it took, in nanoseconds.
 */
bool gpu_render_nv12(const struct isp *isp, const struct hk_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, bool video_range, uint64_t *took);

/* the calling thread is leaving: the context is no longer its own */
void gpu_thread_done(void);

/* where the GPU's pictures come from, for the log ("" until a context is made) */
const char *gpu_renderer(void);

#endif
