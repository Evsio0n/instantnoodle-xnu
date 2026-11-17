#ifndef HK_FRONT_DIAG_H
#define HK_FRONT_DIAG_H
#include <stdint.h>
struct lc_diag {
 uint64_t uid_lo,uid_hi,see_ticks,ap_observed_ticks;
 uint32_t version,bytes,seq,status,lux_bits,c_bits,period_bits,fields;
 uint32_t active,attr_qualified,cal_path_bound,per_unit_calibrated,front_verified;
};
#endif
