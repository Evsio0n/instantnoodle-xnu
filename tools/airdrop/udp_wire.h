#ifndef HK_AIRDROP_UDP_WIRE_H
#define HK_AIRDROP_UDP_WIRE_H
#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <string.h>

enum { HK_UDP_PORT=61743, HK_UDP_REQUEST=1, HK_UDP_REPLY=2, HK_UDP_TOKEN_SIZE=16 };
struct hk_udp_message { uint8_t prefix[16],version,kind,token[HK_UDP_TOKEN_SIZE]; };
_Static_assert(sizeof(struct hk_udp_message)==34,"UDP test message is exactly 34 bytes");
_Static_assert(sizeof("HKAWDL-UDP-TEST1")-1==16,"test prefix length");

static inline void hk_udp_make(struct hk_udp_message *m,unsigned kind,const uint8_t token[HK_UDP_TOKEN_SIZE]) {
    memcpy(m->prefix,"HKAWDL-UDP-TEST1",sizeof(m->prefix));
    m->version=1;
    m->kind=(uint8_t)kind;
    memcpy(m->token,token,sizeof(m->token));
}
static inline int hk_udp_matches(const struct hk_udp_message *data,unsigned kind,const uint8_t token[HK_UDP_TOKEN_SIZE]) {
    struct hk_udp_message expected;
    hk_udp_make(&expected,kind,token);
    const uint8_t *a=(const uint8_t *)data,*b=(const uint8_t *)&expected;
    uint8_t mismatch=0;
    for (size_t i=0;i<sizeof(expected);i++) mismatch|=a[i]^b[i];
    return mismatch==0;
}
static inline void hk_udp_digest(const struct hk_udp_message *m,char hex[65]) {
    static const char digits[]="0123456789abcdef";
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(m,(CC_LONG)sizeof(*m),d);
    for (size_t i=0;i<sizeof(d);i++) { hex[i*2]=digits[d[i]>>4]; hex[i*2+1]=digits[d[i]&15]; }
    hex[64]='\0';
}
#endif
