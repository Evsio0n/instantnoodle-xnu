/* Exercise the production on-phone one-shot state machine with a simulated loader USB mailbox. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
typedef int wait_result_t;
typedef int thread_t;
struct request {void *newptr,*oldptr;size_t newlen;};
static unsigned modem_level,workers;static uint64_t now,usb;
static unsigned ready,done;
static int plug=1;
static int hk_spmi_read(uint16_t id,uint8_t reg,uint8_t *bytes,uint32_t count) {
    assert(id==0x213 && reg==0x10 && count==1);
    if(plug<0)return -1;
    *bytes=plug?0x10:0;
    return 0;
}
#define LCK_MTX_DECLARE(name,group) int name
#define lck_mtx_lock(ptr) ((void)(ptr))
#define lck_mtx_unlock(ptr) ((void)(ptr))
#define SYSCTL_HANDLER_ARGS (void *unused __attribute__((unused)),struct request *req)
#define SYSCTL_IN(req,to,n) (memcpy(to,(req)->newptr,n),0)
#define SYSCTL_OUT(req,from,n) (memcpy((req)->oldptr,from,n),0)
#define SYSCTL_DECL(...)
#define SYSCTL_PROC(...)
#define KERN_SUCCESS 0
static int kernel_thread_start(void (*fn)(void*,int),void *ctx,thread_t *out) {(void)fn;(void)ctx;*out=1;workers++;return 0;}
static void thread_deallocate(thread_t t){(void)t;}
static uint64_t mdm_fix_now(void){return now;}
uint64_t portable_usb_state(void){return usb;}
static void say(const char *fmt,...){if(strstr(fmt,"ready-v1"))ready++;if(strstr(fmt,"rehearsal-v1"))done++;}
#include "portable_kernel.inc"
static void modem_thread(void *p,int r){(void)p;(void)r;}
static int act(int action) {struct request r={.newptr=&action,.newlen=sizeof(action)};return portable_control(NULL,&r);}
int main(void)
{
    unsigned summary[7]={0};struct request r={.oldptr=summary};assert(!portable_summary(NULL,&r));assert(summary[0]==1);
    modem_level=0;portable_latest_start=100;portable_state=P_WAIT_PAGE;usb=3;
    assert(act(2)!=0&&!workers);assert(!act(1)&&ready==1);assert(act(2)==EAGAIN&&!done);
    unsigned state=0;struct request query={.oldptr=&state};
    assert(!portable_control(NULL,&query)&&state==P_READY);
    plug=-1;assert(!portable_control(NULL,&query)&&state==7);assert(act(2)==ENXIO);
    plug=0;assert(!portable_control(NULL,&query)&&state==6);
    /* Physical cable absent with a stale transport edge is the #253 regression. */
    assert(!act(2)&&done==1&&!workers);assert(act(2)!=0);
    modem_level=3;portable_state=P_WAIT_PAGE;usb=4;assert(act(1)!=0);
    usb=5;plug=1;assert(!act(1));assert(act(2)==EAGAIN);
    usb=6;assert(act(2)==EAGAIN);plug=0;now=100;assert(act(2)==ETIMEDOUT&&!workers);
    now=50;assert(!act(2)&&workers==1);assert(act(2)!=0&&workers==1);assert(!act(3)&&portable_stop);
}
