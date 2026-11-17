/* Prefer an already associated RF channel only when the real peer advertises
 * it. This chooses a carrier; the ordinary guarded peer-window and actual
 * firmware ROC checks still govern every submission. */
#ifndef HK_AWDL_HOME_CARRIER_H
#define HK_AWDL_HOME_CARRIER_H
#include <stdint.h>
#include <stdbool.h>
static inline unsigned hk_awdl_home_carrier(const uint8_t slots[16],
    unsigned home,bool eligible)
{
    if (!slots || !home || !eligible) return 0;
    for (unsigned i=0;i<16;i++) if (slots[i]==home) return home;
    return 0;
}
#endif
