/* Host exercise of the production adapter, control admission and packet
 * checks. Hardware callbacks own only synthetic addresses and test frames. */
#define HK_AWDL_HOST_TEST 1
#include <stddef.h>
#include "ifnet_adapter.c"

struct apple80211req {
    char req_if_name[16];
    int32_t req_type, req_val;
    uint32_t req_len;
    uintptr_t req_data __attribute__((aligned(8)));
};
static int copyin(uintptr_t from, void *to, size_t n) {
    if (!from) return EFAULT;
    memcpy(to, (const void *)from, n);
    return 0;
}
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
#define HK_AWDL_LOCK() lck_mtx_lock(&control)
#define HK_AWDL_UNLOCK() lck_mtx_unlock(&control)
static uint64_t test_client_id = 1;
static uint64_t hk_awdl_client_id(void) { return test_client_id; }
static bool hk_awdl_name_attached(const char *name) {
    return !strcmp(name, "awdl0") && kpi.published != 0;
}
#include "apple80211_control.inc"

static struct {
    uint64_t generation, lease;
    unsigned opens, tx, quiesce, closes, open_error, quiesce_error, close_error;
    bool block, entered, resume;
} radio;
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tx_cond = PTHREAD_COND_INITIALIZER;
static const uint8_t local[6] = {2,0,0,0,0,1}, remote[6] = {2,0,0,0,0,2};

