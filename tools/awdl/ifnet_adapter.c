/* BSD endpoint of the real AWDL provider. An unbound module publishes
 * nothing. DLIL callbacks and radio callbacks never own an unpinned lease. */
#ifdef HK_AWDL_HOST_TEST
#include "tests/ifnet_shim.h"
#else
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <sys/hk_awdl_transport.h>
#include <sys/hk_net.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#endif

extern void hk_awdl_notify_link(bool up);
extern int hk_awdl_registry_publish(uint64_t, const char *, const uint8_t *, uint32_t);
extern int hk_awdl_registry_remove(uint64_t);
extern errno_t hk_apple80211_ioctl(u_long, void *);

static LCK_GRP_DECLARE(awif_group, "hk-awdl-ifnet");
static LCK_MTX_DECLARE(awif_lock, &awif_group);
static LCK_MTX_DECLARE(awif_peer_lock, &awif_group);
static struct {
    struct hk_awdl_transport_ops ops;
    void *context;
    uint64_t generation, lease;
    struct hk_awdl_link_config config;
    ifnet_t ifp;
    unsigned calls;
    bool ready, live, withdrawing;
    /* Lifecycle fields have one controller owner. The last detach
     * notification and packet leases additionally use awif_lock. */
    bool opened, quiesced, attached, published, announced, detaching, detached;
} awif;

#ifndef HK_AWDL_HOST_TEST
static int
awif_peer_notice(ifnet_t ifp, bool present, const uint8_t mac[6], const uint8_t ipv6[16])
{
    struct sockaddr_in6 ip = {.sin6_len=sizeof(ip), .sin6_family=AF_INET6, .sin6_scope_id=ifnet_index(ifp)};
    if (ipv6) memcpy(&ip.sin6_addr,ipv6,16);
    if (!present)
        return ifnet_notice_node_absence(ifp,(struct sockaddr *)&ip);
    struct sockaddr_dl link = {.sdl_len=sizeof(link), .sdl_family=AF_LINK,
                              .sdl_type=IFT_ETHER, .sdl_alen=ETHER_ADDR_LEN};
    memcpy(LLADDR(&link),mac,ETHER_ADDR_LEN);
    uint8_t no_service_metadata[48] = {0};
    if (!ipv6)
        return ifnet_notice_node_presence(ifp,(struct sockaddr *)&link,
            IFNET_RSSI_UNKNOWN,IFNET_LQM_THRESH_UNKNOWN,IFNET_NPM_THRESH_UNKNOWN,no_service_metadata);
    return ifnet_notice_node_presence_v2(ifp,(struct sockaddr *)&ip,&link,
        IFNET_RSSI_UNKNOWN,IFNET_LQM_THRESH_UNKNOWN,IFNET_NPM_THRESH_UNKNOWN,no_service_metadata);
}

