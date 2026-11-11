/* 20F66 RAM diagnostic. Enumerate only aggregate Apple stock matches, then
 * query a fixed set of first-party LaunchServices controls as mobile. Never
 * register, launch, remove or change apps. The build supplies the captured
 * stock manifest in gnss-settings-next/stock-check/stock_apps_baseline.inc. */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <sys/attr.h>
#include <sys/stat.h>
/* ATTR_CMN_OBJTYPE uses Darwin enum vtype. The iOS SDK omits sys/vnode.h. */
enum { STOCK_VDIR=2 };
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (StockProxyRead)
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSString *)bundleIdentifier;
- (NSURL *)bundleURL;
- (BOOL)isInstalled;
@end

struct control {const char *name,*identifier,*exe,*plist_hash,*exe_hash;};
#include "stock_apps_baseline.inc"
_Static_assert(sizeof(stock_names)/sizeof(stock_names[0])==STOCK_APPS,"stock name count");
struct inventory { bool found[STOCK_APPS]; unsigned base,staged,duplicates; int error; };
static int index_of(const char *name) {
    for(unsigned i=0;i<STOCK_APPS;i++) if(!strcmp(stock_names[i],name))return (int)i;
    return -1;
}
static void observe(struct inventory *v,const char *name,bool directory) {
    if(!directory)return;
    int i=index_of(name);
    if(i<0)return;
    if(v->found[i]){v->duplicates++;return;}
    v->found[i]=true;
    if(i<BASE_APPS)v->base++;else v->staged++;
}
static bool complete(const struct inventory *v) {
    return !v->error && !v->duplicates && v->base==BASE_APPS && v->staged==STAGED_APPS;
}
/* getattrlistbulk packs each attribute at 4-byte boundaries. The signed name
 * offset is relative to its own attrreference, not the beginning of the row. */
