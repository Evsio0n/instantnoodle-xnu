#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void say(const char *fmt,...) { (void)fmt; }
#define NSEC_PER_SEC 1000000000ull
static bool cell_started;
static struct {int registered, bars, dbm; char operator_name[64],state[120]; bool event_report_on; uint64_t next_poll;} cell;
#include "model.inc"

struct frame { uint8_t bytes[1024]; unsigned n; };
static struct frame card, slots;
static struct { uint8_t data[1024]; unsigned length, result, error; } qmi_reply;
static bool have_endpoint=true, interrupt_poll;
static int fail_slots, fail_card;
static unsigned registrations;
static uint64_t fake_now=92*NSEC_PER_SEC;
static uint8_t dms_mode;
static uint64_t mhi_op_nanoseconds(void *p) { (void)p; return fake_now; }
static bool qmi_find(unsigned srv, uint32_t *node, uint32_t *port)
{
	assert(srv==11); *node=3; *port=51; return have_endpoint;
}
static const uint8_t *qmi_tlv(int t,uint32_t *n)
{
	if (t==1) { *n=1; return &dms_mode; }
	static const uint8_t mask[4]={0x11,0,0,0};
	assert(t==0x10); *n=4; return mask;
}
static int qmi_call_at(unsigned node,unsigned port,unsigned msg,const uint8_t *tlv,unsigned len,unsigned timeout)
{
	assert(node==3 && port==51 && timeout);
	if (msg==0x2e) {
		assert(len==7 && tlv[0]==1 && tlv[3]==0x11);
		registrations++; return 0;
	}
	assert(tlv==NULL && len==0 && (msg==0x2f || msg==0x47));
	if (interrupt_poll && msg==0x47) { cell_sim_invalidate(); interrupt_poll=false; }
	struct frame *f=msg==0x2f?&card:&slots;
	memcpy(qmi_reply.data,f->bytes,f->n); qmi_reply.length=f->n; qmi_reply.result=qmi_reply.error=0;
	return msg==0x2f?fail_card:fail_slots;
}
#include "query.inc"

