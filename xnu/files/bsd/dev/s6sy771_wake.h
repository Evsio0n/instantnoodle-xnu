/* IN2010 S6SY771 low-power tap control. Called only by the touch worker.
 * OEM sec_enable_black_gesture on the single_tap_support board uses mask
 * 0xffff and up to20 E4 writes with a ten-ms readback; the native debug
 * state helper also reads the 16-bit gesture mask. */
#ifndef HK_S6SY771_WAKE_H
#define HK_S6SY771_WAKE_H
struct tw_io {
    int (*write)(const uint8_t *,unsigned);
    int (*read)(uint8_t,uint8_t *,unsigned);
    void (*wait_ms)(uint32_t);
};
struct tw_state {bool armed,posted;unsigned idle_irqs,stage,attempts;uint16_t mask;uint8_t power;};
enum { TW_MASK=0x39,TW_POWER=0xe4,TW_EVENT=0x60,TW_POINTS=0x63,TW_CLEAR=0x62 };
static inline bool tw_mask(struct tw_state *s,const struct tw_io *io,uint8_t value)
{
    uint8_t cmd[]={TW_MASK,value,value},readback[2]={0,0};
    s->stage=2;
    if(io->write(cmd,sizeof(cmd)))return false;
    s->stage=3;
    if(io->read(TW_MASK,readback,2))return false;
    s->mask=(uint16_t)readback[0]|(uint16_t)readback[1]<<8;
    return readback[0]==value && readback[1]==value;
}
static inline bool tw_power(struct tw_state *s,const struct tw_io *io,uint8_t value)
{
    const uint8_t cmd[]={TW_POWER,value};
    for(unsigned i=0;i<20;i++){
        uint8_t got=0xff;
        s->stage=4;s->attempts=i+1;
        if(io->write(cmd,sizeof(cmd)))return false;
        io->wait_ms(10);
        s->stage=5;
        if(io->read(TW_POWER,&got,1))return false;
        s->power=got;
        if(got==value)return true;
    }
    return false;
}
static inline bool tw_enter(struct tw_state *s,const struct tw_io *io)
{
    uint8_t mode=0xff,clear=TW_CLEAR;
    *s=(struct tw_state){.stage=1,.power=0xff};
    if(io->read(TW_POWER,&mode,1))return false;
    s->power=mode;
    if(mode!=0 || io->write(&clear,1))return false;
    if(!tw_mask(s,io,0xff) || !tw_power(s,io,1))return false;
    s->armed=true;s->stage=0;return true;
}
static inline bool tw_exit(struct tw_state *s,const struct tw_io *io)
{
    const uint8_t clear=TW_CLEAR;
    s->armed=false;
    return tw_mask(s,io,0) && tw_power(s,io,0) && !io->write(&clear,1);
}
enum tw_action {TW_IGNORE,TW_WAKE,TW_RESET};
static inline enum tw_action tw_poll(struct tw_state *s,const struct tw_io *io)
{
    uint8_t event[8],points[18];
    if(!s->armed || io->read(TW_EVENT,event,sizeof(event)))return TW_RESET;
    /* Bounded handling of stale/misowned/wedged GPIO39. This is invoked
     * only on an IRQ or low pin while dark; no periodic dark-screen scan. */
    if(!event[0] || (event[0]&3)!=2){
        if(++s->idle_irqs>=8)return TW_RESET;
        return TW_IGNORE;
    }
    if(event[1]!=0x0e && event[1]!=0)return ++s->idle_irqs>=32?TW_RESET:TW_IGNORE;
    unsigned n=event[2]<sizeof(points)?event[2]:sizeof(points);
    if(n<3 || io->read(TW_POINTS,points,n))return TW_RESET;
    if(s->posted)return ++s->idle_irqs>=32?TW_RESET:TW_IGNORE;
    s->idle_irqs=0;s->posted=true;return TW_WAKE;
}
#endif