struct bulk_record { uint32_t len; attribute_set_t returned; attrreference_t name; uint32_t type; };
static bool consume(const uint8_t *buf,size_t size,int count,struct inventory *v) {
    size_t pos=0;size_t base=offsetof(struct bulk_record,name);
    for(int i=0;i<count;i++){
        struct bulk_record row;
        if(pos>size||size-pos<sizeof(row))return false;
        memcpy(&row,buf+pos,sizeof(row));
        if(row.len<sizeof(row)||row.len>size-pos||row.len%4)return false;
        if((row.returned.commonattr&(ATTR_CMN_NAME|ATTR_CMN_OBJTYPE))!=(ATTR_CMN_NAME|ATTR_CMN_OBJTYPE))return false;
        int64_t start=(int64_t)base+row.name.attr_dataoffset;
        if(start<(int64_t)sizeof(row)||(uint64_t)start>row.len||!row.name.attr_length||
           row.name.attr_length>row.len-(uint64_t)start)return false;
        const char *name=(const char *)(buf+pos+start);
        if(strnlen(name,row.name.attr_length)!=row.name.attr_length-1)return false;
        observe(v,name,row.type==STOCK_VDIR);
        pos+=row.len;
    }
    return true;
}
static struct inventory bulk_inventory(const char *path) {
    struct inventory v={0};int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0){v.error=errno;return v;}
    struct attrlist a={.bitmapcount=ATTR_BIT_MAP_COUNT,.commonattr=ATTR_CMN_RETURNED_ATTRS|ATTR_CMN_NAME|ATTR_CMN_OBJTYPE};
    uint8_t buf[32768];bool ended=false;
    for(unsigned page=0;page<128;page++){
        int n=getattrlistbulk(fd,&a,buf,sizeof(buf),FSOPT_PACK_INVAL_ATTRS);
        if(n<0 && errno==EINTR){page--;continue;}
        if(n<0){v.error=errno;break;}
        if(n==0){ended=true;break;}
        if(!consume(buf,sizeof(buf),n,&v)){v.error=EPROTO;break;}
    }
    if(!v.error&&!ended)v.error=EOVERFLOW;
    close(fd);return v;
}
static struct inventory plain_inventory(const char *path) {
    struct inventory v={0};DIR *dir=opendir(path);
    if(!dir){v.error=errno;return v;}
    unsigned count=0;
    for(;;){
        errno=0;struct dirent *e=readdir(dir);
        if(!e){if(errno)v.error=errno;break;}
        if(++count>16384){v.error=EOVERFLOW;break;}
        bool directory=e->d_type==DT_DIR;
        if(e->d_type==DT_UNKNOWN && index_of(e->d_name)>=0){
            struct stat st;
            directory=fstatat(dirfd(dir),e->d_name,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode);
        }
        observe(&v,e->d_name,directory);
    }
    closedir(dir);return v;
}
static bool hash_matches(NSString *path,const char *expected) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;
    struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0 && st.st_size<64*1024*1024;
    uint8_t buf[32768],digest[CC_SHA256_DIGEST_LENGTH];char hex[65];CC_SHA256_CTX sha;CC_SHA256_Init(&sha);
    off_t total=0;
    while(ok){
        ssize_t n=read(fd,buf,sizeof(buf));
        if(n<0&&errno==EINTR)continue;
        if(n<0){ok=false;break;}if(!n)break;
        total+=n;if(total>st.st_size){ok=false;break;}
        CC_SHA256_Update(&sha,buf,(CC_LONG)n);
    }
    close(fd);if(!ok||total!=st.st_size)return false;
    CC_SHA256_Final(digest,&sha);
    for(unsigned i=0;i<sizeof(digest);i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    return !strcmp(hex,expected);
}
static bool trusted_proxy_path(NSString *path,const struct control *c) {
    if(!path || ![path.lastPathComponent isEqualToString:@(c->name)])return false;
    NSString *parent=path.stringByDeletingLastPathComponent;
    if([parent isEqualToString:@"/Applications"])return true;
    if(index_of(c->name)>=BASE_APPS && [parent isEqualToString:@"/private/var/staged_system_apps"])return true;
    /* Built-in first-boot apps may legitimately be installed in a real bundle
     * container. Accept its normal UUID path only with the same original
     * executable and plist hashes, checked by the caller. */
    if(index_of(c->name)>=BASE_APPS){
        for(NSString *prefix in @[@"/var/containers/Bundle/Application/",@"/private/var/containers/Bundle/Application/"]){
            if([parent hasPrefix:prefix]){
                NSString *component=[parent substringFromIndex:prefix.length];
                if(component.length==36 && [[NSUUID alloc] initWithUUIDString:component])return true;
            }
        }
    }
    return false;
}
static bool same_vnode(NSString *a,NSString *b) {
    struct stat x,y;
    return !lstat(a.fileSystemRepresentation,&x)&&!lstat(b.fileSystemRepresentation,&y)&&
           S_ISREG(x.st_mode)&&S_ISREG(y.st_mode)&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino;
}
struct gate { bool bulk,plain,info,exe,proxy_exists,identifier,installed,url,registered_bytes; };
static struct gate control_gate(const struct control *c,NSString *apps,Class proxy,
        const struct inventory *bulk,const struct inventory *plain) {
    NSString *path=[apps stringByAppendingPathComponent:@(c->name)];
    NSString *info=[path stringByAppendingPathComponent:@"Info.plist"],*exe=[path stringByAppendingPathComponent:@(c->exe)];
    int i=index_of(c->name);
    struct gate g={.bulk=bulk->found[i],.plain=plain->found[i],.info=hash_matches(info,c->plist_hash),.exe=hash_matches(exe,c->exe_hash)};
    if(!proxy || ![proxy respondsToSelector:@selector(applicationProxyForIdentifier:)])return g;
    id item=[proxy applicationProxyForIdentifier:@(c->identifier)];g.proxy_exists=item!=nil;
    if(![item respondsToSelector:@selector(bundleIdentifier)] || ![item respondsToSelector:@selector(isInstalled)] ||
       ![item respondsToSelector:@selector(bundleURL)])return g;
    g.identifier=[[item bundleIdentifier] isEqualToString:@(c->identifier)];g.installed=[item isInstalled];
    NSURL *url=[item bundleURL];NSString *registered=url.fileURL?url.path.stringByStandardizingPath:nil;
    g.url=trusted_proxy_path(registered,c);if(!g.identifier||!g.installed||!g.url||!g.info||!g.exe)return g;
    NSString *rinfo=[registered stringByAppendingPathComponent:@"Info.plist"],*rexe=[registered stringByAppendingPathComponent:@(c->exe)];
    g.registered_bytes=(same_vnode(info,rinfo)||hash_matches(rinfo,c->plist_hash)) &&
        (same_vnode(exe,rexe)||hash_matches(rexe,c->exe_hash));
    return g;
}
static bool accepted(struct gate g) {return g.bulk&&g.plain&&g.info&&g.exe&&g.proxy_exists&&g.identifier&&g.installed&&g.url&&g.registered_bytes;}
#ifndef STOCK_APPS_HOST_TEST
int main(void) {
    int logfd=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);if(logfd<0)logfd=STDOUT_FILENO;
    gid_t mobile=501;
    if(geteuid()==0 && (setgroups(1,&mobile)||setgid(mobile)||setuid(501))){
        dprintf(logfd,"stock-apps: unavailable reason=drop-mobile end\n");return 2;
    }
    if(getuid()!=501||geteuid()!=501||getgid()!=501||getegid()!=501){
        dprintf(logfd,"stock-apps: unavailable reason=mobile-identity end\n");return 2;
    }
    @autoreleasepool {
        struct inventory bulk=bulk_inventory("/Applications"),plain=plain_inventory("/Applications");
        dprintf(logfd,"stock-apps: enum uid=501 bulk_base=%u bulk_staged=%u bulk_dup=%u bulk_errno=%d plain_base=%u plain_staged=%u plain_dup=%u plain_errno=%d end\n",
            bulk.base,bulk.staged,bulk.duplicates,bulk.error,plain.base,plain.staged,plain.duplicates,plain.error);
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_LOCAL|RTLD_LAZY);
        Class proxy=NSClassFromString(@"LSApplicationProxy");unsigned passed=0;
        dprintf(logfd,"stock-apps: lookup uid=501 api_ready=%d end\n",proxy!=Nil && [proxy respondsToSelector:@selector(applicationProxyForIdentifier:)]);
        for(unsigned i=0;i<sizeof(controls)/sizeof(*controls);i++){
            @autoreleasepool {
                const struct control *c=&controls[i];struct gate g=control_gate(c,@"/Applications",proxy,&bulk,&plain);
                passed+=accepted(g);
                dprintf(logfd,"stock-apps: control id=%s bulk=%d plain=%d info_sha=%d exe_sha=%d proxy_exists=%d proxy_id=%d installed=%d url_ok=%d proxy_bytes=%d pass=%d end\n",
                        c->identifier,g.bulk,g.plain,g.info,g.exe,g.proxy_exists,g.identifier,g.installed,g.url,g.registered_bytes,accepted(g));
            }
        }
        bool ok=complete(&bulk)&&complete(&plain)&&passed==sizeof(controls)/sizeof(*controls);
        dprintf(logfd,"stock-apps: result uid=501 controls=%u total=%zu pass=%d end\n",passed,sizeof(controls)/sizeof(*controls),ok);
        if(logfd!=STDOUT_FILENO)close(logfd);return ok?0:1;
    }
}
#endif
