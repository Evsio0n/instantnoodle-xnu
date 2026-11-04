/* Transfer-only WLFW BDF ledger. Caller holds qrtr_lock for all mutation.
 * The source pointer is a pinned RAM-image file verified before hardware startup.
 * ACKs prove only acceptance of these requests, not calibration or RF operation. */
#ifndef HK_WIFI_BDF_TRANSFER_H
#define HK_WIFI_BDF_TRANSFER_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "hk_wifi_cap_gate.h"
#define HK_BDF_NATIVE_SIZE 57932u
#define HK_BDF_CHUNK_SIZE 6144u
struct hk_bdf_transfer {
 const uint8_t *data;
 uint32_t size, acked, segment, requests, acknowledgements;
 uint32_t from, node, port, pending_size;
 uint16_t transaction;
 bool verified, pending, complete, failed, finish_stop;
};
static void
wbd_init(struct hk_bdf_transfer *s, const uint8_t *data, uint32_t size, bool verified)
{
 memset(s, 0, sizeof(*s));s->data=data;s->size=size;
 s->verified=verified && data && size==HK_BDF_NATIVE_SIZE;
}
static bool
wbd_fail(struct hk_bdf_transfer *s)
{
 s->failed=true;return false;
}
/* Exactly seven fields from pinned ath11k's BDF request schema. total_size is
 * REMAINING bytes in each request, not the original whole-file length. */
static bool
wbd_begin(struct hk_bdf_transfer *s, uint32_t from, uint32_t node, uint32_t port,
 const uint8_t *b, size_t n, bool eligible)
{
 if(!eligible || !s->verified || s->failed || s->pending || s->complete ||
    s->acked>=s->size || !wc_header(b,n,0) || wc_u16(b+3)!=0x25)return wbd_fail(s);
 uint32_t valid=0,file=0,total=0,segment=0,end=0,type=0,mask=0,data_len=0;
 const uint8_t *data=NULL;
 for(size_t at=7;at<n;){
  if(n-at<3)return wbd_fail(s);
  unsigned t=b[at];size_t len=wc_u16(b+at+1);at+=3;
  if(len>n-at)return wbd_fail(s);const uint8_t *v=b+at;unsigned bit;
  switch(t){
   case 1:bit=1;if(len!=1)return wbd_fail(s);valid=v[0];break;
   case 0x10:bit=2;if(len!=4)return wbd_fail(s);file=wc_u32(v);break;
   case 0x11:bit=4;if(len!=4)return wbd_fail(s);total=wc_u32(v);break;
   case 0x12:bit=8;if(len!=4)return wbd_fail(s);segment=wc_u32(v);break;
   case 0x13:bit=16;if(len<2)return wbd_fail(s);data_len=wc_u16(v);if(len!=2u+data_len)return wbd_fail(s);data=v+2;break;
   case 0x14:bit=32;if(len!=1)return wbd_fail(s);end=v[0];break;
   case 0x15:bit=64;if(len!=1)return wbd_fail(s);type=v[0];break;
   default:return wbd_fail(s);
  }
  if(mask&bit)return wbd_fail(s);mask|=bit;at+=len;
 }
 uint32_t remaining=s->size-s->acked;
 uint32_t chunk=remaining<HK_BDF_CHUNK_SIZE?remaining:HK_BDF_CHUNK_SIZE;
 if(mask!=127 || valid!=1 || file!=0xff || type!=1 || total!=remaining ||
    segment!=s->segment || data_len!=chunk || end!=(remaining==chunk) ||
    memcmp(data,s->data+s->acked,chunk))return wbd_fail(s);
 s->from=from;s->node=node;s->port=port;s->transaction=wc_u16(b+1);
 s->pending_size=chunk;s->pending=true;s->requests++;return true;
}
/* Return true only when this call accepts a new matching successful ACK.
 * Unrelated/duplicate replies do not alter the ledger. A matching bad ACK latches
 * failure; it is still delivered unmodified to the ordinary QMI transaction. */
static bool
wbd_ack(struct hk_bdf_transfer *s,uint32_t from,uint32_t node,uint32_t port,
 const uint8_t *b,size_t n)
{
 if(!s->pending || s->failed || n<7 || from!=s->from || node!=s->node || port!=s->port ||
    b[0]!=2 || wc_u16(b+1)!=s->transaction || wc_u16(b+3)!=0x25)return false;
 if(!wc_header(b,n,2))return wbd_fail(s);
 bool result=false;
 for(size_t at=7;at<n;){
  if(n-at<3)return wbd_fail(s);unsigned t=b[at];size_t len=wc_u16(b+at+1);at+=3;
  if(len>n-at)return wbd_fail(s);
  if(t==2){if(result || len!=4 || wc_u16(b+at)!=0 || wc_u16(b+at+2)!=0)return wbd_fail(s);result=true;}
  at+=len;
 }
 if(!result || s->acked>s->size || !s->pending_size || s->pending_size>s->size-s->acked)return wbd_fail(s);
 s->acked+=s->pending_size;s->segment++;s->acknowledgements++;s->pending=false;
 s->complete=s->acked==s->size;return true;
}
/* After complete transfer, a deliberate refusal of the next M3_INFO request
 * stops pinned ath11k through its ordinary QMI_FAIL path. Never withhold/fake ACK.
 * OFF remains allowed for cleanup, including after a transfer error. */
static bool
wbd_boot_request(struct hk_bdf_transfer *s,const uint8_t *b,size_t n)
{
 bool framed=wc_header(b,n,0);uint16_t id=framed?wc_u16(b+3):0xffff;
 if(id==0x22 && wc_tx_allowed(b,n))return true;
 if(!s->verified)return wbd_fail(s);
 if(s->failed)return false;
 if(s->complete){
  if(id==0x3c){s->finish_stop=true;return false;}
  return wbd_fail(s);
 }
 if(id==0x3c)return wbd_fail(s); /* premature M3 is not a BDF-only sequence */
 if(id==0x24 && (s->acked || s->pending))return wbd_fail(s);
 return wc_tx_allowed(b,n);
}
static bool
wbd_done(const struct hk_bdf_transfer *s)
{
 return s->verified && !s->failed && !s->pending && s->complete && s->finish_stop &&
  s->size==HK_BDF_NATIVE_SIZE && s->acked==s->size && s->requests==10 &&
  s->acknowledgements==10 && s->segment==10;
}
#endif
