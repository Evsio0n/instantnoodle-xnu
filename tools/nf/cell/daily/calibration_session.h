#ifndef HK_ALS_CALIBRATION_SESSION_H
#define HK_ALS_CALIBRATION_SESSION_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
struct hk_als_completion {
 uint8_t result,version;uint16_t reserved;
 uint32_t magic;int64_t boot_sec;int32_t boot_usec;uint32_t reserved2;
};
_Static_assert(sizeof(struct hk_als_completion)==24,"ALS completion wire size");
static inline bool hk_als_completion_matches(const struct hk_als_completion *r,int64_t sec,int32_t usec){
 return r && (r->result=='0'||r->result=='1') && r->version==1 && !r->reserved && !r->reserved2 &&
  r->magic==0x534c4143u && r->boot_sec==sec && r->boot_usec==usec && sec>0 && usec>=0 && usec<1000000;
}
#endif