/* The same BSD link event contract used by the station in hk_net.c. */
static void
awif_link_event(ifnet_t ifp, bool up)
{
    _Alignas(struct kern_event_msg) char bytes[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = {0};
    struct kern_event_msg *event = (struct kern_event_msg *)bytes;
    struct net_event_data *about = (struct net_event_data *)(event+1);
    event->total_size = sizeof(bytes);
    event->vendor_code = KEV_VENDOR_APPLE;
    event->kev_class = KEV_NETWORK_CLASS;
    event->kev_subclass = KEV_DL_SUBCLASS;
    event->event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
    about->if_family = ifnet_family(ifp);
    about->if_unit = ifnet_unit(ifp);
    strlcpy(about->if_name,ifnet_name(ifp),IFNAMSIZ);
    ifnet_event(ifp,event);
    hk_awdl_notify_link(up);
}
#endif

static bool
awif_unicast(const uint8_t address[6])
{
    static const uint8_t zero[6];
    return !(address[0] & 1) && memcmp(address, zero, sizeof(zero));
}

static bool
awif_ipv6(const uint8_t *frame, size_t bytes, bool incoming)
{
    if (!frame || bytes < ETHER_HDR_LEN + 40 || bytes > awif.config.mtu + ETHER_HDR_LEN)
        return false;
    const uint8_t *ip = frame + ETHER_HDR_LEN;
    if (frame[12] != 0x86 || frame[13] != 0xdd || (ip[0] >> 4) != 6 ||
        !awif_unicast(frame + 6))
        return false;
    if (40u + ((unsigned)ip[4] << 8) + ip[5] != bytes - ETHER_HDR_LEN)
        return false;
    if (!incoming && memcmp(frame + 6, awif.config.address, 6))
        return false;
    /* Only the standard IPv6 multicast Ethernet mapping is accepted. */
    if (ip[24] == 0xff)
        return frame[0] == 0x33 && frame[1] == 0x33 && !memcmp(frame+2,ip+36,4);
    if (!awif_unicast(frame))
        return false;
    return !incoming || !memcmp(frame, awif.config.address, 6);
}

static int
awif_pin(ifnet_t output, uint64_t gen, uint64_t lease)
{
    int error = 0;
    lck_mtx_lock(&awif_lock);
    if (!awif.ready || !awif.live || !awif.ifp ||
        (output && awif.ifp != output) ||
        (!output && (gen != awif.generation || lease != awif.lease))) {
        error = ENETDOWN;
    } else if (awif.calls == UINT_MAX) {
        error = EBUSY;
    } else {
        ++awif.calls;
    }
    lck_mtx_unlock(&awif_lock);
    return error;
}

static void
awif_unpin(void)
{
    lck_mtx_lock(&awif_lock);
    --awif.calls;
    lck_mtx_unlock(&awif_lock);
}

#include "ifnet_peer.inc"

static errno_t
awif_output(ifnet_t ifp, mbuf_t packet)
{
    static unsigned reports;
    int error = awif_pin(ifp,0,0);
    if (error) {
        printf("awdl-ifnet: output pin_errno=%d\n",error);
        mbuf_freem(packet);
        return error;
    }
    if (!(ifnet_flags(ifp) & IFF_UP)) {
        mbuf_freem(packet);
        awif_unpin();
        return ENETDOWN;
    }
    size_t size = mbuf_pkthdr_len(packet);
    uint8_t *frame = NULL;
    if (size < ETHER_HDR_LEN + 40 || size > ETHER_HDR_LEN + awif.config.mtu) {
        error = EMSGSIZE;
    } else {
        frame = kalloc_data(size, Z_NOWAIT);
        if (!frame) error = ENOBUFS;
    }
    if (!error) error = mbuf_copydata(packet,0,size,frame);
    if (!error && !awif_ipv6(frame,size,false)) error = EAFNOSUPPORT;
    if (!error) error = awif.ops.transmit(awif.context,awif.generation,awif.lease,frame,size);
    if (reports<12 || error) {
        reports++;
        printf("awdl-ifnet: output bytes=%lu ether_type=%u errno=%d\n",(unsigned long)size,
            frame && size>=14?((unsigned)frame[12]<<8|frame[13]):0,error);
    }
    if (!error) {
        bpf_tap_out(ifp,DLT_EN10MB,packet,NULL,0);
        ifnet_stat_increment_out(ifp,1,size,0);
    } else {
        ifnet_stat_increment_out(ifp,0,0,1);
    }
    if (frame) kfree_data(frame,size);
    mbuf_freem(packet);
    awif_unpin();
    return error;
}

int
hk_awdl_transport_input(uint64_t gen, uint64_t lease, const uint8_t *frame, size_t size)
{
    int error = awif_pin(NULL,gen,lease);
    if (error) return error;
    ifnet_t ifp = awif.ifp;
    mbuf_t packet = NULL;
    if (!(ifnet_flags(ifp) & IFF_UP)) {
        error = ENETDOWN;
    } else if (!awif_ipv6(frame,size,true)) {
        error = EAFNOSUPPORT;
    } else {
        unsigned chunks = 1;
        error = mbuf_allocpacket(MBUF_DONTWAIT,size,&chunks,&packet);
        if (!error) error = mbuf_copyback(packet,0,size,frame,MBUF_DONTWAIT);
        if (!error && mbuf_len(packet) < ETHER_HDR_LEN) error = ENOBUFS;
    }
    if (!error) {
        void *header = mbuf_data(packet);
        mbuf_pkthdr_setrcvif(packet,ifp);
        mbuf_pkthdr_setheader(packet,header);
        mbuf_adj(packet,ETHER_HDR_LEN);
        bpf_tap_in(ifp,DLT_EN10MB,packet,header,ETHER_HDR_LEN);
        struct ifnet_stat_increment_param stats = {.packets_in=1,.bytes_in=(uint32_t)size};
        /* ifnet_input consumes the chain on success and on failure. */
        error = ifnet_input(ifp,packet,&stats);
    } else {
        if (packet) mbuf_freem(packet);
        ifnet_stat_increment_in(ifp,0,0,1);
    }
    awif_unpin();
    return error;
}

static bool
awif_link_up(ifnet_t ifp)
{
    lck_mtx_lock(&awif_lock);
    bool up = awif.ready && awif.live && awif.ifp == ifp;
    lck_mtx_unlock(&awif_lock);
    return up;
}

#include "bonjour_store.inc"

static errno_t
awif_ioctl(ifnet_t ifp, u_long command, void *data)
{
    struct ifreq *r = data;
    switch (command) {
    case SIOCSIFADDR:
        return ifnet_set_flags(ifp,IFF_UP,IFF_UP);
    case SIOCSIFFLAGS:
        return ifnet_set_flags(ifp,((ifnet_flags(ifp)&IFF_UP) && awif_link_up(ifp))?IFF_RUNNING:0,IFF_RUNNING);
    case SIOCGIFDEVMTU:
        r->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
        r->ifr_devmtu.ifdm_max = ifnet_mtu(ifp);
        r->ifr_devmtu.ifdm_min = 1280;
        return 0;
    case SIOCSIFMTU:
        return (uint32_t)r->ifr_mtu == ifnet_mtu(ifp) ? 0 : EINVAL;
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        return 0; /* transport accepts IPv6 multicast on the real AW */
    case SIOCGIFMEDIA32:
    case SIOCGIFMEDIA64: {
        struct ifmediareq *m = data;
        int media = IFM_IEEE80211|IFM_AUTO;
        user_addr_t list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)data)->ifmu_ulist :
                                                     ((struct ifmediareq32 *)data)->ifmu_ulist;
        int capacity = m->ifm_count;
        m->ifm_current = media;
        m->ifm_active = media;
        m->ifm_mask = 0;
        m->ifm_status = IFM_AVALID | (awif_link_up(ifp)?IFM_ACTIVE:0);
        m->ifm_count = 1;
        return list && capacity>0 ? copyout(&media,list,sizeof(media)) : 0;
    }
    case 0x802869c8u: {
        struct {char name[16]; int32_t type,value; uint32_t length,pad; user_addr_t data;} wire;
        memcpy(&wire,data,sizeof(wire));
        if(wire.type==120){
            uint8_t packet[512];
            if(wire.length<20 || wire.length>sizeof(packet))return EMSGSIZE;
            int err=awif_pin(ifp,0,0);
            if(err)return err;
            err=copyin(wire.data,packet,wire.length);
            if(!err)err=awif_service_request(packet,wire.length,awif.lease);
            awif_unpin();
            return err;
        }
        printf("awdl-ifnet: SET type=%d len=%u value=%d\n",wire.type,wire.length,wire.value);
        break;
    }
    case 0xc02869c9u: {
        /* IO80211 20F66 binds each interface by reading its radio capabilities. */
        int32_t type;
        memcpy(&type,(const char *)data+IFNAMSIZ,sizeof(type));
        if (type==12 || type==43 || type==44 || type==244)
            return hk_apple80211_ioctl(command,data);
        printf("awdl-ifnet: unsupported GET type=%d\n",type);
        break;
    }
    }
    return EOPNOTSUPP;
}

