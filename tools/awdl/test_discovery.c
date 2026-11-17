/* Pure encoding, with no RF calls. Two independent reads: the shared typed
 * synchronization decoder and a byte-level check of OWL's MIF-only TLVs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "awdl_discovery.h"
#include "awdl_rx_action.h"

static const uint8_t self[] = {2,0,0,0,0,7};
static const uint8_t core[] = {4,5,18,24,6};
static struct lx_awdl_discovery_window window(void)
{
    struct lx_awdl_discovery_window w = {
        .anchor_us=1000, .now_us=1000+2*65536+9*1024,
        .grant_end_us=1000+5*65536, .ieee_sequence=0xabc, .af_period_tu=128
    };
    return w;
}
static size_t check_tlv(const uint8_t *p, size_t end, size_t off, uint8_t tag)
{
    assert(off + 3 <= end && p[off] == tag);
    size_t n = (size_t)p[off + 1] + ((size_t)p[off + 2] << 8);
    assert(n <= end - off - 3);
    return off + n + 3;
}
static void check_frame(unsigned subtype, const struct lx_awdl_discovery_ht *ht,bool dualband)
{
    struct lx_awdl_discovery_window w = window();
    w.dualband_supported=dualband;
    uint8_t bytes[LX_AWDL_DISCOVERY_MAX + 2];
    memset(bytes,0x42,sizeof(bytes));
    uint8_t *p = bytes+1;
    size_t n = lx_awdl_discovery_action(p,sizeof(bytes)-2,self,subtype,&w,ht);
    assert(n == (subtype==0 ? 259u : ht ? 289u : 278u));
    assert(bytes[0]==0x42 && p[n]==0x42);
    assert(p[0]==0xd0 && p[22]==0xc0 && p[23]==0xab && !memcmp(p+10,self,6));
    struct awdl_rx_action a;
    assert(awdl_rx_decode_action(p,n,&a)==AWDL_RX_OK);
    assert(a.kind==(subtype==0 ? LX_AWDL_PSF : LX_AWDL_MIF));
    assert(a.subtype == subtype && a.phy_tx_us==w.now_us && a.target_tx_us==w.now_us);
    assert(a.fields==(AWDL_RX_HAS_SYNC|AWDL_RX_HAS_ELECTION|AWDL_RX_HAS_ELECTION_V2|
                      AWDL_RX_HAS_SEQUENCE|AWDL_RX_HAS_VERSION));
    assert(!a.duplicate_fields && a.sync.next_aw_sequence==8 && a.sync.tx_down_counter_tu==55);
    assert(a.sync.action_frame_period_tu==128 && a.sync.remaining_aw_tu==7);
    assert(!memcmp(a.sync.master,self,6) && !memcmp(a.election.master,self,6));
    assert(!memcmp(a.election_v2.master,self,6) && !memcmp(a.election_v2.sync_address,self,6));
    assert(a.sequence.entries_decoded && a.sequence.count==16);
    for (unsigned i=0;i<16;i++) {
        bool granted=i>=2 && i<5;
        assert(a.sequence.entries[i].number==(granted ? 6u : 0u));
        assert(a.sequence.entries[i].flags_or_opclass==(granted ? 81u : 0u));
        assert(!memcmp(&a.sequence.entries[i], &a.sync.sequence.entries[i], sizeof(a.sequence.entries[i])));
    }
    size_t off=40;
    for (unsigned i=0;i<sizeof(core);i++) off=check_tlv(p,n,off,core[i]);
    if (subtype==3) {
        if (ht) {
            size_t next=check_tlv(p,n,off,7);
            assert(next-off==11);
            const uint8_t *v=p+off+3;
            assert(v[2]==(uint8_t)ht->capabilities && v[3]==ht->capabilities>>8);
            assert(v[4]==(ht->ampdu_parameters&0x1f) && v[5]==ht->rx_mcs0);
            off=next;
        }
        size_t next=check_tlv(p,n,off,16);
        assert(next-off==19 && p[off+3]==3 && p[off+4]==12);
        assert(!memcmp(p+off+5,"hackios-awdl",12));
        assert(p[next-2]==0xc0 && p[next-1]==0x0c);
        off=next;
    }
    size_t next=check_tlv(p,n,off,12);
    assert(next-off==16 && p[off+3]==(dualband?0x24:4) && p[off+4]==3 && p[off+5]=='X' && p[off+6]=='0' && p[off+7]==0 && p[off+8]==1 && p[off+9]==0);
    assert(!memcmp(p+off+10,self,6));
    assert(check_tlv(p,n,next,21)==n && a.advertised_version==0x34 && a.device_class==2);
}
int main(void)
{
    struct lx_awdl_discovery_ht test_ht={.capabilities=0x016e,.ampdu_parameters=0x7b,.rx_mcs0=0x7f};
    for(unsigned dual=0;dual<2;dual++) {
        check_frame(0,NULL,dual);
        check_frame(0,&test_ht,dual);
        check_frame(3,NULL,dual);
        check_frame(3,&test_ht,dual);
    }
    struct lx_awdl_discovery_window w=window();
    uint8_t p[LX_AWDL_DISCOVERY_MAX], old[LX_AWDL_DISCOVERY_MAX];
    memset(old,0xa5,sizeof(old));
    memcpy(p,old,sizeof(p));
    assert(!lx_awdl_discovery_action(p,288,self,3,&w,&test_ht));
    assert(!lx_awdl_discovery_action(p,sizeof(p),self,1,&w,NULL));
    assert(!lx_awdl_discovery_action(p,sizeof(p),(uint8_t[6]){2},0,&w,NULL));
    w.grant_end_us=w.now_us+1024;
    assert(!lx_awdl_discovery_action(p,sizeof(p),self,3,&w,NULL));
    assert(!memcmp(p,old,sizeof(p)));
    w=window();
    w.now_us=w.anchor_us+15u*65536+512;
    w.grant_end_us=w.anchor_us+17u*65536;
    size_t n=lx_awdl_discovery_action(p,sizeof(p),self,3,&w,NULL);
    struct awdl_rx_action a;
    assert(n && awdl_rx_decode_action(p,n,&a)==AWDL_RX_OK);
    assert(a.sequence.entries[15].number==6 && a.sequence.entries[0].number==6 &&
           a.sequence.entries[1].number==0);
    w.now_us=w.anchor_us+(1ull<<32);
    w.grant_end_us=w.now_us+16u*65536;
    n=lx_awdl_discovery_action(p,sizeof(p),self,0,&w,NULL);
    assert(n && awdl_rx_decode_action(p,n,&a)==AWDL_RX_OK);
    assert(a.sync.next_aw_sequence==0 && a.phy_tx_us==(uint32_t)w.now_us);
    for (unsigned i=0;i<16;i++) assert(a.sequence.entries[i].number==6);
    /* A followed master's counter and phase are retained while the local
     * availability still advertises only the carrier's actual grant. */
    w=window();w.base_aw=104;w.channel=149;w.follow_master=true;
    const uint8_t master[6]={2,0,0,0,0,9}, parent[6]={2,0,0,0,0,8};
    memcpy(w.master,master,6);memcpy(w.sync_address,parent,6);
    w.master_counter=123;w.master_metric=400;w.distance=2;
    n=lx_awdl_discovery_action(p,sizeof(p),self,3,&w,NULL);
    assert(n && awdl_rx_decode_action(p,n,&a)==AWDL_RX_OK);
    assert(a.sync.next_aw_sequence==112 && a.sync.tx_down_counter_tu==55);
    assert(!memcmp(a.sync.master,master,6) && !memcmp(a.election_v2.sync_address,parent,6));
    assert(a.election_v2.master_counter==123 && a.election_v2.master_metric==400 && a.election_v2.distance==2);
    for(unsigned i=0;i<16;i++) {
        bool granted=i>=12 && i<15;
        assert(a.sequence.entries[i].number==(granted?149:0));
    }
    puts("PASS PSF/MIF, actual optional HT, fixed local label, typed field round trip, grant/sequence bounds and clock wrap");
}
