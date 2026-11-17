/* Round-robin only on locally usable social channels the data carrier
 * does not already visit. Keep the existing discovery airtime budget. */
static unsigned social_cursor;
static unsigned social_visit(struct lx_radio *r,unsigned carrier)
{
    static const unsigned channels[]={6,149,44};
    for(unsigned i=0;i<3;i++){
        unsigned channel=channels[social_cursor];
        social_cursor=(social_cursor+1)%3;
        if(channel!=carrier && auto_channel(r,channel,true))return channel;
    }
    return 0;
}
