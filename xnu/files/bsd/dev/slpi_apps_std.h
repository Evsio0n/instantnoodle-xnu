/* Session-local, read-only apps_std subset for the SLPI registry bootstrap. */
#ifndef SLPI_APPS_STD_H
#define SLPI_APPS_STD_H
#include "hk_fastrpc_wire.h"

struct ssc_entry { const char *path; unsigned offset, size; };
#include "slpi_config_manifest.h"

enum { SSC_BADARG=14, SSC_UNSUPPORTED=20, SSC_SMALL=38, SSC_BADHANDLE=44,
       SSC_FULL=45, SSC_NO_MODULE=62, SSC_NO_FILE=1, SSC_MODULE=0x7310 };
struct ssc_file { const struct ssc_entry *entry; unsigned token, offset; const uint8_t *data; };
struct ssc_buf { const uint8_t *p; unsigned n; };
struct ssc_inv { struct ssc_buf in[5]; unsigned out[3], ni,no; };
struct ssc_output { uint8_t bytes[2048]; unsigned used; };
static uint32_t ssc_word(const uint8_t *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void ssc_put(uint8_t *p,uint32_t n){memcpy(p,&n,4);}

static bool ssc_unpack(uint32_t sc,const uint8_t *b,unsigned n,struct ssc_inv *a)
{
    unsigned at=0;
    a->ni=(sc>>16)&255;a->no=(sc>>8)&255;
    if ((sc&255) || a->ni>5 || a->no>3 || n>HK_LISTENER_RX_CAP) return false;
    for(unsigned i=0;i<a->ni;i++){
        if(n-at<4)return false;
        unsigned len=ssc_word(b+at);at+=4;
        if(len)at=(at+7)&~7u;
        if(at>n || len>n-at)return false;
        a->in[i]=(struct ssc_buf){b+at,len};at+=len;
    }
    if(n-at!=a->no*4)return false;
    for(unsigned i=0;i<a->no;i++,at+=4)a->out[i]=ssc_word(b+at);
    return true;
}
static bool ssc_string(struct ssc_buf b,unsigned len)
{
    return b.n==len && len && b.p[len-1]==0 && !memchr(b.p,0,len-1);
}
static uint8_t *ssc_output_buf(struct ssc_output *out,unsigned n)
{
    unsigned at=out->used;
    if(at>sizeof(out->bytes)-4)return NULL;
    unsigned data=n?(at+4+7)&~7u:at+4;
    if(data>sizeof(out->bytes)||n>sizeof(out->bytes)-data)return NULL;
    memset(out->bytes+at,0,data-at+n);
    ssc_put(out->bytes+at,n);out->used=data+n;
    return out->bytes+data;
}
#include "slpi_std_dir.h"
#include "slpi_std_scratch.h"
struct ssc_session {
    const uint8_t *pack;
    const uint8_t *calibration;
    struct ssc_entry cal_file;
    size_t size;
    unsigned module_refs, generation;
    struct ssc_file files[8];
    struct ssc_dirs dirs;
    struct ssc_scratch scratch;
    char path[192], basename[96];
};
/* Only the fixed, privately acquired per-device file can back this optional
 * entry. Absent input remains absent for both stat and fopen. */
static bool ssc_cal_file(const struct ssc_session *s,const char *path)
{
    return s->calibration && s->cal_file.size && !strcmp(path,"/mnt/vendor/persist/engineermode/cali_coe");
}
static void ssc_note_path(struct ssc_session *s,struct ssc_buf b)
{
    if(!ssc_string(b,b.n)||b.n>sizeof(s->path))return;
    const char*path=(const char*)b.p;
    if(strncmp(path,"/vendor/etc/sensors/",20) && strncmp(path,"/odm/etc/sensor/",16) &&
       strncmp(path,"/mnt/vendor/persist/sensors/",28) && strncmp(path,"/sys/devices/soc0/",18))return;
    for(unsigned i=0;i+1<b.n;i++)if(b.p[i]<32||b.p[i]>126)return;
    memcpy(s->path,path,b.n);
}
static unsigned ssc_dispatch(struct ssc_session *s,uint32_t handle,uint32_t sc,
                             const uint8_t *blob,unsigned n,struct ssc_output *out)
{
    struct ssc_inv a={0};
    out->used=0;s->path[0]=0;s->basename[0]=0;
    if(!s->pack || s->size!=SSC_PACK_SIZE || !ssc_unpack(sc,blob,n,&a))return SSC_BADARG;
    for(unsigned i=0;i<a.ni;i++)ssc_note_path(s,a.in[i]);
    if(handle==0 && sc==0x20200){
        char name[65];
        if(!hk_listener_module_name(blob,n,name))return SSC_BADARG;
        if(strcmp(name,"apps_std"))return SSC_NO_MODULE;
        uint8_t *prim=ssc_output_buf(out,8);
        if(!prim||!ssc_output_buf(out,a.out[1]))return SSC_SMALL;
        if(s->module_refs>=32)return SSC_FULL;
        ssc_put(prim,SSC_MODULE);s->module_refs++;
        return 0;
    }
    if(handle!=SSC_MODULE || !s->module_refs)return SSC_BADHANDLE;
    unsigned scratch_result;
    if(ssc_scratch_try(&s->scratch,sc,&a,out,&scratch_result))return scratch_result;
    if(ssc_is_dir_call(sc))return ssc_dir_dispatch(&s->dirs,sc,&a,out);
    bool env_open = sc==0x13050100;
    if(sc==0x1f020100){
        if(a.in[0].n<8 || ssc_word(a.in[0].p)!=31) return SSC_UNSUPPORTED;
        if(!ssc_string(a.in[1],ssc_word(a.in[0].p+4))||a.out[0]!=96)return SSC_BADARG;
        if(ssc_cal_file(s,(const char*)a.in[1].p)){
            uint8_t *st=ssc_output_buf(out,96);
            ssc_put(st+16,0x62100001);ssc_put(st+24,0100400);ssc_put(st+28,1);
            ssc_put(st+40,s->cal_file.size);return 0;
        }
        if(ssc_config_directory((const char*)a.in[1].p)){
            uint8_t *st=ssc_output_buf(out,96);
            ssc_put(st+16,SSC_CONFIG_INO+(ssc_vendor_directory((const char*)a.in[1].p)?4:0));ssc_put(st+24,0040555);ssc_put(st+28,2);return 0;
        }
        for(unsigned i=0;i<sizeof(ssc_entries)/sizeof(ssc_entries[0]);i++){
            if(strcmp(ssc_entries[i].path,(const char*)a.in[1].p))continue;
            uint8_t *st=ssc_output_buf(out,96);
            /* Synthetic inode and Unix metadata of the immutable RAM filesystem. */
            ssc_put(st+16,i+1);ssc_put(st+24,0100444);ssc_put(st+28,1);
            ssc_put(st+40,ssc_entries[i].size);return 0;
        }
        return SSC_NO_FILE;
    }
    if(sc==0x30100 || env_open){
        unsigned path=env_open?3:1, mode=path+1;
        if(a.in[0].n!=4*(a.ni-1)||a.out[0]!=4)return SSC_BADARG;
        for(unsigned j=1;j<a.ni;j++)
            if(!ssc_string(a.in[j],ssc_word(a.in[0].p+4*(j-1))))return SSC_BADARG;
        const char *base=strrchr((const char*)a.in[path].p,'/');
        base=base?base+1:(const char*)a.in[path].p;
        size_t chars=strlen(base);
        if(chars && chars<sizeof(s->basename)){
            size_t j=0;
            for(;j<chars;j++){
                unsigned c=(unsigned char)base[j];
                if(!(c=='_'||c=='.'||c=='-'||(c>='a'&&c<='z')||
                    (c>='A'&&c<='Z')||(c>='0'&&c<='9')))break;
            }
            if(j==chars)memcpy(s->basename,base,chars+1);
        }
        if(strcmp((const char*)a.in[mode].p,"r") && strcmp((const char*)a.in[mode].p,"rb") && strcmp((const char*)a.in[mode].p,"rt"))
            return SSC_UNSUPPORTED;
        /* No host environment is exported. Native direct-path fallback resolves
         * only files present at their exact paths in this mounted config set. */
        const struct ssc_entry *entry=NULL;
        for(unsigned i=0;i<sizeof(ssc_entries)/sizeof(ssc_entries[0]);i++)
            if(!strcmp(ssc_entries[i].path,(const char*)a.in[path].p)){entry=&ssc_entries[i];break;}
        const uint8_t *data;
        if(ssc_cal_file(s,(const char*)a.in[path].p)){
            entry=&s->cal_file;data=s->calibration;
        }else{
            if(!entry)return SSC_NO_FILE;
            if(entry->offset>s->size || entry->size>s->size-entry->offset)return SSC_BADARG;
            data=s->pack+entry->offset;
        }
        for(unsigned i=0;i<8;i++)if(!s->files[i].token){
            if(s->generation>=0x7fffff00u)return SSC_FULL;
            s->files[i]=(struct ssc_file){entry,++s->generation,0,data};
            ssc_put(ssc_output_buf(out,4),s->files[i].token);return 0;
        }
        return SSC_FULL;
    }
    if(sc!=0x03010000 && sc!=0x04010200 && sc!=0x08010100 &&
       sc!=0x09010000 && sc!=0x0a010100 && sc!=0x0b010000)return SSC_UNSUPPORTED;
    unsigned need=sc==0x04010200?8:sc==0x09010000?12:4;
    if(a.in[0].n!=need)return SSC_BADARG;
    unsigned token=ssc_word(a.in[0].p);struct ssc_file *file=NULL;
    for(unsigned i=0;token&&i<8;i++)if(s->files[i].token==token)file=&s->files[i];
    if(!file)return SSC_BADHANDLE;
    if(sc==0x03010000){memset(file,0,sizeof(*file));return 0;}
    if(sc==0x0b010000){file->offset=0;return 0;}
    if(sc==0x0a010100){
        if(a.out[0]!=8)return SSC_BADARG;
        ssc_put(ssc_output_buf(out,8),file->entry->size);return 0;
    }
    if(sc==0x08010100){
        if(a.out[0]!=4)return SSC_BADARG;
        ssc_put(ssc_output_buf(out,4),file->offset);return 0;
    }
    if(sc==0x09010000){
        uint32_t whence=ssc_word(a.in[0].p+8);
        if(whence>2)return SSC_BADARG;
        int64_t offset=whence==0?0:whence==1?file->offset:file->entry->size;
        offset+=(int32_t)ssc_word(a.in[0].p+4);
        if(offset<0||(uint64_t)offset>file->entry->size)return SSC_BADARG;
        file->offset=(unsigned)offset;return 0;
    }
    unsigned requested=ssc_word(a.in[0].p+4);
    if(a.out[0]!=8||requested!=a.out[1])return SSC_BADARG;
    uint8_t *prim=ssc_output_buf(out,8), *dst=ssc_output_buf(out,requested);
    if(!prim||!dst)return SSC_SMALL;
    unsigned remain=file->entry->size-file->offset;
    unsigned count=requested<remain?requested:remain;
    memcpy(dst,file->data+file->offset,count);
    ssc_put(prim,count);ssc_put(prim+4,count<requested);
    file->offset+=count;return 0;
}
#endif
