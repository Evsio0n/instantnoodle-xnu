"""Run the native client's read-only ND gate against deterministic socket/ioctl outcomes."""
from pathlib import Path
import subprocess
import tempfile

source=Path(__file__).with_name('native_client.c').read_text()
start=source.index('static int wait_for_neighbor(')
body=source[start:source.index('\n}\n',start)+3]
stub=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <netinet/in.h>
struct neighbor_info {char name[16]; struct in6_addr addr; int64_t asked;int32_t isrouter,state,expire;};
#define GET_NEIGHBOR 0xc038694eUL
static uint64_t tick;
static unsigned scenario,loops,index_now;
static bool stopping;
static uint64_t now_ms(void){return tick;}
static int open_dummy(int a,int b,int c){assert(a==AF_INET6&&b==SOCK_DGRAM&&c==0);return 7;}
static int close_dummy(int fd){assert(fd==7);return 0;}
static int sleep_dummy(unsigned us){tick+=us/1000;return 0;}
static unsigned index_dummy(const char *name){assert(!strcmp(name,"awdl0"));return index_now;}
static int read_neighbor(int fd,unsigned long cmd,struct neighbor_info *record){
    assert(fd==7 && cmd==GET_NEIGHBOR && !strcmp(record->name,"awdl0"));
    ++loops;
    if(scenario==1 && loops<3){errno=EINVAL;return -1;}
    if(scenario==2){errno=EACCES;return -1;}
    if(scenario==3){record->state=0;return 0;}
    if(scenario==4){index_now=19;record->state=1;return 0;}
    record->state=2;return 0;
}
#define socket open_dummy
#define close close_dummy
#define ioctl read_neighbor
#define if_nametoindex index_dummy
#define usleep sleep_dummy
'''
test=r'''
int main(void){
    struct in6_addr peer={0};
    index_now=9;
    assert(!wait_for_neighbor("awdl0",9,&peer) && loops==1);
    scenario=1;loops=0;assert(!wait_for_neighbor("awdl0",9,&peer) && loops==3);
    scenario=2;loops=0;assert(wait_for_neighbor("awdl0",9,&peer)==EACCES && loops==1);
    scenario=3;loops=0;assert(wait_for_neighbor("awdl0",9,&peer)==ETIMEDOUT && loops==80);
    scenario=4;loops=0;assert(wait_for_neighbor("awdl0",9,&peer)==ENXIO && loops==1);
    stopping=true;index_now=9;loops=0;assert(wait_for_neighbor("awdl0",9,&peer)==EINTR && !loops);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    folder=Path(tmp);file=folder/'gate.c';file.write_text(stub+body+test)
    subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(file),'-o',str(folder/'test')],check=True)
    subprocess.run([str(folder/'test')],check=True)
