#ifndef LX_AWDL_NATIVE_FLIGHT_H
#define LX_AWDL_NATIVE_FLIGHT_H
#include <stddef.h>
#include <stdint.h>
enum live_proto { LP_OTHER,LP_MLD,LP_ND,LP_ECHO,LP_MDNS };
static inline const char *live_proto_name(unsigned p)
{
	static const char *const names[]={"other","mld","nd","echo","mdns"};
	return p<sizeof(names)/sizeof(names[0])?names[p]:"other";
}
/* Classify only the native network header, not the user data. MLD normally
 * carries an IPv6 Hop-by-Hop router-alert option. */
static inline unsigned live_proto_of(const uint8_t *ip,size_t n)
{
	if (!ip || n<40 || (ip[0]>>4)!=6 || ((((size_t)ip[4]<<8)|ip[5])!=n-40)) return LP_OTHER;
	unsigned hint=ip[6];size_t off=40;
	for (unsigned i=0;i<5;i++) {
		if (hint==58) {
			if(n-off<4)return LP_OTHER;
			unsigned t=ip[off];
			if(t==130||t==131||t==132||t==143)return LP_MLD;
			if(t>=133&&t<=137)return LP_ND;
			return t==128||t==129?LP_ECHO:LP_OTHER;
		}
		if(hint==17) {
			if(n-off<8)return LP_OTHER;
			unsigned a=(unsigned)ip[off]*256+ip[off+1],b=(unsigned)ip[off+2]*256+ip[off+3];
			return a==5353||b==5353?LP_MDNS:LP_OTHER;
		}
		if ((hint!=0&&hint!=43&&hint!=60) || n-off<2) return LP_OTHER;
		size_t step=((size_t)ip[off+1]+1u)*8u;
		if(step>n-off)return LP_OTHER;
		hint=ip[off];off+=step;
	}
	return LP_OTHER;
}
/* OWL's 16-slot schedule, 64 TU per extended window and 16 TU guard. */
static inline int live_multicast_due(uint64_t anchor,uint64_t now)
{
	if (!anchor || now<anchor) return 0;
	uint64_t delta=now-anchor;
	unsigned slot=(unsigned)((delta/65536u)%16u),phase=(unsigned)(delta%65536u);
	return (slot==0 || slot==10) && phase>=16384u && phase<=49152u;
}
#endif
