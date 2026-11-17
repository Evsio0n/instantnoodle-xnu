/* One read-only 20F66 en0 CARD_CAPABILITIES request. */
#include <errno.h>
#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

struct hk_apple80211req {
    char name[IFNAMSIZ];
    int32_t type;
    int32_t value;
    uint32_t length;
    void *payload;
};
_Static_assert(sizeof(struct hk_apple80211req)==40,"unexpected iOS ioctl ABI");
_Static_assert(offsetof(struct hk_apple80211req,payload)==32,"unexpected payload offset");
_Static_assert(_IOWR('i',201,struct hk_apple80211req)==0xc02869c9UL,"unexpected get selector");

int main(void) {
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if (fd<0) {
        printf("airdrop-cap: interface=en0 get=12 socket_errno=%d awdl=unknown\n",errno);
        return 1;
    }
    uint8_t caps[13]={0};
    struct hk_apple80211req request={.name="en0",.type=12,.length=sizeof(caps),.payload=caps};
    int rc=ioctl(fd,_IOWR('i',201,struct hk_apple80211req),&request);
    int error=rc<0?errno:0;
    close(fd);
    if (rc<0 || request.length!=sizeof(caps)) {
        printf("airdrop-cap: interface=en0 get=12 requested=13 rc=%d errno=%d returned=%u awdl=unknown\n",
               rc,error,request.length);
        return 1;
    }
    printf("airdrop-cap: interface=en0 get=12 requested=13 rc=0 errno=0 returned=13 byte4=0x%02x bit32=%u\n",
           caps[4],(unsigned)(caps[4]&1u));
    return 0;
}
