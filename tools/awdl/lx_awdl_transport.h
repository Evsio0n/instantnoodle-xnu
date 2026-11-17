#ifndef LX_AWDL_TRANSPORT_H
#define LX_AWDL_TRANSPORT_H

#include <stdint.h>

/* The radio image has no XNU sys/hk_awdl_provider.h. Only the six-byte
 * channel reply crosses this boundary. The shared declaration checks both
 * native and portable layouts before either caller can compile. */
#ifndef HK_AWDL_APPLE80211_PROVIDER_H
struct hk_awdl_channels {
    uint16_t preferred;
    uint16_t peer_master;
    uint8_t extension[2];
};
#endif

#define LX_AWDL_PORTABLE_VIEW 1
#include "ifnet_transport.h"
#undef LX_AWDL_PORTABLE_VIEW
#endif
