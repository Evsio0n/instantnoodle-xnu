#include "peer_windows.h"
#include "psf.h"
#include <assert.h>
#include <string.h>
int main(void) {
	struct awdl_window_clock local={0}, peer={0};
	struct awdl_window w;
	const uint64_t start=100000;
	assert(awdl_clock_local(&local,start,6));
	assert(awdl_window_at(&local,start,8,&w) && !w.in_guarded_interval);
	assert(awdl_window_at(&local,start+8192,8,&w) && w.in_guarded_interval);
	assert(awdl_window_at(&local,start+65536-8192,8,&w) && !w.in_guarded_interval);
	uint8_t frame[LX_AWDL_PSF_LENGTH];
	const uint8_t address[]={2,0,0,0,0,1};
	struct awdl_rx_action a;
	assert(lx_awdl_local_psf(frame,sizeof(frame),address,start+8192,start)==sizeof(frame));
	assert(awdl_rx_decode_action(frame,sizeof(frame),&a)==AWDL_RX_OK);
	const uint64_t lag=2000;
	assert(awdl_clock_from_action(&a,start+8192+lag,&peer));
	assert(peer.from_peer && peer.anchor_us==start+lag && peer.slots[6]==0);
	assert(awdl_window_at(&peer,start+lag+32768,8,&w) && w.aw==2 && w.channel==6 && w.in_guarded_interval);
	assert(awdl_window_at(&peer,start+lag+6*65536+10000,8,&w) && w.channel==0 && !w.in_guarded_interval);
	a.sync.next_aw_sequence=65532;
	assert(awdl_clock_from_action(&a,start+8192+lag,&peer));
	assert(awdl_window_at(&peer,peer.anchor_us+65536,0,&w) && w.aw==0 && w.channel==6);
	struct awdl_window_clock saved=peer;
	a.sync.tx_down_counter_tu=255;
	assert(!awdl_clock_from_action(&a,start+8192+lag,&peer));
	assert(!memcmp(&saved,&peer,sizeof(peer)));
	assert(!awdl_window_at(&peer,peer.valid_until_us,0,&w));
	return 0;
}
