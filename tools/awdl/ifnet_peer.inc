/*
 * One source set per live MAC. A peer may have two simultaneous native
 * routes: an explicitly provisioned EUI-64 and an observed privacy LLA.
 * The third slot allows an observed address to rotate before the old
 * publication is withdrawn. Failed withdrawals remain tracked and retryable.
 * The radio worker serializes its observed/update/expiry events per lease.
 */
#define AWIF_MAX_PEERS 8
#define AWIF_ROUTE_SLOTS 3
struct awif_claims {
    unsigned sources;
    uint8_t observed_ip[16];
};
struct awif_route {
    bool published;
    uint8_t ip[16];
};
struct awif_neighbor {
    bool used;
    uint8_t mac[6];
    struct awif_claims claims;
    struct awif_route routes[AWIF_ROUTE_SLOTS];
};
static struct awif_neighbor awif_neighbors[AWIF_MAX_PEERS];

static void
awif_linklocal_from_mac(const uint8_t mac[6], uint8_t ip[16])
{
    const uint8_t prefix[8] = {0xfe,0x80};
    memcpy(ip,prefix,8);
    ip[8]=mac[0]^2;
    memcpy(ip+9,mac+1,2);
    ip[11]=0xff;
    ip[12]=0xfe;
    memcpy(ip+13,mac+3,3);
}

static bool
awif_linklocal(const uint8_t ip[16])
{
    static const uint8_t prefix[8]={0xfe,0x80};
    static const uint8_t zero[8];
    return ip && !memcmp(ip,prefix,8) && memcmp(ip+8,zero,8);
}

static struct awif_neighbor *
awif_find_neighbor(const uint8_t mac[6], bool reserve)
{
    struct awif_neighbor *empty=NULL;
    for (unsigned i=0;i<AWIF_MAX_PEERS;i++) {
        struct awif_neighbor *p=&awif_neighbors[i];
        if (p->used && !memcmp(p->mac,mac,6)) return p;
        if (!p->used && !empty) empty=p;
    }
    if (!reserve || !empty) return NULL;
    empty->used=true;
    memcpy(empty->mac,mac,6);
    return empty;
}

static bool
awif_has_route(const struct awif_neighbor *p, const uint8_t ip[16])
{
    for (unsigned i=0;i<AWIF_ROUTE_SLOTS;i++)
        if (p->routes[i].published && !memcmp(p->routes[i].ip,ip,16)) return true;
    return false;
}

struct awif_wanted {
    const uint8_t *ip;
    bool from_link;
};

static unsigned
awif_wanted_routes(const struct awif_claims *claims, const uint8_t eui[16], struct awif_wanted out[2])
{
    unsigned n=0;
    bool configured=claims->sources&HK_AWDL_PEER_PROVISIONED;
    bool observed=claims->sources&HK_AWDL_PEER_OVER_AIR_VERIFIED;
    bool link=claims->sources&HK_AWDL_PEER_OVER_AIR_L2;
    if (configured || (link && !observed))
        out[n++]=(struct awif_wanted){eui, !configured};
    if (observed && (!n || memcmp(eui,claims->observed_ip,16)))
        out[n++]=(struct awif_wanted){claims->observed_ip,false};
    return n;
}

static int
awif_publish_route(struct awif_neighbor *p, const struct awif_wanted *want)
{
    struct awif_route *slot=NULL;
    for (unsigned i=0;i<AWIF_ROUTE_SLOTS;i++) {
        if (p->routes[i].published && !memcmp(p->routes[i].ip,want->ip,16)) return 0;
        if (!p->routes[i].published && !slot) slot=&p->routes[i];
    }
    if (!slot) return ENOSPC;
    for (unsigned i=0;i<AWIF_MAX_PEERS;i++)
        if (&awif_neighbors[i]!=p && awif_neighbors[i].used && awif_has_route(&awif_neighbors[i],want->ip))
            return EADDRINUSE;
    int error=awif_peer_notice(awif.ifp,true,p->mac,want->from_link?NULL:want->ip);
    if (error) return error;
    memcpy(slot->ip,want->ip,16);
    slot->published=true;
    return 0;
}

static int
awif_withdraw_other_routes(struct awif_neighbor *p,const struct awif_wanted *keep,unsigned count,bool detached)
{
    int first=0;
    for (unsigned i=0;i<AWIF_ROUTE_SLOTS;i++) {
        struct awif_route *route=&p->routes[i];
        if (!route->published) continue;
        bool wanted=false;
        for (unsigned j=0;j<count;j++)
            if (!memcmp(route->ip,keep[j].ip,16)) wanted=true;
        if (wanted) continue;
        int error=detached?0:awif_peer_notice(awif.ifp,false,p->mac,route->ip);
        if (!error) memset(route,0,sizeof(*route));
        else if (!first) first=error;
    }
    return first;
}