int hk_awdl_registry_publish(uint64_t lease, const char *name, const uint8_t *mac, uint32_t mtu) {
    assert(lease == radio.lease && !kpi.published && !strcmp(name, "awdl0"));
    assert(!memcmp(mac, local, sizeof(local)) && mtu == 1280);
    if (kpi.registry_error) return kpi.registry_error;
    kpi.published = lease;
    return 0;
}
int hk_awdl_registry_remove(uint64_t lease) {
    assert(lease == kpi.published);
    kpi.published = 0;
    return 0;
}
errno_t hk_apple80211_ioctl(u_long cmd, void *wire) {
    assert(cmd == 0xc02869c9u);
    return awdl_channels(wire);
}
static int open_test(void *ctx, uint64_t g, uint64_t lease, struct hk_awdl_link_config *config) {
    assert(ctx == &radio && g == radio.generation && lease && !radio.lease);
    radio.opens++;
    radio.lease = lease;
    memcpy(config->address, local, sizeof(local));
    config->mtu = 1280;
    return radio.open_error;
}
static int transmit_test(void *ctx, uint64_t g, uint64_t lease, const uint8_t *f, size_t n) {
    assert(ctx == &radio && g == radio.generation && radio.lease == lease);
    assert(n == 54 && f[14] == 0x60);
    lck_mtx_lock(&fixture_lock);
    radio.tx++;
    if (radio.block) {
        radio.entered = true;
        assert(pthread_cond_broadcast(&tx_cond) == 0);
        while (!radio.resume)
            assert(pthread_cond_wait(&tx_cond, &fixture_lock) == 0);
    }
    lck_mtx_unlock(&fixture_lock);
    return 0;
}
static int channels_test(void *ctx, uint64_t g, uint64_t lease, struct hk_awdl_channels *channels) {
    assert(ctx == &radio && g == radio.generation && radio.lease == lease);
    *channels = (struct hk_awdl_channels){.preferred=6,.peer_master=44};
    return 0;
}
static int quiesce_test(void *ctx, uint64_t g, uint64_t lease) {
    assert(ctx == &radio && g == radio.generation && radio.lease == lease);
    radio.quiesce++;
    return radio.quiesce_error;
}
static int close_test(void *ctx, uint64_t g, uint64_t lease) {
    assert(ctx == &radio && g == radio.generation && radio.lease == lease && !kpi.published);
    radio.closes++;
    if (radio.close_error) return radio.close_error;
    radio.lease = 0;
    return 0;
}
static const struct hk_awdl_transport_ops backend = {open_test, transmit_test, channels_test, quiesce_test, close_test};
static uint64_t start(void) {
    assert(!awif.generation && !awdl.generation);
    memset(&kpi, 0, sizeof(kpi));
    memset(&radio, 0, sizeof(radio));
    assert(hk_awdl_transport_register(&backend, &radio, &radio.generation) == 0 && radio.generation);
    return radio.generation;
}
static int create(void) {
    struct hk_awdl_create_request wire = {.version=1,.role=6};
    struct apple80211req r = {.req_if_name="en0",.req_len=sizeof(wire),.req_data=(uintptr_t)&wire};
    int error = awdl_create(&r);
    if (!error) assert(!strcmp(wire.name,"awdl0"));
    return error;
}
static void frame(uint8_t dst[54], bool incoming) {
    memset(dst, 0, 54);
    memcpy(dst, incoming ? local : remote, 6);
    memcpy(dst + 6, incoming ? remote : local, 6);
    dst[12]=0x86;
    dst[13]=0xdd;
    dst[14]=0x60;
    dst[20]=59;
    dst[21]=64;
    dst[22]=0xfe;
    dst[23]=0x80;
    dst[38]=0xfe;
    dst[39]=0x80;
}
static int send_frame(ifnet_t i, const uint8_t *f, size_t n) {
    mbuf_t m = NULL;
    unsigned chunks=1;
    assert(mbuf_allocpacket(MBUF_DONTWAIT,n,&chunks,&m) == 0);
    assert(mbuf_copyback(m,0,n,f,MBUF_DONTWAIT) == 0);
    return i->params.output(i,m);
}
static void test_partial(void) {
    for (unsigned fault=0; fault<5; fault++) {
        uint64_t g=start();
        assert(create() == EOPNOTSUPP && !radio.opens && !kpi.allocations);
        assert(hk_awdl_transport_set_ready(g,true) == 0);
        int expected=0;
        switch (fault) {
        case 0: radio.open_error=expected=EIO; break;
        case 1: kpi.allocate_error=expected=ENOBUFS; break;
        case 2: kpi.attach_error=expected=EIO; break;
        case 3: kpi.plumb_error=expected=ENXIO; break;
        case 4: kpi.registry_error=expected=EBUSY; break;
        }
        assert(create() == expected);
        assert(!awif.live && !radio.lease && !kpi.published);
        assert(radio.opens == 1 && radio.quiesce == 1 && radio.closes == 1);
        assert(kpi.allocations == kpi.releases);
        assert(kpi.detaches == (fault>=3));
        assert(hk_awdl_transport_unregister(g) == 0);
    }
}
static void test_existing_name(unsigned takeover_lookup) {
    uint64_t generation = start();
    kpi.foreign_on_search = takeover_lookup;
    kpi.foreign.params.name = "awdl";
    kpi.foreign.params.unit = 0;
    kpi.foreign.references = 1;
    kpi.foreign.attached = true;
    assert(hk_awdl_transport_set_ready(generation, true) == 0);
    assert(create() == EEXIST);
    assert(!kpi.published && !radio.lease);
    assert(kpi.searches == takeover_lookup);
    assert(kpi.borrow == 1 && kpi.give_back == 1);
    assert(kpi.foreign.references == 1 && kpi.foreign.attached && !kpi.foreign.released);
    /* When the second check finds a different instance, only our own
     * partial attach is retired. The pre-attach collision allocates none. */
    assert(kpi.allocations == takeover_lookup - 1 && kpi.releases == kpi.allocations);
    assert(kpi.detaches == kpi.allocations && radio.quiesce == 1 && radio.closes == 1);
    assert(hk_awdl_transport_unregister(generation) == 0);
    assert(kpi.foreign.attached && !kpi.foreign.detached && kpi.foreign.references == 1);
}

