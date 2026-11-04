/* QCA6390 capability-only diagnostic wire policy. No hardware or allocations.
 * WLFW IDs/layout checked against upstream ath11k qmi.h/qmi.c (v6.12); the
 * compiled port's generated decoder remains responsible for its normal reply. */
#ifndef HK_WIFI_CAP_GATE_H
#define HK_WIFI_CAP_GATE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct hk_wifi_cap { uint32_t chip,family,board,soc,firmware,fields; };
static uint16_t wc_u16(const uint8_t *b){return (uint16_t)(b[0]|b[1]<<8);}
static uint32_t wc_u32(const uint8_t *b){return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static bool wc_header(const uint8_t *b,size_t n,uint8_t type){return n>=7 && b[0]==type && wc_u16(b+5)==n-7;}
/* Default deny. Only boot/host-memory/capability requests and explicit WLAN OFF
 * may leave. BDF(25), CFG(23), all non-OFF WLAN modes and unknown IDs are denied. */
static bool wc_tx_allowed(const uint8_t *b,size_t n){
 if(!wc_header(b,n,0))return false;
 uint16_t id=wc_u16(b+3);
 if(id==0x24 || id==0x4c)return n==7;
 if(id==0x20 || id==0x34 || id==0x36 || id==0x3c)return true;
 if(id!=0x22)return false;
 bool mode=false;
 for(size_t at=7;at<n;){if(n-at<3)return false;unsigned t=b[at];size_t len=wc_u16(b+at+1);at+=3;
  if(len>n-at)return false;
  if(t==1){if(mode || len!=4 || wc_u32(b+at)!=4)return false;mode=true;}
  at+=len;
 }
 return mode;
}
/* Require an exact matching requested transaction/peer in caller, then decode
 * only nonprivate hardware IDs. Optional fields remain explicitly absent. */
static bool wc_cap_decode(const uint8_t *b,size_t n,struct hk_wifi_cap *out){
 if(!wc_header(b,n,2) || wc_u16(b+3)!=0x24)return false;
 struct hk_wifi_cap c={0};bool result=false;
 for(size_t at=7;at<n;){if(n-at<3)return false;unsigned t=b[at];size_t len=wc_u16(b+at+1);at+=3;
  if(len>n-at)return false;const uint8_t *v=b+at;
  if(t==2){if(result || len!=4 || wc_u16(v)!=0 || wc_u16(v+2)!=0)return false;result=true;}
  else if(t==0x10){if((c.fields&1)||len!=8)return false;c.chip=wc_u32(v);c.family=wc_u32(v+4);c.fields|=1;}
  else if(t==0x11){if((c.fields&2)||len!=4)return false;c.board=wc_u32(v);c.fields|=2;}
  else if(t==0x12){if((c.fields&4)||len!=4)return false;c.soc=wc_u32(v);c.fields|=4;}
  else if(t==0x13){if((c.fields&8)||len<4)return false;c.firmware=wc_u32(v);c.fields|=8;}
  at+=len;
 }
 if(!result)return false;*out=c;return true;
}
#endif
