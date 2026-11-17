/* Protocol lifecycle vectors: a rotating radio address is a new peer;
 * observed IPv6 changes must not extend its management-frame lifetime. */
#include "peer_table.h"
#include "awdl_discovery.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    struct lx_awdl_peer_table table={0};
    struct awdl_rx_action decoded;
    const uint8_t self[6]={2,0,0,0,0,1}, peer[6]={2,0,0,0,0,2}, rotated[6]={2,0,0,0,0,3};
    uint8_t frame[LX_AWDL_DISCOVERY_MAX], ip[40]={0x60};
    struct lx_awdl_discovery_window w={.now_us=110000,.anchor_us=100000,
        .grant_end_us=1100000,.af_period_tu=128,.ieee_sequence=1};
    const struct lx_awdl_discovery_ht ht={.capabilities=0x016e,.ampdu_parameters=0x1b,.rx_mcs0=0x7f};
    size_t n=lx_awdl_discovery_action(frame,sizeof(frame),peer,3,&w,&ht);
    assert(n);
    struct lx_awdl_peer_metadata metadata;
    assert(lx_awdl_peer_decode(frame,n,&decoded,&metadata)==AWDL_RX_OK);
    assert(metadata.path_flags==0x0304 && metadata.has_social_channels && metadata.social_channels==1);
    assert(metadata.has_data_address && !memcmp(metadata.data_address,peer,6));
    int first=lx_awdl_peer_observe(&table,frame,n,self,2437,w.now_us,&decoded);
    assert(first>=0 && lx_awdl_peer_select(&table,self,w.now_us)==first);
    assert(table.peers[first].has_ht && table.peers[first].ht_capabilities==0x016e);
    assert(table.peers[first].ht_ampdu==0x1b && table.peers[first].ht_mcs0==0x7f);
    uint64_t serial=table.peers[first].serial;
    memcpy(ip+8,table.peers[first].eui64,16);
    assert(lx_awdl_peer_note_ipv6(&table,first,serial,peer,ip,sizeof(ip),w.now_us+1));
    ip[23]^=0x10;
    assert(lx_awdl_peer_note_ipv6(&table,first,serial,peer,ip,sizeof(ip),w.now_us+2));
    assert(!memcmp(table.peers[first].observed_ipv6,ip+8,16));
    assert(!lx_awdl_peer_note_ipv6(&table,first,serial,rotated,ip,sizeof(ip),w.now_us+3));
    n=lx_awdl_discovery_action(frame,sizeof(frame),rotated,3,&w,NULL);
    int second=lx_awdl_peer_observe(&table,frame,n,self,5220,w.now_us+3,&decoded);
    assert(second>=0 && second!=first && table.peers[second].serial!=serial);
    assert(lx_awdl_peer_expired(&table,w.now_us+LX_AWDL_PEER_TTL_US)&(1u<<first));
    assert(!lx_awdl_peer_note_ipv6(&table,first,serial,peer,ip,sizeof(ip),w.now_us+LX_AWDL_PEER_TTL_US));
    assert(!lx_awdl_peer_remove(&table,first,serial+1));
    assert(lx_awdl_peer_remove(&table,first,serial));
    assert(!table.peers[first].serial && table.peers[second].serial);
    n=lx_awdl_discovery_action(frame,sizeof(frame),peer,3,&w,NULL);
    assert(lx_awdl_peer_observe(&table,frame,n,self,2437,w.now_us+1,&decoded)==first);
    assert(table.peers[first].serial!=serial);
    /* The management transmitter and advertised data address are distinct
     * protocol fields. Updating one must retain the peer's identity while
     * replacing its link address and invalidating an old IPv6 observation. */
    const uint8_t data_address[6]={2,0,0,0,0,4};
    bool changed=false;
    for(size_t at=40;at+3<=n;at+=3+awdl_rx_le16(frame+at+1)) {
        if(frame[at]==12) {memcpy(frame+at+3+7,data_address,6);changed=true;break;}
    }
    assert(changed);
    serial=table.peers[first].serial;
    assert(lx_awdl_peer_observe(&table,frame,n,self,2437,w.now_us+10,&decoded)==first);
    const struct lx_awdl_peer *p=table.peers+first;
    assert(p->serial==serial && !memcmp(p->mac,peer,6) && !memcmp(p->data_mac,data_address,6));
    assert(p->data_address_present && !p->data_address_matches && !p->has_observed_ipv6);
    memcpy(ip+8,p->eui64,16);
    assert(!lx_awdl_peer_note_ipv6(&table,first,serial,peer,ip,sizeof(ip),w.now_us+11));
    assert(lx_awdl_peer_note_ipv6(&table,first,serial,data_address,ip,sizeof(ip),w.now_us+11));
    uint8_t targets[LX_AWDL_PEER_SLOTS][6];
    assert(lx_awdl_group_targets(&table,w.now_us+12,targets,LX_AWDL_PEER_SLOTS)==2);
    assert(lx_awdl_group_targets(&table,w.now_us+12,targets,1)==1);
    /* A different transmitter may advertise the same data link address.
     * Retiring its older observation must not remove the fresh route. */
    memcpy(frame+10,rotated,6);
    assert(lx_awdl_peer_observe(&table,frame,n,self,2437,w.now_us+12,&decoded)==second);
    assert(lx_awdl_group_targets(&table,w.now_us+13,targets,LX_AWDL_PEER_SLOTS)==1);
    assert(lx_awdl_other_claim(&table,first,data_address,NULL,w.now_us+13));
    assert(!lx_awdl_other_claim(&table,first,data_address,ip+8,w.now_us+13));
    assert(lx_awdl_peer_note_ipv6(&table,second,table.peers[second].serial,data_address,ip,sizeof(ip),w.now_us+13));
    assert(lx_awdl_other_claim(&table,first,data_address,ip+8,w.now_us+14));
    assert(!lx_awdl_other_claim(&table,second,data_address,NULL,w.now_us+LX_AWDL_PEER_TTL_US+11));
    puts("PASS radio identity rotation, IPv6 replacement, expiry and serial-scoped removal");
}
