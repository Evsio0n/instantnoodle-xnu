/* The native IPv6 group is copied unchanged once per currently known link
 * recipient. Master election does not choose a multicast service recipient.
 * Each copy has its own normal bounded queue ticket and hardware receipt. */
#include "wire_checksum.h"
static int live_output(void *ctx,uint64_t gen,uint64_t lease,const uint8_t *frame,size_t n)
{
	if(ctx!=&live || !frame || n<54 || n>14+LIVE_MTU || frame[12]!=0x86 || frame[13]!=0xdd ||
	    !lx_awdl_valid_ipv6(frame+14,n-14) || !live_mac_local(frame+6)) return EINVAL;
	bool group=live_group(frame,frame+14);
	if(!group && !auto_route(frame,frame+38)) return EHOSTUNREACH;
	unsigned cls=live_proto_of(frame+14,n-14),tag=0,depth=0,count=0;
	if(frame[20]==17)lx_tell("awdl-ntx: native_checksum class=%s valid=%d source_scope_embedded=%u\n",
	    live_proto_name(cls),awdl_wire_checksum(frame+14,n-14),!!(frame[24]|frame[25]));
	uint8_t targets[LX_AWDL_PEER_SLOTS][6]={{0}};
	uint64_t now=lx_os_nanoseconds()/1000ull;
	lx_os_lock();
	struct live_fifo *q=group?&live.groups:&live.tx;
	if(group) {
		count=lx_awdl_group_targets(&air.table,now,targets,LX_AWDL_PEER_SLOTS);
	} else {memcpy(targets[0],frame,6);count=1;}
	/* With no observation yet, retain one pending copy. Its link recipient
	 * will be resolved from a real future observation by the owner. */
	if(!count)count=1;
	int error=0;
	if(!live_current(gen,lease) || !live.accept || live.stop) error=ENETDOWN;
	else if(count>LIVE_DEPTH-q->count) {live.queue_unavailable++;error=ENOBUFS;}
	else for(unsigned i=0;i<count;i++) {
		if(!live_put(q,lease,frame,n)) {error=ENOBUFS;break;}
		struct live_frame *f=&q->slot[(q->first+q->count-1)%LIVE_DEPTH];
		f->queued_us=now;f->tag=++live.next_tag;f->proto=cls;memcpy(f->link_target,targets[i],6);
		if(!tag)tag=f->tag;
	}
	depth=q->count;
	lx_os_unlock();
	lx_tell("awdl-ntx: enqueue ticket=%u class=%s group=%u copies=%u lane_depth=%u rc=%d\n",
	    tag,live_proto_name(cls),group,count,depth,error);
	return error;
}