static void
awif_detached(ifnet_t ifp)
{
    lck_mtx_lock(&awif_lock);
    if (awif.ifp == ifp) {
        awif.detached = true;
        awif.live = false;
        if (!awif.detaching) {
            awif.ready = false;
            (void)hk_awdl_provider_set_ready(awif.generation,false);
        }
        wakeup(&awif.detached);
    }
    lck_mtx_unlock(&awif_lock);
}

static int
awif_attach(uint64_t lease, char name[HK_AWDL_NAME_SIZE])
{
    const struct ifnet_init_eparams init = {
        .ver=IFNET_INIT_CURRENT_VERSION,.len=sizeof(init),
        .flags=IFNET_INIT_LEGACY|IFNET_INIT_NX_NOAUTO,
        .name="awdl",.unit=0,.family=IFNET_FAMILY_ETHERNET,.subfamily=IFNET_SUBFAMILY_WIFI,
        .uniqueid="hk-awdl",.uniqueid_len=sizeof("hk-awdl")-1,.type=IFT_ETHER,
        .output=awif_output,.demux=ether_demux,.add_proto=ether_add_proto,
        .del_proto=ether_del_proto,.check_multi=ether_check_multi,.framer_extended=ether_frameout_extended,
        .ioctl=awif_ioctl,.detach=awif_detached,.broadcast_addr=etherbroadcastaddr,.broadcast_len=ETHER_ADDR_LEN
    };
    ifnet_t ifp = NULL;
    ifnet_t existing = NULL;
    int error = ifnet_find_by_name("awdl0",&existing);
    if (!error && existing) {
        ifnet_release(existing);
        return EEXIST;
    }
    if (error != ENXIO) return error ? error : EIO;
    error = ifnet_allocate_extended(&init,&ifp);
    if (error) return error;
    lck_mtx_lock(&awif_lock);
    awif.ifp = ifp;
    lck_mtx_unlock(&awif_lock);
    if ((error=ifnet_set_addrlen(ifp,ETHER_ADDR_LEN)) ||
        (error=ifnet_set_hdrlen(ifp,ETHER_HDR_LEN)) ||
        (error=ifnet_set_mtu(ifp,awif.config.mtu)) ||
        (error=ifnet_set_flags(ifp,IFF_SIMPLEX|IFF_MULTICAST|IFF_BROADCAST|IFF_NOARP,0xffff)) ||
        (error=ifnet_set_eflags(ifp,IFEF_AWDL,IFEF_AWDL)))
        return error;
    struct sockaddr_dl addr = {.sdl_len=sizeof(addr),.sdl_family=AF_LINK,.sdl_type=IFT_ETHER,.sdl_alen=ETHER_ADDR_LEN};
    memcpy(LLADDR(&addr),awif.config.address,ETHER_ADDR_LEN);
    error = ifnet_attach(ifp,&addr);
    if (error) return error;
    awif.attached = true;
    error = proto_plumb(PF_INET6,ifp);
    if (error && error!=EEXIST) return error;
    int n = snprintf(name,HK_AWDL_NAME_SIZE,"%s%u",ifnet_name(ifp),ifnet_unit(ifp));
    if (n<1 || n>=HK_AWDL_NAME_SIZE) return ENAMETOOLONG;
    error = ifnet_find_by_name(name,&existing);
    if (error) return error;
    if (!existing) return ENXIO;
    bool matches = existing == ifp;
    ifnet_release(existing);
    if (!matches) return EEXIST;
    bpfattach(ifp,DLT_EN10MB,ETHER_HDR_LEN);
    error = hk_awdl_registry_publish(lease,name,awif.config.address,awif.config.mtu);
    if (error) return error;
    awif.published = true;
    lck_mtx_lock(&awif_lock);
    bool admitted = awif.ready && !awif.withdrawing && !awif.detached;
    if (admitted) awif.live = true;
    lck_mtx_unlock(&awif_lock);
    if (!admitted) return ENETDOWN;
    error = ifnet_set_flags(ifp,IFF_UP|IFF_RUNNING,IFF_UP|IFF_RUNNING);
    if (!error) {
        awif.announced = true;
        awif_link_event(ifp,true);
    }
    return error;
}

