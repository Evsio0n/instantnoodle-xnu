#ifndef HK_AWDL_APPLE80211_PROVIDER_H
#define HK_AWDL_APPLE80211_PROVIDER_H

#include <sys/types.h>
#include <stdbool.h>

#define HK_AWDL_NAME_SIZE 16
/* Optional diagnostic caller guard in the outer scalar. This does not
 * change any stock request or nested Apple payload. GET12 with GUARD
 * returns ACK in that scalar on this driver only. */
#define HK_AWDL_DIAG_GUARD 0x484b4131
#define HK_AWDL_DIAG_ACK   0x484b4132

struct hk_awdl_channels {
    uint16_t preferred;
    uint16_t peer_master;
    uint8_t extension[2];
};

/*
 * All callbacks may sleep. They are serialized, called without the control
 * mutex, and must finish within a bound on the provider's radio worker.
 * The provider owns its AF/peer state, ifnet, IORegistry publication and
 * async completions. Never fulfill an old lease from a new radio generation.
 */
struct hk_awdl_ops {
    /* Return only after both real interfaces (BSD and IORegistry) are live.
     * name must be the actual attached BSD name, NUL-terminated. */
    int (*create)(void *context, uint64_t lease, char name[HK_AWDL_NAME_SIZE]);
    /* Idempotent for this lease, including a partial/failed create. Zero
     * certifies firmware retirement, drained callbacks and actual detachment.
     * If ownership is uncertain return an errno and retain it for a retry. */
    int (*remove)(void *context, uint64_t lease);
    /* Return the actual current snapshot, including zero/unspecified channel
     * fields. Report ENODATA only when a snapshot is unavailable. A peer
     * need not exist for discovery or for interface creation. */
    int (*channels)(void *context, uint64_t lease, struct hk_awdl_channels *status);
};

int hk_awdl_provider_register(const struct hk_awdl_ops *ops, void *context, uint64_t *generation);
/* Publish readiness only after all dependencies for a usable AWDL interface
 * exist. Clear it on loss. A disabled provider can still release a lease. */
int hk_awdl_provider_set_ready(uint64_t generation, bool ready);
/* Close admission, then remove the current lease. EBUSY means a callback is
 * still running; retry after it returns. On any error keep storage and the
 * worker alive. Zero is the first point where the provider can be released. */
int hk_awdl_provider_unregister(uint64_t generation);

#endif
