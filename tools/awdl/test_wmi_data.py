"""Compile the actual staged WMI writer with a fake command transport.

Checks command/tag choice, DMA reference, byte payload alignment, explicit
raw PHY/data parameters, feature rejection and the deferred-grant failure.
No firmware, network access or privileged operation in the test executable.
"""
import json,subprocess,argparse
from pathlib import Path
from build_roc import read,OUT
p=argparse.ArgumentParser();p.add_argument('--receipt',default='wmi-data-v2-build.json');p.add_argument('--amsdu',action='store_true');args=p.parse_args()
r=json.loads((OUT/args.receipt).read_text())
s=read(r['stage']+'/xnu-deps/build/ath11k/src/ath11k/wmi.c')
a=s.index('int ath11k_wmi_mgmt_send(');b=s.index('{',a);d=1;e=b+1
while d:
    if s[e]=='{':d+=1
    if s[e]=='}':d-=1
    e+=1
fn=s[a:e]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define WMI_MGMT_SEND_DOWNLD_LEN 64
#define WMI_TLV_TAG 0xffff0000u
#define WMI_TLV_LEN 0xffffu
#define TLV_HDR_SIZE 4
#define WMI_TAG_MGMT_TX_SEND_CMD 100
#define WMI_TAG_OFFCHAN_DATA_TX_SEND_CMD 101
#define WMI_TAG_ARRAY_BYTE 17
#define WMI_TAG_TX_SEND_PARAMS 102
#define WMI_MGMT_TX_SEND_CMDID 10
#define WMI_OFFCHAN_DATA_TX_SEND_CMDID 11
#define WMI_TLV_SERVICE_OFFCHAN_TX_WMI 123
#define WMI_TLV_SERVICE_OFFCHAN_DATA_TID_SUPPORT 144
#define WMI_TX_PARAMS_DWORD1_CFR_CAPTURE (1u<<21)
#define ATH11K_DBG_WMI 0
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m))&(m))
#define roundup(v,a) (((v)+(a)-1)&~((a)-1))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((v)>>32))
#define test_bit(n,a) (((a)[(n)/64]>>((n)%64))&1)
#define ath11k_info(...) ((void)0)
#define ath11k_warn(...) ((void)0)
#define ath11k_dbg(...) ((void)0)
#define lx_tell(...) ((void)0)
struct ieee80211_tx_info {int unused;};
struct skb_cb {uint64_t paddr;};
struct sk_buff {unsigned char *data;size_t len;struct ieee80211_tx_info info;struct skb_cb cb;};
#define IEEE80211_SKB_CB(s) (&(s)->info)
#define ATH11K_SKB_CB(s) (&(s)->cb)
struct wmi_ab {uint64_t svc_map[4];};
struct ath11k_base {struct wmi_ab wmi_ab;};
struct ath11k_pdev_wmi {struct wmi_ab *wmi_ab;};
struct ath11k {void *hw;struct ath11k_base *ab;struct ath11k_pdev_wmi *wmi;};
struct wmi_mgmt_send_cmd {u32 tlv_header,vdev_id,desc_id,chanfreq,paddr_lo,paddr_hi,frame_len,buf_len,tx_params_valid;};
struct wmi_mgmt_send_params {u32 tlv_header,tx_params_dword0,tx_params_dword1;};
struct wmi_tlv {u32 header;unsigned char value[];};
static bool owned;
static int grant=1,allocs,posted,posted_id,mgmt_submit;
static struct sk_buff *last;
static int lx_awdl_wmi_data_owned(void *h,struct sk_buff *s){(void)h;(void)s;return owned;}
static u32 current_frequency=5745;
static u32 lx_awdl_wmi_data_frequency(void *h){(void)h;return current_frequency;}
static u32 ath11k_wmi_mgmt_get_freq(struct ath11k *a,struct ieee80211_tx_info *i){(void)a;(void)i;return 0;}
static struct sk_buff *ath11k_wmi_alloc_skb(struct wmi_ab *a,int n){(void)a;allocs++;struct sk_buff *s=calloc(1,sizeof(*s));s->data=calloc(1,n);s->len=n;return s;}
static int ath11k_wmi_cmd_send(struct ath11k_pdev_wmi *w,struct sk_buff *s,int id){(void)w;posted++;posted_id=id;last=s;return 0;}
static void ath11k_ce_byte_swap(void *p,int n){(void)p;(void)n;}
static int lx_awdl_psf_prepare(void *h,struct sk_buff *f,u32 v,u32 b,u32 c){(void)h;(void)f;assert(v==3&&b==9&&c==(owned?5745u:0u));return grant;}
static void lx_awdl_psf_submit_result(void *h,u32 b,int r){(void)h;(void)b;(void)r;mgmt_submit++;}
static void dev_kfree_skb(struct sk_buff *s){free(s->data);free(s);allocs--;}
'''
post=r'''
static void reset(void){if(last){dev_kfree_skb(last);last=NULL;}posted=0;posted_id=0;mgmt_submit=0;grant=1;assert(allocs==0);}
int main(void){
 _Static_assert(sizeof(struct wmi_mgmt_send_cmd)==36,"Qualcomm fixed header");
 _Static_assert(sizeof(struct wmi_mgmt_send_params)==12,"Qualcomm raw parameters");
 struct ath11k_base ab={0};struct ath11k_pdev_wmi w={.wmi_ab=&ab.wmi_ab};struct ath11k ar={.ab=&ab,.wmi=&w};
 unsigned char bytes[99];for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(unsigned char)i;
 struct sk_buff f={.data=bytes,.len=sizeof(bytes),.cb={.paddr=0x123456789ull}};
 owned=false;assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==0&&posted_id==10&&mgmt_submit==1);
 struct wmi_mgmt_send_cmd *c=(void *)last->data;
 assert(c->tlv_header==(100u<<16|32u)&&c->paddr_lo==0x23456789u&&c->paddr_hi==1&&c->frame_len==99&&c->buf_len==64&&last->len==104);
 assert(!memcmp(last->data+40,bytes,64));reset();
 assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,true)==0&&posted_id==10&&last->len==116);
 struct wmi_mgmt_send_params *p=(void *)(last->data+104);assert(p->tx_params_dword1==(1u<<21));reset();
 owned=true;assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==-EOPNOTSUPP&&posted==0&&allocs==0);
 ab.wmi_ab.svc_map[123/64]|=1ull<<(123%64);
 assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==0&&posted_id==11&&mgmt_submit==0&&last->len==116);
 c=(void *)last->data;assert(c->tlv_header==(101u<<16|32u)&&c->tx_params_valid==0&&c->chanfreq==5745);p=(void *)(last->data+104);assert(p->tx_params_dword1==(1u<<20));reset();
 ab.wmi_ab.svc_map[144/64]|=1ull<<(144%64);
 assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==0&&posted_id==11&&last->len==116);
 c=(void *)last->data;p=(void *)(last->data+104);assert(c->tx_params_valid==1&&p->tlv_header==(102u<<16|8u));
 assert((p->tx_params_dword0&255)==0);assert(((p->tx_params_dword0>>8)&4095)==16);assert(((p->tx_params_dword0>>20)&255)==1);assert((p->tx_params_dword0>>28)==4);
 assert(((p->tx_params_dword1>>8)&127)==4&&((p->tx_params_dword1>>15)&31)==1);assert((p->tx_params_dword1&(1u<<20))&&!(p->tx_params_dword1&(1u<<21)));reset();
 current_frequency=0;assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==-ECANCELED&&posted==0&&allocs==0);current_frequency=5745;
 grant=-ECANCELED;assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==-ECANCELED&&posted==0&&allocs==0);
 puts("PASS actual WMI writer: legacy/CFR preserved; feature gate, data tag, DMA reference, frame bytes, nonpause PHY fields and stale-grant rejection");
}
'''
if args.amsdu:
    post=post.replace(' current_frequency=0;', ''' f.data[0]=0x88;f.data[24]=0x80;
 assert(ath11k_wmi_mgmt_send(&ar,3,9,&f,false)==0&&last->len==116);
 p=(void *)(last->data+104);assert(((p->tx_params_dword0>>8)&4095)==1&&((p->tx_params_dword1>>15)&31)==4);reset();
 f.data[0]=0;f.data[24]=24;
 current_frequency=0;''')
p=OUT/'test-wmi-data-writer.c';p.write_text(pre+fn+post)
exe=OUT/'test-wmi-data-writer'
subprocess.run(['clang','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
