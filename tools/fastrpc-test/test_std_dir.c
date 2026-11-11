#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/slpi_apps_std.h"
#include "../../xnu/files/bsd/dev/slpi_std_dir.h"

static struct ssc_dirs dirs;
static struct ssc_output out;
static unsigned open_dir(const char *path, uint64_t *token)
{
    unsigned len = strlen(path)+1;
    uint8_t b[128]={0};
    ssc_put(b,4); ssc_put(b+8,len); ssc_put(b+12,len);
    memcpy(b+16,path,len); ssc_put(b+16+len,8);
    struct ssc_inv inv;
    assert(ssc_unpack(SSC_OPENDIR,b,20+len,&inv));
    out.used=0;
    unsigned e=ssc_dir_dispatch(&dirs,SSC_OPENDIR,&inv,&out);
    if (!e) {
        assert(out.used==16 && ssc_word(out.bytes)==8);
        memcpy(token,out.bytes+8,8);
    }
    return e;
}
static unsigned use_dir(uint32_t sc, uint64_t token, unsigned out_len)
{
    uint8_t b[20]={0}; ssc_put(b,8); memcpy(b+8,&token,8); ssc_put(b+16,out_len);
    struct ssc_inv inv;
    assert(ssc_unpack(sc,b,sc==SSC_CLOSEDIR?16:20,&inv));
    out.used=0;
    return ssc_dir_dispatch(&dirs,sc,&inv,&out);
}

int main(void)
{
    uint64_t one=0,two=0,extra[3]={0};
    assert(open_dir(SSC_CONFIG_DIR "/../config",&one)==SSC_NO_FILE);
    assert(open_dir("/vendor/etc/sensors",&one)==SSC_NO_FILE);
    assert(open_dir(SSC_CONFIG_DIR,&one)==0);
    assert(open_dir(SSC_CONFIG_DIR "/",&two)==0 && one!=two);
    assert(use_dir(SSC_READDIR,one,263)==SSC_BADARG);
    assert(use_dir(SSC_READDIR,one,9000)==SSC_SMALL);
    assert(use_dir(SSC_READDIR,one,264)==0 && !strcmp((char*)out.bytes+12,"."));
    assert(use_dir(SSC_READDIR,one,264)==0 && !strcmp((char*)out.bytes+12,".."));
    unsigned found=0, matched=0;
    uint32_t seen[64]={0};
    for (unsigned i=0;i<sizeof(ssc_entries)/sizeof(ssc_entries[0]);i++)
        if (!strncmp(ssc_entries[i].path,SSC_CONFIG_DIR "/",sizeof(SSC_CONFIG_DIR)))
            matched++;
    for (;;) {
        assert(use_dir(SSC_READDIR,one,264)==0 && out.used==272);
        if (ssc_word(out.bytes+268)) break;
        uint32_t ino=ssc_word(out.bytes+8);
        assert(ino && ino<=sizeof(ssc_entries)/sizeof(ssc_entries[0]) && ino<=64);
        assert(!seen[ino-1]++);
        char path[192];int n=snprintf(path,sizeof(path),"%s/%s",SSC_CONFIG_DIR,(char*)out.bytes+12);
        assert(n>0 && n<(int)sizeof(path) && !strcmp(path,ssc_entries[ino-1].path));
        assert(!strchr((char*)out.bytes+12,'/'));found++;
    }
    assert(found==matched && found==13);
    assert(use_dir(SSC_READDIR,one,264)==0 && ssc_word(out.bytes+268)==1);
    assert(use_dir(SSC_READDIR,two,264)==0 && !strcmp((char*)out.bytes+12,"."));
    assert(open_dir(SSC_CONFIG_DIR,&extra[0])==0 && open_dir(SSC_CONFIG_DIR,&extra[1])==0);
    assert(open_dir(SSC_CONFIG_DIR,&extra[2])==SSC_FULL);
    assert(use_dir(SSC_CLOSEDIR,one,0)==0);
    assert(open_dir(SSC_CONFIG_DIR,&extra[2])==0 && extra[2]!=one);
    assert(use_dir(SSC_READDIR,one,264)==SSC_BADHANDLE && use_dir(SSC_CLOSEDIR,one,0)==SSC_BADHANDLE);
    assert(use_dir(SSC_READDIR,0,264)==SSC_BADHANDLE);
    /* Also use the ordinary dispatcher symbol when compiling this isolated integration fixture. */
    (void)&ssc_dispatch;
    assert(!use_dir(SSC_CLOSEDIR,two,0));
    for(unsigned i=0;i<3;i++)assert(!use_dir(SSC_CLOSEDIR,extra[i],0));
    uint64_t vendor=0;assert(!open_dir(SSC_VENDOR_CONFIG_DIR,&vendor));
    unsigned vendor_files=0;
    for(;;){
        assert(!use_dir(SSC_READDIR,vendor,264));
        if(ssc_word(out.bytes+268))break;
        const char *name=(const char*)out.bytes+12;
        if(!strcmp(name,".")||!strcmp(name,".."))continue;
        unsigned ino=ssc_word(out.bytes+8);
        assert(ino && ino<=sizeof(ssc_entries)/sizeof(ssc_entries[0]));
        char full[192];snprintf(full,sizeof(full),"%s/%s",SSC_VENDOR_CONFIG_DIR,name);
        assert(!strcmp(full,ssc_entries[ino-1].path));vendor_files++;
    }
    assert(vendor_files==84 && !use_dir(SSC_CLOSEDIR,vendor,0));
    puts("PASS directory wire ABI, 13 actual manifest files, independent cursors, EOF, bounds and stale handles.");
}
