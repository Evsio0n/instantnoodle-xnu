#ifndef HK_AWDL_UNICAST_HOLD_H
#define HK_AWDL_UNICAST_HOLD_H
#include <stdint.h>
#include <stdbool.h>
/* Discovery is real traffic, but is not an active user data transfer.
 * Periodic query renewal must never keep all other group recipients out. */
static inline uint64_t hk_awdl_unicast_hold(uint64_t previous,uint64_t now,
                                           bool unicast,bool discovery){
 return unicast && !discovery?now+10000000ull:previous;
}
#endif
