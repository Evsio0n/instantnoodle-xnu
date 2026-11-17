#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include "hk_light_policy.h"
#include "hk_user_preference.h"
#include "slpi_dma_route.h"

static uint32_t bits(float value) {
    uint32_t u;
    memcpy(&u,&value,sizeof(u));
    return u;
}

static unsigned ramp(lp_state &s,uint64_t time) {
    unsigned old=s.current;
    unsigned next=lp_ramp_step(&s,time);
    if(next){
        unsigned distance=next>old?next-old:old-next;
        assert(distance<=10 && next<=2047);
        s.current=next;s.last_apply=time;s.writes++;
        return distance;
    }
    return 0;
}

struct Panel {
    lp_state s{};
    uint64_t clock=1000000;
    Panel() { s.armed=true;s.current=s.baseline=224;s.last_apply=s.last_sample=clock; }
    void sample(float lux) {
        for(unsigned i=0;i<9;i++){clock+=20000;ramp(s,clock);}
        clock+=20000;
        lp_observe(&s,clock,clock,s.sample_sequence+1,bits(lux));
        ramp(s,clock);
    }
    void hold(float lux,unsigned n) { for(unsigned i=0;i<n;i++)sample(lux); }
};

static unsigned mapped(float lux) {
    uint32_t n=lp_interp(lp_millilux(bits(lux)),lux_nits,sizeof(lux_nits)/sizeof(*lux_nits));
    return (lp_interp(n,nits_backlight,sizeof(nits_backlight)/sizeof(*nits_backlight))+127)/255;
}

struct Mmu {
    std::vector<uint32_t> v=std::vector<uint32_t>(0x100000/4);
    unsigned flushes=0;bool stolen=false;
    Mmu() {
        v[0]=0x400;v[0x20/4]=0x4c017e57;v[0x24/4]=0x6000004a;
        for(unsigned i=0;i<87;i++)v[(0xc00+i*4)/4]=0x200ff;
        for(unsigned i=0;i<3;i++) {
            v[(0x800+4*i)/4]=0x80001001+i;v[(0xc00+4*i)/4]=6+i;
        }
    }
    static uint32_t read(void *p,unsigned reg) { return ((Mmu*)p)->v.at(reg/4); }
    static void write(void *p,unsigned reg,uint32_t v) { ((Mmu*)p)->v.at(reg/4)=v; }
    static bool flush(void *p,unsigned n) { assert(n==9);((Mmu*)p)->flushes++;return true; }
    static bool use(void *p) {
        auto &m=*(Mmu*)p;
        assert(m.v[0x80c/4]==0x80000541 && m.v[0xc0c/4]==9);
        if(m.stolen)m.v[0xc0c/4]=7;
        return true;
    }
    slpi_route_io io() { return {this,read,write,flush}; }
};

static void replay() {
    lp_state state{};
    state.armed=true;state.current=state.baseline=224;
    unsigned seq,status,bits,count=0;
    unsigned long long ticks,last=0,first=0;uint64_t pump=1;
    while(scanf("%u %llu %u %x",&seq,&ticks,&status,&bits)==4) {
        assert(seq==count && ticks>last && status==3);
        if (!count)first=ticks;
        uint64_t us=(ticks-first)*5/96+1;
        while(pump<us){ramp(state,pump);pump+=20000;}
        lp_observe(&state,us,ticks,seq,bits);
        if(pump==us){ramp(state,pump);pump+=20000;}
        last=ticks;count++;
    }
    assert(count==1196 && state.writes>4 && state.current>272 && state.current<=2047);
    printf("Run 074253: %u physical samples replayed, final algorithm target %u DBV, %u steps\n",count,state.current,state.writes);
}

