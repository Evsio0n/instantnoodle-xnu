/* A periodic native browse subscription does not need multiple identical
 * unsent questions for the same real link recipient. Coalesce only exact
 * queued bytes in the same lease; never replace answers, TCP or flights,
 * reset the original queue age, or manufacture a completion receipt. */
static bool live_pending_question(const struct live_fifo *q,uint64_t lease,
    const uint8_t target[6],const uint8_t *frame,size_t n)
{
    if(!hk_mdns_shape(frame,n,false))return false;
    for(unsigned i=0;i<q->count;i++) {
        const struct live_frame *f=&q->slot[(q->first+i)%LIVE_DEPTH];
        if(f->lease==lease && f->n==n && !memcmp(f->link_target,target,6) &&
           !memcmp(f->bytes,frame,n))return true;
    }
    return false;
}
