#ifndef LX_AWDL_PEER_TABLE_H
#define LX_AWDL_PEER_TABLE_H

#include "peer_windows.h"

/* Serialized by the radio owner. Nothing here opens a peer, authorizes an
 * application or transmits. Publication and retirement are explicit caller
 * actions; an occupied slot is never silently recycled. */
#define LX_AWDL_PEER_SLOTS 8
/* Identity/neighbor lifetime covers firmware chanctx setup and MCC visits.
 * This is separate from the much shorter advertised clock's validity. */
#define LX_AWDL_PEER_TTL_US UINT64_C(10000000)
enum { LX_AWDL_PEER_OTHER=-1, LX_AWDL_PEER_INVALID=-2, LX_AWDL_PEER_FULL=-3, LX_AWDL_PEER_STALE=-4 };

struct lx_awdl_peer_metadata {
    uint8_t source[6], data_address[6];
    uint16_t path_flags, social_channels;
    bool has_path, has_data_address, has_social_channels, has_hostname;
    bool repeated_path, repeated_arpa;
    bool has_ht;
    uint16_t ht_capabilities;
    uint8_t ht_ampdu, ht_mcs0;
    char hostname[64]; /* transient single host label; never retained by the table */
};

struct lx_awdl_peer {
    uint8_t mac[6], data_mac[6], eui64[16], observed_ipv6[16], master[6], sync_master[6];
    uint8_t version, device_class, master_channel;
    bool has_ht;
    uint16_t ht_capabilities;
    uint8_t ht_ampdu, ht_mcs0;
    bool seen_mif, election_v2, has_observed_ipv6, data_address_present, data_address_matches;
    uint16_t ieee_sequence, social_channels;
    uint32_t master_counter, master_metric, distance, last_rx_mhz, duplicate_fields;
    uint64_t serial, last_rx_us, election_us, ipv6_rx_us;
    struct awdl_window_clock clock;
};
struct lx_awdl_peer_table {
    uint64_t next_serial;
    struct lx_awdl_peer peers[LX_AWDL_PEER_SLOTS];
};

static inline int lx_peer_cmp(const uint8_t *a,const uint8_t *b,size_t n)
{
    for (size_t i=0;i<n;i++)
        if (a[i]!=b[i]) return a[i]>b[i] ? 1 : -1;
    return 0;
}
static inline bool lx_peer_unicast(const uint8_t *a)
{
    return a && !(a[0]&1) && (a[0]|a[1]|a[2]|a[3]|a[4]|a[5]);
}

/* Recognize a single host label with the AWDL .local abbreviation or a
 * fully encoded .local suffix. Leave unfamiliar DNS names opaque. */
static inline bool lx_peer_hostname(const uint8_t *p,size_t n,char label[64])
{
    if (n<5 || p[0]!=3 || !p[1] || p[1]>63) return false;
    unsigned k=p[1];
    if (n<2u+k || p[2]=='-' || p[1+k]=='-') return false;
    for (unsigned i=0;i<k;i++) {
        uint8_t c=p[2+i];
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
            return false;
    }
    static const uint8_t local[]={5,'l','o','c','a','l',0};
    const uint8_t *suffix=p+2+k;
    size_t extra=n-2-k;
    if (!((extra==2 && suffix[0]==0xc0 && suffix[1]==0x0c) ||
          (extra==sizeof(local) && !lx_peer_cmp(suffix,local,sizeof(local))))) return false;
    for (unsigned i=0;i<k;i++) label[i]=(char)p[2+i];
    label[k]=0;
    return true;
}

/* Wireshark packet-awdl.c:999–1070. The documented Data Path State carries
 * a link address, not an IPv6 address. Unknown trailing extensions are opaque. */
