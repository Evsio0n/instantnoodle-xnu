#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include "cali_ext4.h"

static int permission=0600, owner=0, receipt_size=32, open_ok=1, changed=0, read_ok=1;
static uint8_t expected[32],media[CE_BS];
static unsigned calls;
static int testopen(const char *path,int flags) {
    assert(strcmp(path,"/hackios/fw/als-calibration-249.sha256")==0);
    assert(flags==(O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    return open_ok?20:-1;
}
static int testfstat(int fd, struct stat *sb) {
    assert(fd==20);
    memset(sb,0,sizeof(*sb));sb->st_mode=S_IFREG|permission;sb->st_uid=owner;sb->st_size=receipt_size;
    return 0;
}
static ssize_t testpread(int fd,void *data,size_t n,off_t off) {
    assert(fd==20 && n==32 && off==0);memcpy(data,expected,32);return 32;
}
static int testclose(int fd){assert(fd==20);return 0;}
#define open testopen
#define fstat testfstat
#define pread testpread
#define close testclose
#include "cal_receipt.h"
static int readblock(void *unused,uint32_t block,uint8_t *data) {
    assert(unused==NULL && block==9);
    calls++;
    memcpy(data,media,CE_BS);
    if(changed)data[111]++;
    return read_ok;
}
int main(void) {
    const uint8_t current[]={'T','e','s','t'};
    struct ce_fs fs={.read=readblock,.inc=0x46,.ro=0x7b,.count=1};
    struct ce_slot slot={.number=9};
    memcpy(slot.data,media,CE_BS);fs.cache=&slot;
    CC_SHA256(current,sizeof(current),expected);
    assert(als_current_capture(&fs,current,4) && calls==1);
    expected[11]++;assert(!als_current_capture(&fs,current,4));expected[11]--;
    assert(!als_current_capture(&fs,current,3));
    changed=1;assert(!als_current_capture(&fs,current,4));changed=0;
    read_ok=0;assert(!als_current_capture(&fs,current,4));read_ok=1;
    open_ok=0;assert(!als_current_capture(&fs,current,4));open_ok=1;
    receipt_size=33;assert(!als_current_capture(&fs,current,4));receipt_size=32;
    permission=0666;assert(!als_current_capture(&fs,current,4));permission=0600;
    owner=501;assert(!als_current_capture(&fs,current,4));owner=0;
    fs.inc=0x146;assert(!als_current_capture(&fs,current,4));fs.inc=0x46;
    fs.ro=0x7f;assert(!als_current_capture(&fs,current,4));fs.inc=0x42;
    /* Ordinary clean-vol path is already verified by the original ext4 parser. */
    assert(als_current_capture(&fs,current,4));
    puts("PASS matching live digest, pathname/metadata re-read, mismatch, mutation, read error, receipt metadata, foreign filesystem features");
    return 0;
}
