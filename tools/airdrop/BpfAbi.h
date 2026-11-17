#ifndef HK_BPF_ABI_H
#define HK_BPF_ABI_H
/* Minimal userspace ABI from XNU8796.121.2 bsd/net/bpf.h. The iPhone SDK
 * omits this public kernel interface; keep the 32-bit wire timeval on LP64. */
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <net/if.h>
struct hk_bpf_insn {uint16_t code;uint8_t jt,jf;uint32_t k;};
struct hk_bpf_program {unsigned bf_len;struct hk_bpf_insn *bf_insns;};
struct hk_bpf_stat {unsigned bs_recv,bs_drop;};
struct hk_bpf_hdr {int32_t seconds,microseconds;uint32_t bh_caplen,bh_datalen;uint16_t bh_hdrlen;};
#define HK_BPF_WORDALIGN(x) (((x)+3u)&~3u)
#define HK_BIOCGBLEN _IOR('B',102,unsigned)
#define HK_BIOCSBLEN _IOWR('B',102,unsigned)
#define HK_BIOCSETF _IOW('B',103,struct hk_bpf_program)
#define HK_BIOCSETIF _IOW('B',108,struct ifreq)
#define HK_BIOCGSTATS _IOR('B',111,struct hk_bpf_stat)
#define HK_BIOCIMMEDIATE _IOW('B',112,unsigned)
#define HK_BIOCSSEESENT _IOW('B',119,unsigned)
_Static_assert(offsetof(struct hk_bpf_hdr,bh_hdrlen)+sizeof(uint16_t)==18,"Darwin BPF wire header");
_Static_assert(sizeof(struct hk_bpf_insn)==8,"BPF instruction ABI");
#endif