static int
awif_create(void *ctx, uint64_t lease, char name[HK_AWDL_NAME_SIZE])
{
    (void)ctx;
    lck_mtx_lock(&awif_lock);
    if (!awif.ready || awif.withdrawing || awif.lease) {
        lck_mtx_unlock(&awif_lock);
        return ENETDOWN;
    }
    awif.lease = lease;
    awif.opened = true; /* includes partially opened transport leases */
    lck_mtx_unlock(&awif_lock);
    int error = awif.ops.open(awif.context,awif.generation,lease,&awif.config);
    if (error) return error;
    if (!awif_unicast(awif.config.address)) return EINVAL;
    if (awif.config.mtu<1280 || awif.config.mtu>ETHERMTU) return EMSGSIZE;
    return awif_attach(lease,name);
}

static int
awif_remove(void *ctx, uint64_t lease)
{
    (void)ctx;
    lck_mtx_lock(&awif_lock);
    if (!awif.lease) {
        lck_mtx_unlock(&awif_lock);
        return 0;
    }
    if (awif.lease!=lease) {
        lck_mtx_unlock(&awif_lock);
        return ENOENT;
    }
    awif.live = false;
    bool busy = awif.calls != 0;
    lck_mtx_unlock(&awif_lock);
    if (busy) return EBUSY;
    int error;
    if (awif.announced) {
        (void)ifnet_set_flags(awif.ifp,0,IFF_RUNNING);
        awif_link_event(awif.ifp,false);
        awif.announced = false;
    }
    if (awif.opened && !awif.quiesced) {
        error = awif.ops.quiesce(awif.context,awif.generation,lease);
        if (error) return error;
        awif.quiesced = true;
    }
    /* All packet/event references are closed and the transport is drained. */
    lck_mtx_lock(&awif_lock);
    bool detached = awif.detached;
    lck_mtx_unlock(&awif_lock);
    error=awif_remove_all_peers(detached);
    if (error) return error;
    if (awif.published) {
        error=hk_awdl_registry_remove(lease);
        if (error) return error;
        awif.published = false;
    }
    if (awif.attached) {
        lck_mtx_lock(&awif_lock);
        bool initiate = !awif.detached && !awif.detaching;
        if (initiate) awif.detaching = true;
        lck_mtx_unlock(&awif_lock);
        if (initiate) {
            error = ifnet_detach(awif.ifp);
            if (error) {
                lck_mtx_lock(&awif_lock);
                bool done = awif.detached;
                awif.detaching = false;
                lck_mtx_unlock(&awif_lock);
                if (!done) return error;
            }
        }
        lck_mtx_lock(&awif_lock);
        if (!awif.detached) {
            struct timespec bound={.tv_sec=1};
            (void)msleep(&awif.detached,&awif_lock,0,"awdl_detach",&bound);
        }
        bool done=awif.detached;
        lck_mtx_unlock(&awif_lock);
        if (!done) return EINPROGRESS;
        awif.attached = false;
    }
    if (awif.ifp) {
        lck_mtx_lock(&awif_lock);
        ifnet_t retired=awif.ifp;
        awif.ifp=NULL;
        lck_mtx_unlock(&awif_lock);
        ifnet_release(retired);
    }
    if (awif.opened) {
        error=awif.ops.close(awif.context,awif.generation,lease);
        if (error) return error;
    }
    lck_mtx_lock(&awif_lock);
    awif.lease=0;
    awif.opened=false;
    awif.quiesced=false;
    awif.detaching=false;
    awif.detached=false;
    memset(&awif.config,0,sizeof(awif.config));
    lck_mtx_unlock(&awif_lock);
    return 0;
}