static inline bool lx_peer_path(const uint8_t *p,size_t n,struct lx_awdl_peer_metadata *m)
{
    if (n<2) return false;
    uint16_t f=awdl_rx_le16(p);
    m->path_flags=f;
    size_t at=2;
    if (f&0x0100) at+=3;
    m->has_social_channels=(f&0x0200)!=0;
    m->social_channels=0;
    if (m->has_social_channels) {
        if (at+2>n) return false;
        m->social_channels=awdl_rx_le16(p+at);
        at+=2;
    }
    if (f&1) at+=8;
    if (f&2) at+=6;
    m->has_data_address=(f&4)!=0;
    for (unsigned i=0;i<6;i++) m->data_address[i]=0;
    if (m->has_data_address) {
        if (at+6>n) return false;
        awdl_rx_address(m->data_address,p+at);
        at+=6;
    }
    if (f&0x10) at+=2;
    if (at>n) return false;
    if (f&0x1000) {
        if (n-at<2) return false;
        size_t size=awdl_rx_le16(p+at);
        at+=2;
        if (size>n-at) return false;
        at+=size;
    }
    if (f&0x8000) {
        if (n-at<2) return false;
        unsigned ext=awdl_rx_le16(p+at);
        at+=2;
        if (ext&1) at+=2;
        if (ext&4) at+=4;
        if (ext&0x40) at+=12;
    }
    return at<=n;
}

static inline enum awdl_rx_result
lx_awdl_peer_decode(const uint8_t *frame,size_t len,struct awdl_rx_action *scratch,
                    struct lx_awdl_peer_metadata *metadata)
{
    if (!metadata) return AWDL_RX_INVALID;
    enum awdl_rx_result status=awdl_rx_decode_action(frame,len,scratch);
    if (status!=AWDL_RX_OK) return status;
    static const uint8_t bssid[]={0,0x25,0,0xff,0x94,0x73};
    if (lx_peer_cmp(frame+16,bssid,6) || !lx_peer_unicast(frame+10)) return AWDL_RX_OTHER;
    struct lx_awdl_peer_metadata m={0};
    awdl_rx_address(m.source,frame+10);
    bool arpa_seen=false;
    for (size_t at=(frame[1]&0x80)?44:40;at<len;) {
        unsigned tag=frame[at];
        size_t n=awdl_rx_le16(frame+at+1);
        const uint8_t *p=frame+at+3;
        if (tag==12) {
            if (!lx_peer_path(p,n,&m)) return AWDL_RX_INVALID;
            m.repeated_path=m.has_path;
            m.has_path=true;
        } else if (tag==16) {
            m.repeated_arpa=arpa_seen;
            arpa_seen=true;
            m.hostname[0]=0;
            m.has_hostname=lx_peer_hostname(p,n,m.hostname);
        } else if (tag==7) {
            if(n<8) return AWDL_RX_INVALID;
            m.has_ht=true;m.ht_capabilities=awdl_rx_le16(p+2);
            m.ht_ampdu=p[4];m.ht_mcs0=p[5];
        }
        at+=3+n; /* already bounded by awdl_rx_decode_action */
    }
    *metadata=m;
    return AWDL_RX_OK;
}

static inline void lx_peer_make_eui64(uint8_t out[16],const uint8_t mac[6])
{
    for (unsigned i=0;i<16;i++) out[i]=0;
    out[0]=0xfe; out[1]=0x80; out[8]=mac[0]^2; out[9]=mac[1]; out[10]=mac[2];
    out[11]=0xff; out[12]=0xfe; out[13]=mac[3]; out[14]=mac[4]; out[15]=mac[5];
}

static inline uint8_t lx_awdl_peer_expired(const struct lx_awdl_peer_table *t,uint64_t now_us)
{
    uint8_t mask=0;
    if (t) for (unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
        const struct lx_awdl_peer *p=t->peers+i;
        if (p->serial && now_us>=p->last_rx_us && now_us-p->last_rx_us>=LX_AWDL_PEER_TTL_US)
            mask|=(uint8_t)(1u<<i);
    }
    return mask;
}

static inline bool lx_awdl_peer_remove(struct lx_awdl_peer_table *t,unsigned slot,uint64_t serial)
{
    if (!t || slot>=LX_AWDL_PEER_SLOTS || !serial || t->peers[slot].serial!=serial) return false;
    t->peers[slot]=(struct lx_awdl_peer){0};
    return true;
}

/* Link multicast reaches every live data address, independently of which
 * node wins the master election. Repeated management identities that name
 * the same link recipient do not multiply a native packet. */
