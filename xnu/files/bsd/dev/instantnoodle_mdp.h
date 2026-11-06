/*
 * Our own MDP layers on the OnePlus 8: what iokit/Kernel/MiniIOMobileFramebuffer.cpp and iokit/Kernel/MiniIOSurface.cpp call in
 * bsd/dev/instantnoodle_mdp.c. docs/research/mdp-driver-plan.md.
 */
#ifndef _BSD_DEV_INSTANTNOODLE_MDP_H_
#define _BSD_DEV_INSTANTNOODLE_MDP_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One layer of a frame. x, y, width, height; source in the surface's pixels, destination on the panel. */
struct hk_mdp_layer {
	uint32_t format;                /* the surface's pixel format, as a FourCC */
	uint32_t address[2];            /* its planes, in the MDP's addresses (hk_mdp_range()) */
	uint32_t stride[2];
	uint32_t sx, sy, sw, sh;
	uint32_t dx, dy, dw, dh;
};

#define HK_MDP_LAYERS           3

uint32_t hk_mdp_level(void);                    /* boot-arg mdp */
void hk_mdp_start(vm_offset_t dpu);             /* the framebuffer's start, autorefresh still fetching */
void hk_mdp_after_push(void);                   /* after each of the window server's pushes */
uint32_t hk_mdp_flush_bits(void);               /* what goes with the frame's own flush */
void hk_mdp_restore(void);                      /* the found state again, from any context */

/* The display's own context bank (under the panel's lock, nothing in flight). false: refused, nothing changed. */
bool hk_mdp_smmu_up(void);
void hk_mdp_smmu_down(void);
uint32_t hk_mdp_smmu_fsr(void);                 /* the bank's fault status, said once a fault, and cleared; 0: none */
bool hk_mdp_smmu_is_up(void);

/* The MDP's addresses for memory outside the display's own. */
uint32_t hk_mdp_range(uint32_t pages);          /* 0: none left */
void hk_mdp_range_page(uint32_t address, uint64_t phys);
void hk_mdp_range_ready(void);
void hk_mdp_range_free(uint32_t address, uint32_t pages);

/* A frame's layers, lowest first, into the pipes (under the panel's lock, nothing in flight). false: they cannot all be taken. */
bool hk_mdp_can_take(const struct hk_mdp_layer *layers, unsigned count);
bool hk_mdp_commit(const struct hk_mdp_layer *layers, unsigned count);
/* The firmware's two pipes back on a framebuffer, rows of `stride` bytes (under the panel's lock). */
void hk_mdp_leave_pipes(uint32_t stride);
bool hk_mdp_on_pipes(void);                     /* the frame is in the pipes as hk_mdp_commit() left them */
void hk_mdp_say_stats(uint32_t frames, uint32_t layered);

#ifdef __cplusplus
}
#endif

#endif /* _BSD_DEV_INSTANTNOODLE_MDP_H_ */
