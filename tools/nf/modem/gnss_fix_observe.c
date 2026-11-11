/* Only read the coordinate-free quality ABI. One fixed record for each sample. */
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <unistd.h>
static int digest_file(const char *path, char out[65])
{
    uint8_t buf[8192],digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX ctx;FILE *f=fopen(path,"rb");size_t used=0,n;
    if(!f)return 0;
    CC_SHA256_Init(&ctx);
    while((n=fread(buf,1,sizeof(buf),f))!=0){
        used+=n;if(used>2*1024*1024){fclose(f);return 0;}
        CC_SHA256_Update(&ctx,buf,(CC_LONG)n);
    }
    int ok=!ferror(f)&&used>0;fclose(f);
    if(!ok)return 0;
    CC_SHA256_Final(digest,&ctx);
    for(unsigned i=0;i<CC_SHA256_DIGEST_LENGTH;i++)snprintf(out+2*i,3,"%02x",digest[i]);
    return 1;
}
int main(void)
{
    if(fork()!=0)return 0;
    int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
    if(fd<0)return 1;
    char executable[65],plist[65];
    if(digest_file("/Applications/GNSSMonitor.app/GNSSMonitor",executable)&&digest_file("/Applications/GNSSMonitor.app/Info.plist",plist))
        dprintf(fd,"gnss-bundle-sha executable=%s plist=%s end\n",executable,plist);
    for(unsigned i=0;i<48;i++){
        struct {uint32_t version,state,accepted,accuracy_bits;uint64_t age_ms;} v={0};
        size_t n=sizeof(v);
        int rc=sysctlbyname("kern.hackios.modem_fix_quality",&v,&n,NULL,0);
        if(rc||v.version!=1||n!=sizeof(v))dprintf(fd,"gnss-fix-quality seq=%u unavailable=%d end\n",i,rc?errno:0);
        else dprintf(fd,"gnss-fix-quality seq=%u state=%u valid_count=%u age_ms=%llu end\n",i,v.state,v.accepted,v.age_ms);
        sleep(5);
    }
    close(fd);return 0;
}
