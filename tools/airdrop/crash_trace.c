/* RAM-only sharingd fault recorder. Native identity/protocol untouched.
 * Preserve the actual signal context before normal fatal delivery. */
#include <signal.h>
#include <sys/ucontext.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
static int record_fd=-1;
static struct sigaction previous_segv,previous_abrt;
static volatile sig_atomic_t entered;
static unsigned append_hex(char *p,unsigned at,const char *key,uint64_t value){
 while(*key)p[at++]=*key++;
 p[at++]='0';p[at++]='x';
 for(int i=15;i>=0;i--)p[at++]="0123456789abcdef"[(value>>(i*4))&15];
 p[at++]=' ';return at;
}
static void fault(int sig,siginfo_t *info,void *context){
 if(entered)_exit(128+sig);entered=1;
 char b[256];unsigned n=0;const char *head="native-airdrop-fault ";while(*head)b[n++]=*head++;
 n=append_hex(b,n,"signal=",sig);n=append_hex(b,n,"address=",(uintptr_t)(info?info->si_addr:0));
 ucontext_t *u=context;
 if(u && u->uc_mcontext){
  arm_thread_state64_t s=u->uc_mcontext->__ss;
  n=append_hex(b,n,"pc=",arm_thread_state64_get_pc(s));n=append_hex(b,n,"lr=",arm_thread_state64_get_lr(s));
  n=append_hex(b,n,"sp=",arm_thread_state64_get_sp(s));n=append_hex(b,n,"fp=",arm_thread_state64_get_fp(s));
 }
 b[n++]='\n';if(record_fd>=0)write(record_fd,b,n);write(STDERR_FILENO,b,n);
 sigaction(sig,sig==SIGSEGV?&previous_segv:&previous_abrt,0);
 /* Return to the original fault with the original fatal disposition. */
}
static int matches(const struct mach_header_64 *h){
 static const unsigned char uuid[16]={46,79,175,64,30,196,56,230,172,131,151,10,167,26,27,144};
 if(!h || h->magic!=MH_MAGIC_64)return 0;
 const struct load_command *c=(const void *)(h+1);
 for(unsigned i=0;i<h->ncmds;i++){
  if(c->cmd==LC_UUID && c->cmdsize>=sizeof(struct uuid_command))return !memcmp(((const struct uuid_command *)c)->uuid,uuid,16);
  c=(const void *)((const char *)c+c->cmdsize);
 }return 0;
}
static void install(void){
 const char *enabled=getenv("HK_AIRDROP_FAULT_TRACE");if(!enabled || strcmp(enabled,"1"))return;
 int valid=0;for(unsigned i=0;i<_dyld_image_count();i++)if(matches((const void *)_dyld_get_image_header(i)))valid=1;
 if(!valid)return;
 char path[]="/private/var/tmp/airdrop-fault-XXXXXX";record_fd=mkstemp(path);if(record_fd<0)return;
 fcntl(record_fd,F_SETFD,FD_CLOEXEC);
 dprintf(record_fd,"pid=%d path=%s\n",getpid(),path);
 for(unsigned i=0;i<_dyld_image_count();i++)dprintf(record_fd,"image %p slide=%llx %s\n",_dyld_get_image_header(i),(unsigned long long)_dyld_get_image_vmaddr_slide(i),_dyld_get_image_name(i));
 struct sigaction action={0};action.sa_sigaction=fault;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
 sigaction(SIGSEGV,0,&previous_segv);sigaction(SIGABRT,0,&previous_abrt);
 if(previous_segv.sa_handler==SIG_DFL)sigaction(SIGSEGV,&action,0);
 if(previous_abrt.sa_handler==SIG_DFL)sigaction(SIGABRT,&action,0);
 dprintf(record_fd,"recorder installed; original fatal dispositions retained\n");
 dprintf(STDERR_FILENO,"airdrop-fault-recorder: %s\n",path);
}
__attribute__((constructor)) static void start(void){dispatch_async(dispatch_get_main_queue(),^{install();});}