#define QMI_PORT 0x4000u
struct frame_header { uint32_t from_node, from_port, to_port, size; };
static void indication(struct frame_header h,const uint8_t *q,uint32_t tl)
{
#include "indication.inc"
}
static void notify(unsigned message,const struct frame *f,bool right_peer)
{
	uint8_t q[1024]={4,0,0,(uint8_t)message,(uint8_t)(message>>8)};
	assert(f->n>=7);unsigned n=f->n-7;
	q[5]=(uint8_t)n;q[6]=(uint8_t)(n>>8);memcpy(q+7,f->bytes+7,n);
	indication((struct frame_header){3,right_peer?51u:52u,QMI_PORT,n+7},q,n);
}
static void tlv(struct frame *f,uint8_t tag,const uint8_t *data,unsigned len)
{
	assert(f->n+3+len<=sizeof(f->bytes));
	f->bytes[f->n++]=tag; f->bytes[f->n++]=(uint8_t)len; f->bytes[f->n++]=(uint8_t)(len>>8);
	memcpy(f->bytes+f->n,data,len); f->n+=len;
}
static void response(struct frame *f) {
	memset(f,0,sizeof(*f)); const uint8_t result[4]={0}; tlv(f,2,result,4);
}
static void logical(uint8_t first,uint8_t second,uint8_t app_state,bool valid)
{
	response(&card);
	uint8_t v[50]={0};
	memset(v,0xff,8); v[8]=2;
	unsigned n=9;
	for (unsigned slot=0;slot<2;slot++) {
		uint8_t state=slot?second:first;v[n]=state;v[n+4]=state==2?3:0;
		v[n+5]=(slot && state==1)?1:0; n+=6;
		if (slot && state==1) {
			v[n]=2;v[n+1]=app_state;v[n+6]=5;
			for (unsigned i=0;i<5;i++) v[n+7+i]=(uint8_t)(0xad+i);
			v[n+13]=2;v[n+14]=3;v[n+15]=9;n+=19;
		}
	}
	tlv(&card,0x10,v,n);
	const uint8_t flags[]={2,1,valid?1:0};
	tlv(&card,0x12,flags,sizeof(flags));
}
static void physical(uint8_t first,uint8_t second,unsigned logical_b)
{
	response(&slots);
	uint8_t v[42]={2};unsigned n=1;
	for (unsigned i=0;i<2;i++) {
		v[n]=i?second:first;v[n+4]=1;v[n+8]=i?logical_b:1;v[n+9]=7;
		memset(v+n+10,0xae,7); n+=17;
	}
	tlv(&slots,0x10,v,n);
}
static void check(unsigned value)
{
	(void)uim_card_status(); assert((unsigned)cell_sim_state==value);
}
static bool option_sms_self, option_sms, option_service=true, option_bringup=true;
static unsigned option_data=1,option_data6=1,modem_generation;
static struct { unsigned bar; } pcie;
static unsigned provisions,online_requests,registration_reads,data4,data6,retirements,sms,pumps;
static char dcm_address[48];
static bool dcm_activate_pending,dcm_address_pending,dcm_resend_pending;
static uint32_t rd(uint32_t reg){ (void)reg;return 0; }
static void mission_pump(unsigned time,const void *cancel){ (void)cancel;pumps++;fake_now+=time*1000000ull; }
static void modem_diagnose(void){ }
static void data_bringup(void){data4++;}
static void data_bringup6(void){data6++;}
static void data_down(void){retirements++;}
static bool PE_parse_boot_argn(const char *s,void *p,size_t n){(void)s;(void)p;(void)n;return false;}
static int wms_send_text(const char *s,const char *b){(void)s;(void)b;sms++;return 0;}
static void tlv_put(uint8_t *data,uint32_t *used,unsigned tag,const void *p,unsigned n){
	data[(*used)++]=tag;data[(*used)++]=n;data[(*used)++]=0;memcpy(data+*used,p,n);*used+=n;
}
static int qmi_call(unsigned svc,unsigned msg,const uint8_t *buf,unsigned n,unsigned time){
	(void)time;
	if (svc==2 && msg==0x2d) return 0;
	if (svc==2 && msg==0x2e) { assert(n==4 && buf[3]==0);online_requests++;dms_mode=0;return 0; }
	if (svc==11 && msg==0x38) {
		assert(n==15 && buf[0]==1 && buf[3]==0 && buf[4]==1 && buf[5]==0x10 && buf[8]==2 && buf[9]==5);
		provisions++;return 0;
	}
	assert(0); /* In particular, no PIN/SMS request is permitted by the fixture. */
	return -1;
}
static int nas_registration(uint8_t *reg,uint8_t *radio){
	registration_reads++;*reg=cell_sim_state==CELL_SIM_READY;*radio=0;return 0;
}
#include "owner.inc"
static void late_card(void)
{
	cell_sim_invalidate();cell_started=false;cell_sim_retire_pending=false;
	logical(0,0,0,true);physical(1,1,2);modem_bringup(true);
	assert(cell_sim_state==CELL_SIM_ABSENT && !cell_started && !pumps && !provisions);
	fake_now+=600ull*NSEC_PER_SEC;dms_mode=3;option_sms_self=option_sms=true;
	logical(0,1,1,true);physical(1,2,2);notify(0x48,&slots,true);
	assert(!provisions && !cell_started && cell_sim_state==CELL_SIM_PRESENT);
	cell_sim_service(); assert(provisions==1 && online_requests==1 && !cell_started);
	cell_sim_service(); assert(provisions==1); /* Repeated busy-loop tick. */
	fake_now+=5ull*NSEC_PER_SEC;logical(0,1,7,true);
	card.bytes[7+3]=0;card.bytes[7+3+1]=1; /* primary GW names slot index 1, app 0 */
	cell_sim_service();
	assert(cell_started && cell.registered && data4==1 && data6==1 && !sms && !pumps);
	strcpy(cell.operator_name,"sample");strcpy(dcm_address,"fixture");cell.event_report_on=true;
	physical(1,1,2);logical(0,0,0,true);notify(0x48,&slots,true);
	assert(!cell_started && !cell.registered && !cell.operator_name[0] && !retirements && cell_sim_retire_pending);
	cell_sim_service();assert(retirements==1 && !dcm_address[0] && !cell.event_report_on && !cell_sim_provision_tries);
	fake_now+=5ull*NSEC_PER_SEC;logical(0,1,2,true);physical(1,2,2);
	notify(0x32,&card,true);cell_sim_service();assert(cell_sim_state==CELL_SIM_PIN && provisions==1);
	fake_now+=5ull*NSEC_PER_SEC;logical(0,1,1,true);notify(0x32,&card,true);
	for (unsigned i=0;i<6;i++) { cell_sim_service();fake_now+=5ull*NSEC_PER_SEC; }
	assert(provisions==4 && cell_sim_provision_tries==3 && !sms && !pumps);
	puts("PASS real mission owner: delayed insertion, ordinary ONLINE + primary GW activation, NAS registration, data restoration, removal retirement, per-insert cap, no PIN/SMS requests");
}
int main(void)
{
	logical(0,2,0,true);physical(1,1,2);check(CELL_SIM_ABSENT);
	assert(!sim.known && !sim.aid_len);
	physical(1,2,2);check(CELL_SIM_PRESENT); /* Logical NO_ATR is not physical absence. */
	logical(0,1,7,true);check(CELL_SIM_READY);
	assert(sim.known && sim.slot==2 && sim.app_type==2 && sim.aid_len==5);
	logical(0,1,2,true);check(CELL_SIM_PIN); logical(0,1,3,true);check(CELL_SIM_PUK);
	logical(0,1,1,true);check(CELL_SIM_PRESENT);
	logical(0,1,7,false);check(CELL_SIM_PRESENT);
	logical(0,1,7,true);physical(1,2,1);check(CELL_SIM_PRESENT);
	physical(1,2,2);check(CELL_SIM_READY);
	cell.registered=1;cell.bars=3;cell.dbm=-78;strcpy(cell.operator_name,"sample");
	physical(1,1,2);check(CELL_SIM_ABSENT);
	assert(!cell.registered && !cell.bars && !*cell.operator_name && !sim.aid_len && !sim.app_state);
	fail_slots=1;logical(0,0,0,true);check(CELL_SIM_UNKNOWN); /* No physical negative. */
	fail_slots=0;logical(0,1,7,true);physical(1,2,2);check(CELL_SIM_READY);
	interrupt_poll=true;check(CELL_SIM_UNKNOWN); /* Delayed reply after endpoint reset. */
	check(CELL_SIM_READY);
	have_endpoint=false;check(CELL_SIM_UNKNOWN);have_endpoint=true;
	fail_card=1;fail_slots=1;check(CELL_SIM_UNKNOWN);fail_card=fail_slots=0;
	response(&slots);uint8_t bad[]={2,1,0,0,0,0,0,0,0,1,200};tlv(&slots,0x10,bad,sizeof(bad));
	logical(0,0,0,true);check(CELL_SIM_UNKNOWN);
	struct cell_sim_card decoded={0};bool mapped=false;
	response(&card);uint8_t short_card[]={255,255,255,255,255,255,255,255,2,0,0,0,0,0,0};
	tlv(&card,0x10,short_card,sizeof(short_card));
	assert(cell_sim_decode(card.bytes,card.n,false,true,&decoded,0,&mapped)==CELL_SIM_UNKNOWN);
	physical(1,1,2);assert(cell_sim_decode(slots.bytes+7,slots.n-7,true,false,&decoded,0,&mapped)==CELL_SIM_ABSENT);
	assert(cell_sim_decode(slots.bytes,slots.n,true,false,&decoded,0,&mapped)==CELL_SIM_UNKNOWN);
	uint8_t zero=0; response(&slots);tlv(&slots,0x10,&zero,1);
	assert(cell_sim_decode(slots.bytes,slots.n,true,true,&decoded,0,&mapped)==CELL_SIM_UNKNOWN);
	physical(1,2,2);logical(0,1,7,true);check(CELL_SIM_READY);
	physical(1,1,2);notify(0x48,&slots,false); assert(cell_sim_state==CELL_SIM_READY);
	notify(0x48,&slots,true);assert(cell_sim_state==CELL_SIM_ABSENT && !sim.aid_len);
	/* #407: a stale logical READY followed confirmed physical removal by
	 * 2-4 ms. It must not resurrect the removed subscription. */
	notify(0x32,&card,true);assert(cell_sim_state==CELL_SIM_ABSENT && !sim.known);
	fail_slots=1;check(CELL_SIM_UNKNOWN);notify(0x32,&card,true);assert(cell_sim_state==CELL_SIM_UNKNOWN);
	fail_slots=0;
	physical(1,2,2);notify(0x48,&slots,true);assert(cell_sim_state==CELL_SIM_PRESENT);
	notify(0x32,&card,true);assert(cell_sim_state==CELL_SIM_READY && sim.slot==2);
	cell_sim_invalidate();assert(cell_sim_state==CELL_SIM_UNKNOWN && !sim.known);
	assert(registrations>=2);
	late_card();
	puts("PASS UIM unknown/physical absence/present/ready/PIN/PUK, second-slot match, invalid logical entry, stale radio/SIM cleanup, reset races, malformed status and identity-free physical indications");
}