static inline unsigned lx_awdl_group_targets(const struct lx_awdl_peer_table *t,uint64_t now,
                                             uint8_t (*targets)[6],unsigned capacity)
{
    if(!t || !targets || !capacity) return 0;
    uint8_t expired=lx_awdl_peer_expired(t,now);
    unsigned count=0;
    for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
        const struct lx_awdl_peer *p=t->peers+i;
        if(!p->serial || !p->seen_mif || !p->version || !p->device_class ||
            !lx_peer_unicast(p->data_mac) || (expired&(1u<<i))) continue;
        bool duplicate=false;
        for(unsigned j=0;j<count;j++) if(!lx_peer_cmp(targets[j],p->data_mac,6))duplicate=true;
        if(!duplicate) {
            awdl_rx_address(targets[count++],p->data_mac);
            if(count==capacity) break;
        }
    }
    return count;
}

static inline bool lx_awdl_other_claim(const struct lx_awdl_peer_table *t,unsigned slot,
                                      const uint8_t mac[6],const uint8_t *ip,uint64_t now)
{
    uint8_t expired=lx_awdl_peer_expired(t,now);
    for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
        const struct lx_awdl_peer *p=t->peers+i;
        if(i!=slot && p->serial && !(expired&(1u<<i)) && !lx_peer_cmp(p->data_mac,mac,6) &&
            (!ip || (p->has_observed_ipv6 && !lx_peer_cmp(p->observed_ipv6,ip,16)))) return true;
    }
    return false;
}

static inline int
lx_awdl_peer_observe(struct lx_awdl_peer_table *t,const uint8_t *frame,size_t len,
                     const uint8_t self[6],uint32_t rx_mhz,uint64_t rx_us,
                     struct awdl_rx_action *scratch)
{
    if (!t || !lx_peer_unicast(self) || !rx_us || !rx_mhz) return LX_AWDL_PEER_INVALID;
    struct lx_awdl_peer_metadata m;
    enum awdl_rx_result status=lx_awdl_peer_decode(frame,len,scratch,&m);
    if (status==AWDL_RX_OTHER) return LX_AWDL_PEER_OTHER;
    if (status!=AWDL_RX_OK) return LX_AWDL_PEER_INVALID;
    if (m.has_data_address && !lx_peer_unicast(m.data_address)) return LX_AWDL_PEER_INVALID;
    if (!lx_peer_cmp(m.source,self,6)) return LX_AWDL_PEER_OTHER;
    struct awdl_window_clock clock;
    if (!awdl_clock_from_action(scratch,rx_us,&clock)) return LX_AWDL_PEER_INVALID;
    bool v2=(scratch->fields&AWDL_RX_HAS_ELECTION_V2)!=0;
    if (!v2 && !(scratch->fields&AWDL_RX_HAS_ELECTION)) return LX_AWDL_PEER_INVALID;
    const uint8_t *master=v2?scratch->election_v2.master:scratch->election.master;
    if (!lx_peer_unicast(master) || (v2 && !lx_peer_unicast(scratch->election_v2.sync_address)))
        return LX_AWDL_PEER_INVALID;
    int slot=-1;
    for (unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
        if (!t->peers[i].serial) {
            if (slot<0) slot=(int)i;
        } else if (!lx_peer_cmp(t->peers[i].mac,m.source,6)) {
            slot=(int)i;
            break;
        }
    }
    if (slot<0) return LX_AWDL_PEER_FULL;
    struct lx_awdl_peer *p=t->peers+slot;
    uint16_t sequence=awdl_rx_le16(frame+22)>>4;
    if (p->serial) {
        if (rx_us<=p->last_rx_us || rx_us-p->last_rx_us>=LX_AWDL_PEER_TTL_US ||
            ((frame[1]&8) && p->ieee_sequence==sequence)) return LX_AWDL_PEER_STALE;
    } else {
        if (t->next_serial==UINT64_MAX) return LX_AWDL_PEER_FULL;
        p->serial=++t->next_serial;
        awdl_rx_address(p->mac,m.source);
        awdl_rx_address(p->data_mac,m.source);
        lx_peer_make_eui64(p->eui64,m.source);
    }
    p->clock=clock;
    p->master_channel=scratch->sync.master_channel;
    p->last_rx_us=rx_us;
    p->last_rx_mhz=rx_mhz;
    p->ieee_sequence=sequence;
    p->duplicate_fields=scratch->duplicate_fields;
    p->seen_mif|=scratch->kind==LX_AWDL_MIF;
    if (scratch->fields&AWDL_RX_HAS_VERSION) {
        p->version=scratch->advertised_version;
        p->device_class=scratch->device_class;
    }
    if(m.has_ht) {
        p->has_ht=true;p->ht_capabilities=m.ht_capabilities;
        p->ht_ampdu=m.ht_ampdu;p->ht_mcs0=m.ht_mcs0;
    }
    if (m.has_path) {
        p->data_address_present=m.has_data_address;
        p->data_address_matches=m.has_data_address && !lx_peer_cmp(m.data_address,m.source,6);
        if (m.has_data_address && lx_peer_cmp(p->data_mac,m.data_address,6)) {
            awdl_rx_address(p->data_mac,m.data_address);
            lx_peer_make_eui64(p->eui64,m.data_address);
            p->has_observed_ipv6=false;
            p->ipv6_rx_us=0;
        }
        p->social_channels=m.social_channels;
    }
    if (v2 || !p->election_v2 || rx_us-p->election_us>=LX_AWDL_PEER_TTL_US) {
        p->election_v2=v2;
        p->election_us=rx_us;
        awdl_rx_address(p->master,master);
        awdl_rx_address(p->sync_master,v2?scratch->election_v2.sync_address:master);
        p->master_counter=v2?scratch->election_v2.master_counter:0;
        p->master_metric=v2?scratch->election_v2.master_metric:scratch->election.master_metric;
        p->distance=v2?scratch->election_v2.distance:scratch->election.distance;
    }
    return slot;
}

