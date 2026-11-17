#include "psf.h"
#include "awdl_rx_action.h"
#include <assert.h>
#include <string.h>
int main(void) {
	uint8_t guarded[LX_AWDL_PSF_LENGTH+2];
	memset(guarded,0xc5,sizeof(guarded));
	const uint8_t self[6]={2,0,0,0,0,1};
	size_t n=lx_awdl_local_psf(guarded+1,LX_AWDL_PSF_LENGTH,self,108192,100000);
	assert(n==LX_AWDL_PSF_LENGTH && guarded[0]==0xc5 && guarded[n+1]==0xc5);
	struct awdl_rx_action a;
	assert(awdl_rx_decode_action(guarded+1,n,&a)==AWDL_RX_OK && a.kind==LX_AWDL_PSF);
	assert((a.fields & (AWDL_RX_HAS_SYNC|AWDL_RX_HAS_ELECTION|AWDL_RX_HAS_ELECTION_V2|AWDL_RX_HAS_SEQUENCE|AWDL_RX_HAS_VERSION))==
	    (AWDL_RX_HAS_SYNC|AWDL_RX_HAS_ELECTION|AWDL_RX_HAS_ELECTION_V2|AWDL_RX_HAS_SEQUENCE|AWDL_RX_HAS_VERSION));
	assert(a.phy_tx_us==108192 && a.target_tx_us==108192 && a.sync.aw_period_tu==16);
	assert(a.sync.tx_down_counter_tu==56 && a.sequence.count==16);
	assert(a.sequence.entries[5].number==6 && a.sequence.entries[6].number==0);
	assert(a.election.self_metric==60 && a.election_v2.self_metric==60);
	assert(!lx_awdl_local_psf(guarded,12,self,108192,100000));
	assert(!lx_awdl_local_psf(guarded,sizeof(guarded),self,200000,100000));
	return 0;
}