static int
awif_channels(void *ctx,uint64_t lease,struct hk_awdl_channels *status)
{
    (void)ctx;
    int error=awif_pin(NULL,awif.generation,lease);
    if (error) return error;
    error=awif.ops.channels(awif.context,awif.generation,lease,status);
    awif_unpin();
    return error;
}

int
hk_awdl_transport_register(const struct hk_awdl_transport_ops *ops, void *ctx,uint64_t *generation)
{
    if (!ops || !generation || !ops->open || !ops->transmit || !ops->channels || !ops->quiesce || !ops->close) return EINVAL;
    static const struct hk_awdl_ops control={.create=awif_create,.remove=awif_remove,.channels=awif_channels};
    lck_mtx_lock(&awif_lock);
    if (awif.generation || awif.withdrawing) {
        lck_mtx_unlock(&awif_lock);
        return EBUSY;
    }
    int error=hk_awdl_provider_register(&control,NULL,&awif.generation);
    if (!error) {
        awif.ops=*ops;
        awif.context=ctx;
        *generation=awif.generation;
    }
    lck_mtx_unlock(&awif_lock);
    return error;
}

int
hk_awdl_transport_set_ready(uint64_t generation,bool ready)
{
    lck_mtx_lock(&awif_lock);
    if (!generation || awif.generation!=generation || awif.withdrawing) {
        lck_mtx_unlock(&awif_lock);
        return ENOENT;
    }
    int error=hk_awdl_provider_set_ready(generation,ready);
    if (!ready || !error) awif.ready=ready;
    if (!ready) awif.live=false;
    lck_mtx_unlock(&awif_lock);
    return error;
}

int
hk_awdl_transport_unregister(uint64_t generation)
{
    lck_mtx_lock(&awif_lock);
    if (!generation || awif.generation!=generation || awif.withdrawing) {
        int error = awif.withdrawing ? EBUSY : ENOENT;
        lck_mtx_unlock(&awif_lock);
        return error;
    }
    awif.ready=false;
    awif.live=false;
    awif.withdrawing=true;
    lck_mtx_unlock(&awif_lock);

    int error=hk_awdl_provider_unregister(generation);
    lck_mtx_lock(&awif_lock);
    if (!error) {
        memset(&awif,0,sizeof(awif));
    } else {
        awif.withdrawing=false;
    }
    lck_mtx_unlock(&awif_lock);
    return error;
}


/* Export the same owner and lease contract across the LinuxKPI boundary.
 * No second registry, readiness state or deferred packet ownership. */
int
lx_os_awdl_register(const struct hk_awdl_transport_ops *ops, void *ctx, uint64_t *generation)
{
    return hk_awdl_transport_register(ops, ctx, generation);
}

int
lx_os_awdl_set_ready(uint64_t generation, bool ready)
{
    return hk_awdl_transport_set_ready(generation, ready);
}

int
lx_os_awdl_unregister(uint64_t generation)
{
    return hk_awdl_transport_unregister(generation);
}

int
lx_os_awdl_input(uint64_t generation, uint64_t lease, const uint8_t *frame, size_t bytes)
{
    return hk_awdl_transport_input(generation, lease, frame, bytes);
}