static inline bool
lx_awdl_peer_note_ipv6(struct lx_awdl_peer_table *t,unsigned slot,uint64_t serial,
                       const uint8_t actual_source[6],const uint8_t *ip,size_t len,uint64_t rx_us)
{
    if (!t || slot>=LX_AWDL_PEER_SLOTS || !serial || !actual_source || !ip || len<40 ||
        (ip[0]>>4)!=6 || len-40u!=((size_t)ip[4]<<8|ip[5]) ||
        ip[8]!=0xfe || (ip[9]&0xc0)!=0x80) return false;
    struct lx_awdl_peer *p=t->peers+slot;
    if (p->serial!=serial || lx_peer_cmp(actual_source,p->data_mac,6) || rx_us<p->last_rx_us ||
        rx_us<p->ipv6_rx_us || rx_us-p->last_rx_us>=LX_AWDL_PEER_TTL_US) return false;
    for (unsigned i=0;i<16;i++) p->observed_ipv6[i]=ip[8+i];
    p->has_observed_ipv6=true;
    p->ipv6_rx_us=rx_us;
    return true;
}

static inline int lx_awdl_peer_rank(const struct lx_awdl_peer *a,const struct lx_awdl_peer *b)
{
    if (a->master_counter!=b->master_counter) return a->master_counter>b->master_counter?1:-1;
    if (a->master_metric!=b->master_metric) return a->master_metric>b->master_metric?1:-1;
    if (a->distance!=b->distance) return a->distance<b->distance?1:-1;
    return lx_peer_cmp(a->mac,b->mac,6);
}

static inline bool lx_awdl_peer_should_follow(const struct lx_awdl_peer *p,const uint8_t self[6],
                                               uint32_t local_counter,uint32_t local_metric)
{
    if (!p || !p->serial || !self || !p->seen_mif || !p->version || !p->device_class ||
        p->distance>=10 || !lx_peer_cmp(p->sync_master,self,6) || !lx_peer_cmp(p->master,self,6)) return false;
    if (p->master_counter!=local_counter) return p->master_counter>local_counter;
    if (p->master_metric!=local_metric) return p->master_metric>local_metric;
    return !p->distance && lx_peer_cmp(p->mac,self,6)>0;
}

static inline int lx_awdl_peer_select(const struct lx_awdl_peer_table *t,const uint8_t self[6],
                                      uint64_t now_us)
{
    if (!t || !self) return -1;
    int best=-1;
    for (unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++) {
        const struct lx_awdl_peer *p=t->peers+i;
        if (!p->serial || !p->seen_mif || !p->version || !p->device_class || p->distance>=10 ||
            now_us<p->last_rx_us || now_us-p->last_rx_us>=LX_AWDL_PEER_TTL_US ||
            now_us>=p->clock.valid_until_us || !lx_peer_cmp(p->mac,self,6) ||
            !lx_peer_cmp(p->sync_master,self,6) || !lx_peer_cmp(p->master,self,6)) continue;
        if (best<0 || lx_awdl_peer_rank(p,t->peers+best)>0) best=(int)i;
    }
    return best;
}
#endif
