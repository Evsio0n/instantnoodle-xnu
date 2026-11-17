#ifndef HK_AWDL_IFNET_SHIM_H
#define HK_AWDL_IFNET_SHIM_H
/* Host stand-ins for memory, synchronization and DLIL ownership only. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include "../ifnet_transport.h"

typedef int errno_t;
typedef struct fake_interface *ifnet_t;
typedef struct fake_mbuf {
    uint8_t *bytes;
    size_t size, skip;
    ifnet_t received_on;
    void *header;
} *mbuf_t;
struct ifnet_stat_increment_param { unsigned packets_in, bytes_in; };
struct ifreq {
    struct { int ifdm_current, ifdm_min, ifdm_max; } ifr_devmtu;
    int ifr_mtu;
};
struct ifmediareq {
    char ifm_name[16];
    int ifm_current, ifm_mask, ifm_status, ifm_active, ifm_count;
};
struct ifmediareq32 { struct ifmediareq common; uint32_t ifmu_ulist; };
struct ifmediareq64 { struct ifmediareq common; uint64_t ifmu_ulist; };
struct sockaddr_dl { uint8_t sdl_len, sdl_family, sdl_type, sdl_alen, bytes[6]; };
#define LLADDR(a) ((a)->bytes)
enum {
    SIOCSIFADDR = 1, SIOCSIFFLAGS, SIOCGIFDEVMTU, SIOCSIFMTU, SIOCADDMULTI,
    SIOCDELMULTI, SIOCGIFMEDIA32, SIOCGIFMEDIA64, IFM_AUTO, IFM_IEEE80211,
    IFM_AVALID, IFM_ACTIVE, IFNET_INIT_CURRENT_VERSION, IFNET_INIT_LEGACY,
    IFNET_INIT_NX_NOAUTO, IFNET_FAMILY_ETHERNET, IFNET_SUBFAMILY_WIFI,
    DLT_EN10MB, AF_LINK, PF_INET6, IFT_ETHER, Z_NOWAIT, MBUF_DONTWAIT
};
enum { IFF_UP=1, IFF_RUNNING=64, IFF_BROADCAST=2, IFF_MULTICAST=0x8000,
       IFF_NOARP=128, IFF_SIMPLEX=0x800, IFEF_AWDL=0x2000,
       ETHER_HDR_LEN=14, ETHER_ADDR_LEN=6, ETHERMTU=1500, IFNAMSIZ=16 };
struct ifnet_init_eparams {
    unsigned ver, len, flags, unit, family, subfamily, uniqueid_len, type, broadcast_len;
    const char *name, *uniqueid;
    const uint8_t *broadcast_addr;
    void *demux, *add_proto, *del_proto, *check_multi, *framer_extended;
    int (*output)(ifnet_t, mbuf_t);
    int (*ioctl)(ifnet_t, unsigned long, void *);
    void (*detach)(ifnet_t);
};
static const uint8_t etherbroadcastaddr[6] = {255,255,255,255,255,255};
#define ether_demux NULL
#define ether_add_proto NULL
#define ether_del_proto NULL
#define ether_check_multi NULL
#define ether_frameout_extended NULL
struct fake_interface {
    struct ifnet_init_eparams params;
    unsigned mtu, flags, eflags, references;
    bool attached, detached, released;
};
static struct {
    struct fake_interface pool[24], foreign;
    unsigned searches, borrow, give_back, foreign_on_search;
    unsigned allocations, releases, detaches, freed, inputs, outputs, rx_errors, tx_errors;
    unsigned link_up, link_down;
    bool hold_detach;
    int allocate_error, attach_error, plumb_error, input_error, registry_error;
    uint64_t published;
    ifnet_t pending;
} kpi;
static unsigned awif_notice_adds, awif_notice_removes;
static int awif_notice_error;
static int awif_peer_notice(ifnet_t ifp, bool present, const uint8_t mac[6], const uint8_t ip[16]) {
    assert(ifp && ifp->attached && mac && ip);
    if (awif_notice_error) return awif_notice_error;
    if (present) ++awif_notice_adds;
    else ++awif_notice_removes;
    return 0;
}
#define LCK_GRP_DECLARE(name,desc) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name,group) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock(m) assert(pthread_mutex_lock(m) == 0)
#define lck_mtx_unlock(m) assert(pthread_mutex_unlock(m) == 0)
static pthread_cond_t detach_cond = PTHREAD_COND_INITIALIZER;
static void wakeup(void *address) {
    (void)address;
    assert(pthread_cond_broadcast(&detach_cond) == 0);
}
static int msleep(void *address, pthread_mutex_t *mutex, int priority, const char *name,
                  const struct timespec *bound) {
    (void)address; (void)priority; (void)name;
    assert(bound->tv_sec == 1);
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_nsec += 10000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(&detach_cond, mutex, &until);
}
static int copyout(const void *from, uintptr_t to, size_t n) {
    if (!to) return EFAULT;
    memcpy((void *)to, from, n);
    return 0;
}
#define kalloc_data(n,opts) malloc(n)
#define kfree_data(p,n) free(p)
static int mbuf_allocpacket(int opt, size_t size, unsigned *chunks, mbuf_t *result) {
    (void)opt;
    assert(*chunks == 1);
    mbuf_t m = calloc(1, sizeof(*m));
    assert(m);
    m->bytes = calloc(size, 1);
    assert(m->bytes);
    m->size = size;
    *result = m;
    return 0;
}
static void mbuf_freem(mbuf_t m) { assert(m); free(m->bytes); free(m); kpi.freed++; }
static size_t mbuf_len(mbuf_t m) { return m->size - m->skip; }
#define mbuf_pkthdr_len(m) mbuf_len(m)
#define mbuf_data(m) ((m)->bytes + (m)->skip)
#define mbuf_pkthdr_setrcvif(m,ifp) ((m)->received_on = (ifp))
#define mbuf_pkthdr_setheader(m,hdr) ((m)->header = (hdr))
static void mbuf_adj(mbuf_t m, size_t n) { assert(n <= mbuf_len(m)); m->skip += n; }
static int mbuf_copyback(mbuf_t m, size_t pos, size_t n, const void *data, int opt) {
    (void)opt;
    assert(pos + n <= mbuf_len(m));
    memcpy(mbuf_data(m) + pos, data, n);
    return 0;
}
static int mbuf_copydata(mbuf_t m, size_t pos, size_t n, void *data) {
    assert(pos + n <= mbuf_len(m));
    memcpy(data, mbuf_data(m) + pos, n);
    return 0;
}
static int ifnet_allocate_extended(const struct ifnet_init_eparams *params, ifnet_t *result) {
    if (kpi.allocate_error) return kpi.allocate_error;
    assert(kpi.allocations < 24);
    ifnet_t i = &kpi.pool[kpi.allocations++];
    i->params = *params;
    i->references = 1;
    *result = i;
    return 0;
}
static void awif_link_event(ifnet_t i, bool up) {
    assert(i && !i->released);
    if (up) kpi.link_up++;
    else kpi.link_down++;
}
static int ifnet_find_by_name(const char *name, ifnet_t *result) {
    *result = NULL;
    if (!name) return EINVAL;
    kpi.searches++;
    if (strcmp(name, "awdl0")) return ENXIO;
    if (kpi.foreign_on_search && kpi.searches >= kpi.foreign_on_search) {
        *result = &kpi.foreign;
    } else {
        for (unsigned index = 0; index < kpi.allocations; index++) {
            if (kpi.pool[index].attached) {
                *result = &kpi.pool[index];
                break;
            }
        }
    }
    if (!*result) return ENXIO;
    assert((*result)->attached && !(*result)->released && (*result)->references);
    (*result)->references++;
    kpi.borrow++;
    return 0;
}
static int ifnet_release(ifnet_t i) {
    assert(i && !i->released && i->references > 0);
    if (--i->references) {
        kpi.give_back++;
        return 0;
    }
    assert(!i->attached && i != &kpi.foreign);
    i->released = true;
    kpi.releases++;
    return 0;
}
static int ifnet_set_addrlen(ifnet_t i, unsigned n) { assert(i && n == 6); return 0; }
static int ifnet_set_hdrlen(ifnet_t i, unsigned n) { assert(i && n == 14); return 0; }
static int ifnet_set_mtu(ifnet_t i, unsigned n) { i->mtu = n; return 0; }
static int ifnet_set_flags(ifnet_t i, unsigned f, unsigned mask) {
    i->flags = (i->flags & ~mask) | (f & mask);
    return 0;
}
static int ifnet_set_eflags(ifnet_t i, unsigned f, unsigned mask) {
    i->eflags = (i->eflags & ~mask) | (f & mask);
    return 0;
}
#define ifnet_flags(i) ((i)->flags)
#define ifnet_mtu(i) ((i)->mtu)
#define ifnet_name(i) ((i)->params.name)
#define ifnet_unit(i) ((i)->params.unit)
static int ifnet_attach(ifnet_t i, const struct sockaddr_dl *addr) {
    assert(!i->attached && addr->sdl_alen == 6);
    if (kpi.attach_error) return kpi.attach_error;
    i->attached = true;
    return 0;
}
static void finish_detach(ifnet_t i) {
    assert(i->attached && !i->released);
    i->attached = false;
    i->detached = true;
    i->params.detach(i);
}
static int ifnet_detach(ifnet_t i) {
    assert(i->attached && !i->detached);
    kpi.detaches++;
    if (kpi.hold_detach) kpi.pending = i;
    else finish_detach(i);
    return 0;
}
static int proto_plumb(int protocol, ifnet_t i) {
    assert(protocol == PF_INET6 && i->attached);
    return kpi.plumb_error;
}
#define bpfattach(i,d,h) ((void)0)
#define bpf_tap_out(i,d,m,h,n) ((void)0)
#define bpf_tap_in(i,d,m,h,n) ((void)0)
static void ifnet_stat_increment_out(ifnet_t i, unsigned sent, size_t bytes, unsigned errors) {
    assert(i->attached && ((sent && bytes >= 54) || errors));
    kpi.outputs += sent;
    kpi.tx_errors += errors;
}
static void ifnet_stat_increment_in(ifnet_t i, unsigned sent, size_t bytes, unsigned errors) {
    assert(i->attached && !sent && !bytes);
    kpi.rx_errors += errors;
}
static int ifnet_input(ifnet_t i, mbuf_t m, struct ifnet_stat_increment_param *stats) {
    assert(i->attached && m->received_on == i && m->header == m->bytes);
    assert(m->skip == 14 && stats->bytes_in == m->size && stats->packets_in == 1);
    kpi.inputs++;
    mbuf_freem(m); /* consumes the chain even on an error */
    return kpi.input_error;
}
#endif