int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--slider")){
        lp_slider_state slider{};unsigned total=0,changes=0;
        unsigned long long raw;
        unsigned lit;
        while(scanf("%llx %u",&raw,&lit)==2){total++;changes+=lp_slider_changed(&slider,raw,lit!=0);}
        assert(slider.seen);
        printf("%u %u\n",total,changes);return 0;
    }
    if(argc>1 && !strcmp(argv[1],"--goals")){
        unsigned oldMax=0,newMax=0,oldN=0,newN=0,direction=0;
        unsigned seq,initial,oldTarget,goal;
        unsigned long long stamp,prior=0,pump=0;lp_state state{};
        while(scanf("%u %llu %u %u %u",&seq,&stamp,&initial,&oldTarget,&goal)==5){
            if(!prior || stamp-prior>350000){
                state={};state.current=initial;state.ramp_clock=stamp;pump=stamp;
                state.accepted_light=state.armed=true;
            }
            while(pump<stamp){
                unsigned step=ramp(state,pump);newN+=!!step;
                if(step>newMax)newMax=step;
                pump+=20000;
            }
            state.last_sample=stamp;state.goal=goal;
            unsigned oldStep=oldTarget>initial?oldTarget-initial:initial-oldTarget;
            if(oldStep>oldMax)oldMax=oldStep;
            direction|=oldTarget>initial?1:2;oldN++;prior=stamp;
        }
        assert(direction==3 && oldMax>=70 && newMax<=8 && newN>oldN);
        printf("Run 072309: %u measured 5Hz changes, largest %u DBV; replay 20ms ticks %u changes, largest %u DBV, both directions\n",oldN,oldMax,newN,newMax);
        return 0;
    }
    if(argc>1){replay();return 0;}
    assert(mapped(0)==26 && mapped(1)==181 && mapped(10)==600);
    assert(mapped(64.333336f)==945 && mapped(1000)==1289 && mapped(20000)==2047);
    Panel p;p.hold(64.333336f,6);assert(p.s.writes==0);
    p.hold(64.333336f,60);assert(p.s.current>=943 && p.s.current<=945);
    const auto old=p.s.current;
    p.hold(4,3);p.hold(64.333336f,9);assert(p.s.current==old);
    p.hold(0,80);assert(p.s.current<=28);
    p.hold(20000,100);assert(p.s.current>=2045 && p.s.goal==2047);
    for(unsigned i=0;i<30;i++) {p.hold(1,50);p.hold(20000,50);}
    assert(p.s.writes>240 && p.s.current>=2045);
    Panel same;same.hold(10,50);auto last=same.s.current;same.hold(12,50);assert(same.s.current==last);
    Panel rising;rising.hold(5,50);last=rising.s.current;rising.hold(12,50);assert(rising.s.current>last);
    Panel falling;falling.hold(20,50);last=falling.s.current;falling.hold(12,50);assert(falling.s.current<last);
    auto accepted=p.s.last_tick;
    lp_observe(&p.s,p.clock,p.s.last_tick,0,bits(0));
    assert(p.s.last_tick==accepted);
    lp_observe(&p.s,p.clock+1,p.s.last_tick+1,0,0x7fc00000u);
    assert(p.s.last_tick==accepted);
    // Observe exactly 384 counts in one second at 50 Hz, with four fresh
    // sensor intervals in between. Half-period callbacks cannot double the rate.
    lp_state rate{};rate.accepted_light=rate.armed=true;
    rate.current=200;rate.goal=1600;rate.ramp_clock=100;rate.last_sample=100;
    for(unsigned i=1;i<=50;i++){
        uint64_t now=100+20000*i;
        if(i%10==0)rate.last_sample=now;
        assert(!lp_ramp_step(&rate,now-10000));
        unsigned step=ramp(rate,now);assert(step==7||step==8);
    }
    assert(rate.current==584);
    rate.current=rate.goal;
    for(unsigned i=0;i<10;i++)ramp(rate,rate.last_sample+=200000);
    uint64_t quiet=rate.last_sample;
    assert(rate.ramp_clock==quiet);
    rate.goal=300;
    assert(!lp_ramp_step(&rate,quiet));
    assert(ramp(rate,quiet+20000)==7);
    rate.last_sample=quiet+500000;
    assert(ramp(rate,quiet+500000)<=10);
    assert(!lp_ramp_step(&rate,rate.last_sample+LP_STALE_US));
    rate.armed=false;assert(!lp_ramp_step(&rate,rate.last_sample+20000));
    lp_pref pref{};assert(lp_preference(&pref,LP_PREF_ON,1)==LP_ENABLE);
    assert(lp_pref_fresh(&pref,2) && !lp_pref_fresh(&pref,3500002));
    lp_pref_manual(&pref);lp_preference(&pref,LP_PREF_UNKNOWN,3);lp_preference(&pref,LP_PREF_ON,4);
    assert(!lp_pref_fresh(&pref,4));lp_preference(&pref,LP_PREF_OFF,5);
    assert(lp_preference(&pref,LP_PREF_ON,6)==LP_ENABLE);
    lp_pref_manual(&pref);
    assert(!lp_pref_fresh(&pref,6));
    lp_preference(&pref,LP_PREF_ON,7);
    assert(!lp_pref_fresh(&pref,7)); // same lit session remains with the slider
    lp_pref_screen_wake(&pref);
    assert(!lp_pref_fresh(&pref,8)); // power alone cannot turn the preference on
    assert(lp_preference(&pref,LP_PREF_ON,9)==LP_ENABLE && lp_pref_fresh(&pref,9));
    lp_preference(&pref,LP_PREF_OFF,10);
    lp_pref_screen_wake(&pref);lp_preference(&pref,LP_PREF_OFF,11);
    assert(!lp_pref_fresh(&pref,11)); // wake preserves an actual disabled preference
    Mmu m;auto io=m.io();auto before=m.v;
    auto done=slpi_route_run(&io,4,0x112340000,true,Mmu::use,&m);
    assert(done.mapped && done.bank==9 && done.restored && done.reclaimable && m.v==before && m.flushes==2);
    m.stolen=true;auto unknown=slpi_route_run(&io,4,0x112340000,true,Mmu::use,&m);
    assert(!unknown.reclaimable && m.v[0xc0c/4]==7);
    Mmu unavailable;auto no=unavailable.io();
    auto skipped=slpi_route_run(&no,9,0x112340000,true,Mmu::use,&unavailable);
    assert(!skipped.attempted && skipped.reclaimable && unavailable.flushes==0);
    unavailable.v[0x800/4]=0x80000541;
    skipped=slpi_route_run(&no,4,0x112340000,true,Mmu::use,&unavailable);
    assert(!skipped.attempted && skipped.reclaimable);
    puts("PASS board curve ranges, high/low transitions, hysteresis, replay/stale input, manual handoff, SMMU ownership and rollback");
}
