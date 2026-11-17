/* OnePlus IN2010 RAM diagnostic. Success publishes an opaque file in RAM only.
 * This executable takes no path or data arguments. */
#include "cali_ext4.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define CALI_DK_BLOCKSIZE _IOR('d', 24, uint32_t)
#define CALI_DK_BLOCKCOUNT _IOR('d', 25, uint64_t)
#define CALI_DK_PHYSICAL_BLOCKSIZE _IOR('d', 77, uint32_t)
#define CALI_BYTES (UINT64_C(32)*1024*1024)
static int device_read(void *source,uint32_t block,uint8_t *out){
    int fd=*(int *)source;
    ssize_t n;
    do n=pread(fd,out,CE_BS,(off_t)block*CE_BS);while(n<0&&errno==EINTR);
    return n==CE_BS;
}
static bool exact(const char *args,const char *name,const char *value){
    size_t n=strlen(name),z=strlen(value);unsigned seen=0;
    for(const char *p=args;*p;){
        while(*p==' ')p++;
        const char *q=p;while(*q&&*q!=' ')q++;
        if((size_t)(q-p)>n && !memcmp(p,name,n)&&p[n]=='='){
            if((size_t)(q-p)!=n+1+z||memcmp(p+n+1,value,z))return false;
            seen++;
        }
        p=q;
    }
    return seen==1;
}
static bool ram_scope(void){
    char args[1024]={0};size_t size=sizeof(args);
    return geteuid()==0 && sysctlbyname("kern.bootargs",args,&size,NULL,0)==0 &&
        size>0 && size<=sizeof(args) && memchr(args,0,size) &&
        exact(args,"board","instantnoodle") && exact(args,"hk_data","2") &&
        exact(args,"ufs_write","0") && exact(args,"var_store","0") && exact(args,"var_data","hfs");
}
static bool private_dir(int fd){
    struct stat st;
    return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==0 && !(st.st_mode&022);
}
static bool write_private(const uint8_t *data,unsigned size){
    int root=open("/private/var/root",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(!private_dir(root)){if(root>=0)close(root);return false;}
    if(mkdirat(root,"sensor-calibration",0700)<0&&errno!=EEXIST){close(root);return false;}
    int dir=openat(root,"sensor-calibration",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);close(root);
    struct stat st;
    if(!private_dir(dir)||fstat(dir,&st)!=0||(st.st_mode&0777)!=0700){if(dir>=0)close(dir);return false;}
    char tmp[64];snprintf(tmp,sizeof(tmp),".capture-%ld.tmp",(long)getpid());
    int fd=openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0){close(dir);return false;}
    unsigned pos=0;
    while(pos<size){
        ssize_t n=write(fd,data+pos,size-pos);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        pos+=(unsigned)n;
    }
    bool ok=pos==size && !fsync(fd) && !fstat(fd,&st) && st.st_uid==0 &&
            (st.st_mode&0777)==0600 && st.st_size==size;
    if(close(fd))ok=false;
    /* Atomically replace only our fixed result inside a verified root-only RAM directory.
     * A previous good result remains intact on interrupted reads/writes. */
    if(ok)ok=renameat(dir,tmp,dir,"cali_coe")==0;
    if(!ok)unlinkat(dir,tmp,0);
    close(dir);return ok;
}
static int acquire_file(int argc,char **argv){
    (void)argv;
    if(argc!=1||!ram_scope()){fprintf(stderr,"sensor-cali: requires isolated IN2010 RAM data volume\n");return 2;}
    umask(077);
    int fd=open("/dev/rufs_persist",O_RDONLY|O_NOFOLLOW);struct stat st;
    uint64_t count=0;uint32_t logical=0,physical=0;
    if(fd<0||fstat(fd,&st)||!S_ISCHR(st.st_mode)||ioctl(fd,CALI_DK_BLOCKSIZE,&logical)||
       ioctl(fd,CALI_DK_BLOCKCOUNT,&count)||ioctl(fd,CALI_DK_PHYSICAL_BLOCKSIZE,&physical)||
       physical!=CE_BS || logical<512||logical>CE_BS||logical&(logical-1)||
       count!=CALI_BYTES/logical){fprintf(stderr,"sensor-cali: device identity or UFS geometry mismatch\n");if(fd>=0)close(fd);return 3;}
    struct ce_fs fs={.read=device_read,.source=&fd,.blocks=CALI_BYTES/CE_BS};
    fs.cache=calloc(CE_MAX_READS,sizeof(*fs.cache));uint8_t *data=malloc(CE_MAX_FILE);
    unsigned size=0;int status=1;
    if(fs.cache&&data&&ce_acquire(&fs,data,&size)){
        if(write_private(data,size)){
            uint8_t hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data,size,hash);
            printf("sensor-cali: captured source=persist:/engineermode/cali_coe bytes=%u blocks_read=%u metadata_crc32c=%u sha256=",size,fs.count,!!(fs.ro&1024));
            for(unsigned i=0;i<sizeof(hash);i++)printf("%02x",hash[i]);
            puts(" destination=/private/var/root/sensor-calibration/cali_coe mode=0600");status=0;
        }else fprintf(stderr,"sensor-cali: private RAM output failed\n");
    }else fprintf(stderr,"sensor-cali: read refused (%s), blocks_read=%u\n",fs.why?fs.why:"allocation",fs.count);
    if(data){memset(data,0,CE_MAX_FILE);free(data);}
    if(fs.cache){memset(fs.cache,0,CE_MAX_READS*sizeof(*fs.cache));free(fs.cache);}
    close(fd);return status;
}

/* This completion record lives on the same fresh RAM /var as the output.
 * Publish only after the selected file has been fully written or rejected. */
int main(int argc,char **argv){
    if(argc!=1||!ram_scope())return 2;
    int result=acquire_file(argc,argv);
    int fd=open("/private/var/root/.slpi-cali-checked",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd>=0){
        char flag=result==0?'1':'0';
        if(write(fd,&flag,1)!=1)fprintf(stderr,"sensor-cali: could not record completion\n");
        close(fd);
    }
    return result;
}