static void test_data(void) {
    uint64_t g=start();
    assert(hk_awdl_transport_set_ready(g,true) == 0 && create() == 0);
    uint64_t lease=radio.lease;
    ifnet_t i=awif.ifp;
    uint8_t f[54];
    assert(kpi.link_up == 1 && !kpi.link_down);
    assert(kpi.searches == 2 && kpi.borrow == 1 && kpi.give_back == 1 && i->references == 1);
    int listed = -1;
    struct ifmediareq64 media = {.common = {.ifm_mask = -1, .ifm_count = 1},
                                .ifmu_ulist = (uintptr_t)&listed};
    assert(i->params.ioctl(i, SIOCGIFMEDIA64, &media) == 0);
    assert(media.common.ifm_mask == 0 && media.common.ifm_count == 1);
    assert(listed == media.common.ifm_current && media.common.ifm_active == listed);
    struct ifmediareq32 sizing = {.common = {.ifm_mask = -1}};
    assert(i->params.ioctl(i, SIOCGIFMEDIA32, &sizing) == 0);
    assert(sizing.common.ifm_mask == 0 && sizing.common.ifm_count == 1);
    struct ifreq flags_request = {0};
    assert(ifnet_set_flags(i,0,IFF_UP) == 0);
    assert(i->params.ioctl(i,SIOCSIFFLAGS,&flags_request) == 0 && !(i->flags & IFF_RUNNING));
    frame(f,true);
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == ENETDOWN && !kpi.inputs);
    frame(f,false);
    assert(send_frame(i,f,sizeof(f)) == ENETDOWN && !radio.tx);
    assert(ifnet_set_flags(i,IFF_UP,IFF_UP) == 0);
    assert(i->params.ioctl(i,SIOCSIFFLAGS,&flags_request) == 0 && (i->flags & IFF_RUNNING));
    assert(send_frame(i,f,sizeof(f)) == 0 && radio.tx==1 && kpi.outputs==1);
    f[6]^=4;
    assert(send_frame(i,f,sizeof(f)) == EAFNOSUPPORT && radio.tx==1);
    frame(f,false);
    f[13]=0;
    assert(send_frame(i,f,sizeof(f)) == EAFNOSUPPORT);
    frame(f,false);
    f[19]=1;
    assert(send_frame(i,f,sizeof(f)) == EAFNOSUPPORT);
    assert(send_frame(i,f,50) == EMSGSIZE);
    frame(f,true);
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == 0 && kpi.inputs==1);
    kpi.input_error=ENOBUFS;
    unsigned freed=kpi.freed;
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == ENOBUFS && kpi.freed==freed+1);
    kpi.input_error=0;
    assert(hk_awdl_transport_input(g+1,lease,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_input(g,lease+1,f,sizeof(f)) == ENETDOWN);
    f[5]++;
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == EAFNOSUPPORT);
    frame(f,true);
    f[6]|=1;
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == EAFNOSUPPORT);
    frame(f,true);
    f[38]=0xff;
    f[53]=0x42;
    f[0]=f[1]=0x33;
    memset(f+2,0,4);
    f[5]=0x42;
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == 0);
    f[5]=1;
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == EAFNOSUPPORT);
    /* This seam checks frame shape and direction. The producer is responsible
     * for its authenticated peer and synchronization window. */
    assert(hk_awdl_transport_set_ready(g,false) == 0);
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == ENETDOWN);
    assert(send_frame(i,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_unregister(g) == 0 && kpi.releases == 1);
    assert(kpi.link_up == 1 && kpi.link_down == 1);
    uint64_t next=start();
    assert(next != g && !awdl_capable("en0"));
    assert(hk_awdl_transport_set_ready(next,true) == 0 && awdl_capable("en0") && create() == 0);
    frame(f,true);
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_input(next,lease,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_input(next,radio.lease,f,sizeof(f)) == 0);
    struct hk_awdl_delete_request removal = {.version=1,.name="awdl0"};
    struct apple80211req d = {.req_if_name="en0",.req_len=sizeof(removal),.req_data=(uintptr_t)&removal};
    uint64_t retired = radio.lease;
    assert(awdl_delete(&d) == 0 && !kpi.published);
    assert(create() == 0 && radio.lease != retired);
    assert(hk_awdl_transport_input(next,retired,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_unregister(next) == 0);
}
static void test_external_detach(void) {
    uint64_t g = start();
    assert(hk_awdl_transport_set_ready(g,true) == 0 && create() == 0);
    ifnet_t i = awif.ifp;
    uint64_t lease = radio.lease;
    finish_detach(i);
    assert(!awif.ready && !awif.live && !awdl_capable("en0") && !kpi.releases);
    assert(kpi.published == lease);
    uint8_t f[54];
    frame(f,true);
    assert(hk_awdl_transport_input(g,lease,f,sizeof(f)) == ENETDOWN);
    assert(send_frame(i,f,sizeof(f)) == ENETDOWN);
    assert(hk_awdl_transport_unregister(g) == 0);
    assert(!kpi.detaches && kpi.releases == 1 && radio.closes == 1 && !kpi.published);
}

static void *blocked_tx(void *arg) {
    uint8_t f[54];
    frame(f,false);
    assert(send_frame(arg,f,sizeof(f)) == 0);
    return NULL;
}
static void test_drain(void) {
    uint64_t g=start();
    assert(hk_awdl_transport_set_ready(g,true)==0 && create()==0);
    uint64_t lease=radio.lease;
    radio.block=true;
    pthread_t thread;
    assert(pthread_create(&thread,NULL,blocked_tx,awif.ifp)==0);
    lck_mtx_lock(&fixture_lock);
    while (!radio.entered)
        assert(pthread_cond_wait(&tx_cond,&fixture_lock)==0);
    lck_mtx_unlock(&fixture_lock);
    assert(hk_awdl_transport_unregister(g)==EBUSY);
    assert(awif.generation==g && radio.lease==lease && kpi.published && !radio.quiesce);
    assert(awif.ifp && !awif.live);
    lck_mtx_lock(&fixture_lock);
    radio.resume=true;
    assert(pthread_cond_broadcast(&tx_cond)==0);
    lck_mtx_unlock(&fixture_lock);
    assert(pthread_join(thread,NULL)==0);
    radio.quiesce_error=ETIMEDOUT;
    assert(hk_awdl_transport_unregister(g)==ETIMEDOUT && !radio.closes && !kpi.detaches);
    assert(awif.generation==g && radio.lease==lease && kpi.published);
    radio.quiesce_error=0;
    kpi.hold_detach=true;
    assert(hk_awdl_transport_unregister(g)==EINPROGRESS);
    assert(kpi.detaches==1 && !kpi.releases && kpi.pending && !kpi.published);
    assert(hk_awdl_transport_unregister(g)==EINPROGRESS && kpi.detaches==1 && !radio.closes);
    finish_detach(kpi.pending);
    radio.close_error=EIO;
    assert(hk_awdl_transport_unregister(g)==EIO && kpi.releases==1 && radio.lease==lease);
    radio.close_error=0;
    assert(hk_awdl_transport_unregister(g)==0 && !radio.lease && !awif.generation);
    assert(kpi.releases==1 && radio.quiesce==2 && radio.closes==2);
}
static void test_peer_sources(void) {
    uint64_t g=start();
    assert(hk_awdl_transport_set_ready(g,true)==0 && create()==0);
    uint64_t l=radio.lease;
    uint8_t ip[16]={0xfe,0x80}; ip[11]=0xff; ip[12]=0xfe; ip[15]=2;
    uint8_t opaque[16]={0xfe,0x80}; opaque[15]=9;
    assert(lx_os_awdl_peer_present(g,l+1,1,remote,ip)==ENETDOWN);
    assert(lx_os_awdl_peer_present(g,l,1,remote,opaque)==EINVAL);
    assert(lx_os_awdl_peer_present(g,l,2,local,ip)==EINVAL);
    awif_notice_error=ENXIO;
    assert(lx_os_awdl_peer_present(g,l,1,remote,ip)==ENXIO && !awif_peer.sources);
    awif_notice_error=0;
    assert(!lx_os_awdl_peer_present(g,l,1,remote,ip));
    assert(!lx_os_awdl_peer_present(g,l,2,remote,ip));
    assert(awif_peer.sources==3 && lx_os_awdl_peer_present(g,l,2,remote,opaque)==EBUSY);
    assert(!lx_os_awdl_peer_absent(g,l,2,remote,ip) && awif_peer.sources==1 && !awif_notice_removes);
    assert(lx_os_awdl_peer_absent(g,l,2,remote,ip)==ENOENT);
    awif_notice_error=EIO;
    assert(lx_os_awdl_peer_absent(g,l,1,remote,ip)==EIO && awif_peer.sources==1);
    awif_notice_error=0;
    assert(!lx_os_awdl_peer_absent(g,l,1,remote,ip));
    assert(!lx_os_awdl_peer_present(g,l,2,remote,opaque));
    assert(!hk_awdl_transport_unregister(g));
    assert(!awif_peer.sources && awif_notice_removes==2 && awif_notice_adds==3);
}
int main(void) {
    test_existing_name(1);
    test_existing_name(2);
    test_peer_sources();
    test_partial();
    test_data();
    test_drain();
    test_external_detach();
    puts("passed: existing and raced foreign name, reference balance, media mask, idle, 5 partial create failures, IPv6 TX/RX admission and mbuf ownership, admin-down, external detach, link events, old leases, inflight drain, asynchronous detach, quiesce/close retry");
}