static void
awif_release_empty_neighbor(struct awif_neighbor *p)
{
    if (p->claims.sources) return;
    for (unsigned i=0;i<AWIF_ROUTE_SLOTS;i++)
        if (p->routes[i].published) return;
    memset(p,0,sizeof(*p));
}

static int
awif_peer_event(uint64_t g, uint64_t lease, unsigned source, const uint8_t mac[6],
                const uint8_t ip[16], bool present)
{
    if (!mac || !awif_unicast(mac)) return EINVAL;
    uint8_t eui[16];awif_linklocal_from_mac(mac,eui);
    if (source==HK_AWDL_PEER_OVER_AIR_L2) {
        if (ip) return EINVAL;
    } else if (source==HK_AWDL_PEER_PROVISIONED || source==HK_AWDL_PEER_OVER_AIR_VERIFIED) {
        if (!awif_linklocal(ip) || (source==HK_AWDL_PEER_PROVISIONED && memcmp(eui,ip,16))) return EINVAL;
    } else return EINVAL;
    int error=awif_pin(NULL,g,lease);
    if (error) return error;
    lck_mtx_lock(&awif_peer_lock);
    uint8_t self_ip[16];awif_linklocal_from_mac(awif.config.address,self_ip);
    if (!memcmp(awif.config.address,mac,6) || (ip && !memcmp(self_ip,ip,16))) {
        error=EINVAL;
        goto done;
    }
    struct awif_neighbor *p=awif_find_neighbor(mac,present);
    if (!p) { error=present?ENOSPC:ENOENT; goto done; }

    /* Retry previously failed retirements before using the overlap slot. */
    struct awif_wanted current[2];
    unsigned current_count=awif_wanted_routes(&p->claims,eui,current);
    bool completed_previous = false;
    if (!present && !(p->claims.sources&source))
        completed_previous=awif_has_route(p,ip?ip:eui);
    error=awif_withdraw_other_routes(p,current,current_count,false);
    if (error) goto release;
    struct awif_claims next=p->claims;
    if (present) {
        next.sources|=source;
        if (source==HK_AWDL_PEER_OVER_AIR_VERIFIED) memcpy(next.observed_ip,ip,16);
    } else {
        if (!(next.sources&source) ||
            (source==HK_AWDL_PEER_OVER_AIR_VERIFIED && memcmp(next.observed_ip,ip,16))) {
            error=completed_previous && !(next.sources&source)?0:ENOENT;
            goto release;
        }
        next.sources&=~source;
        if (source==HK_AWDL_PEER_OVER_AIR_VERIFIED) memset(next.observed_ip,0,16);
    }

    struct awif_wanted wanted[2];
    unsigned count=awif_wanted_routes(&next,eui,wanted);
    for (unsigned i=0;i<count;i++) {
        error=awif_publish_route(p,&wanted[i]);
        if (error) goto release;
    }
    p->claims=next;
    error=awif_withdraw_other_routes(p,wanted,count,false);
release:
    awif_release_empty_neighbor(p);
done:
    lck_mtx_unlock(&awif_peer_lock);
    awif_unpin();
    return error;
}

static int
awif_remove_all_peers(bool detached)
{
    int first=0;
    lck_mtx_lock(&awif_peer_lock);
    for (unsigned i=0;i<AWIF_MAX_PEERS;i++) {
        struct awif_neighbor *p=&awif_neighbors[i];
        if (!p->used) continue;
        memset(&p->claims,0,sizeof(p->claims));
        int error=awif_withdraw_other_routes(p,NULL,0,detached);
        if (!first && error) first=error;
        awif_release_empty_neighbor(p);
    }
    lck_mtx_unlock(&awif_peer_lock);
    return first;
}

int lx_os_awdl_peer_present(uint64_t g,uint64_t lease,unsigned source,const uint8_t mac[6],const uint8_t ip[16])
{
    return awif_peer_event(g,lease,source,mac,ip,true);
}
int lx_os_awdl_peer_absent(uint64_t g,uint64_t lease,unsigned source,const uint8_t mac[6],const uint8_t ip[16])
{
    return awif_peer_event(g,lease,source,mac,ip,false);
}
