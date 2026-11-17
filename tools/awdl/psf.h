/* One canonical local discovery announcement. No arbitrary TLV input, remote
 * identity, service name, or caller-supplied raw frame is accepted. OWL's
 * normal PSF composition is documented by src/tx.c at 0ee3263f944ea721. */
#ifndef LX_AWDL_PSF_H
#define LX_AWDL_PSF_H
#include <stddef.h>
#include <stdint.h>
#define LX_AWDL_PSF_LENGTH 254u

static inline void psf_le16(uint8_t *p, unsigned v)
{
	p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}
static inline void psf_le32(uint8_t *p, uint32_t v)
{
	psf_le16(p,v); psf_le16(p+2,v>>16);
}
static inline void psf_addr(uint8_t *p, const uint8_t a[6])
{
	for (unsigned i=0;i<6;i++) p[i]=a[i];
}
static inline uint8_t *psf_tlv(uint8_t **at, unsigned type, unsigned length)
{
	uint8_t *p=*at;
	p[0]=(uint8_t)type;
	psf_le16(p+1,length);
	*at+=3+length;
	return p+3;
}
static inline void psf_sequence(uint8_t *p)
{
	/* Six 64 TU extended windows fit the 500 ms local ch6 reservation.
	 * The remaining slots are idle. No later window is promised. */
	p[0]=15; p[1]=3; p[3]=3; psf_le16(p+4,0xffff);
	for (unsigned i=0;i<6;i++) {p[6+i*2]=6; p[7+i*2]=81;}
}
static inline size_t
lx_awdl_local_psf(uint8_t *out,size_t capacity,const uint8_t self[6],
    uint64_t now_us,uint64_t foreign_us)
{
	if (!out || !self || capacity<LX_AWDL_PSF_LENGTH || !foreign_us ||
	    now_us<foreign_us || now_us-foreign_us>65536 || (self[0]&3)!=2) return 0;
	for (unsigned i=0;i<LX_AWDL_PSF_LENGTH;i++) out[i]=0;
	/* 802.11 action, broadcast destination, our assigned VDEV as sender. */
	static const uint8_t bssid[6]={0,0x25,0,0xff,0x94,0x73};
	out[0]=0xd0;
	for (unsigned i=0;i<6;i++) out[4+i]=0xff;
	psf_addr(out+10,self); psf_addr(out+16,bssid);
	uint8_t *a=out+24;
	a[0]=127; a[1]=0; a[2]=0x17; a[3]=0xf2; a[4]=8; a[5]=0x10;
	psf_le32(a+8,(uint32_t)now_us); psf_le32(a+12,(uint32_t)now_us);

	uint8_t *at=out+40;
	uint8_t *sync=psf_tlv(&at,4,73);
	const unsigned elapsed_tu=(unsigned)((now_us-foreign_us)/1024);
	const unsigned aw=elapsed_tu/16;
	sync[0]=6; psf_le16(sync+1,64-elapsed_tu%64); sync[3]=6;
	psf_le16(sync+5,16); psf_le16(sync+7,110); psf_le16(sync+9,0x1800);
	psf_le16(sync+11,16); psf_le16(sync+13,16);
	psf_le16(sync+15,elapsed_tu<16?16-elapsed_tu:0);
	for (unsigned i=17;i<=20;i++) sync[i]=3;
	psf_addr(sync+21,self); sync[27]=4;
	psf_le16(sync+29,aw); psf_le16(sync+31,aw);
	psf_sequence(sync+33);
	/* Initial independent clock, default low election metric, zero counters. */
	uint8_t *e=psf_tlv(&at,5,21);
	psf_addr(e+5,self); psf_le32(e+11,60); psf_le32(e+15,60);
	psf_sequence(psf_tlv(&at,18,40));
	uint8_t *v2=psf_tlv(&at,24,40);
	psf_addr(v2,self); psf_addr(v2+6,self);
	psf_le32(v2+20,60); psf_le32(v2+24,60);
	(void)psf_tlv(&at,6,9); /* empty service bloom */
	uint8_t *data=psf_tlv(&at,12,8);
	psf_le16(data,4); psf_addr(data+2,self); /* only the local address present */
	uint8_t *version=psf_tlv(&at,21,2);
	version[0]=0x34; version[1]=2; /* iOS class */
	return (size_t)(at-out);
}
#endif
