/* Bounded read-only firmware API. Freestanding; stdint types supplied by caller. */
#ifndef FW_READER_H
#define FW_READER_H
#define FW_CAP (16ULL * 1024 * 1024)
struct fw_device {
 uint64_t last_block;
 uint32_t block_size, io_align;
 int present, logical;
 void *opaque;
 int (*read)(void *opaque, uint64_t lba, uint64_t bytes, void *dst);
};
struct fw_request { char name[32]; uint64_t offset, length; int info; };
static int fw_equal(const char *a,const char *b) { while(*a && *a==*b){a++;b++;} return *a==*b; }
static int fw_allowed(const char *name) {
 static const char *names[]={"super","vendor_a","vendor_b","modem_a","modem_b","bluetooth_a","bluetooth_b","dsp_a","dsp_b"};
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) if(fw_equal(name,names[i])) return 1;
 return 0;
}
/* No signs, spaces, octal, partial conversion, trailing junk or overflow. */
static int fw_integer(const char **cursor,uint64_t *out) {
 const char *p=*cursor;uint64_t v=0;unsigned base=10,n=0;
 if(p[0]=='0' && p[1]=='x'){base=16;p+=2;}
 while(*p && *p!=' ') {
  unsigned d;
  if(*p>='0' && *p<='9')d=(unsigned)(*p-'0');
  else if(*p>='a' && *p<='f')d=(unsigned)(*p-'a'+10);
  else if(*p>='A' && *p<='F')d=(unsigned)(*p-'A'+10);
  else return 0;
  if(d>=base || v>(~(uint64_t)0-d)/base)return 0;
  v=v*base+d;p++;n++;
 }
 if(!n)return 0;*cursor=p;*out=v;return 1;
}
static const char *fw_parse(const char *cmd,struct fw_request *r) {
 const char *p=cmd;unsigned n=0;
 const char *prefix="oem fw";
 while(*prefix)if(*p++!=*prefix++)return "bad firmware command";
 if(p[0]=='i'&&p[1]=='n'&&p[2]=='f'&&p[3]=='o'&&p[4]==' '){r->info=1;p+=5;}
 else if(p[0]=='r'&&p[1]=='e'&&p[2]=='a'&&p[3]=='d'&&p[4]==' '){r->info=0;p+=5;}
 else return "bad firmware command";
 while(*p && *p!=' ') {if(n+1>=sizeof(r->name))return "bad partition name";r->name[n++]=*p++;}
 r->name[n]=0;r->offset=0;r->length=0;
 if(!fw_allowed(r->name))return "partition not allowlisted";
 if(r->info)return *p ? "trailing arguments" : 0;
 if(*p++!=' ' || !fw_integer(&p,&r->offset) || *p++!=' ' || !fw_integer(&p,&r->length) || *p)return "invalid complete integers";
 return 0;
}
static const char *fw_geometry(const struct fw_device *d,uint64_t *bytes) {
 uint64_t blocks;
 if(!d->present || !d->logical || !d->read)return "not a present GPT volume";
 if(d->block_size<512 || d->block_size>65536 || (d->block_size&(d->block_size-1)))return "invalid block size";
 if(d->io_align>1 && (d->io_align&(d->io_align-1)))return "invalid I/O alignment";
 if(d->last_block==~(uint64_t)0)return "geometry overflow";
 blocks=d->last_block+1;
 if(blocks>~(uint64_t)0/d->block_size)return "geometry overflow";
 *bytes=blocks*d->block_size;return 0;
}
/* Lookup must provide only GPT partition handles. No write/reset/flush API exists. */
static const char *fw_execute(const struct fw_request *r,int busy,
 int (*lookup)(const char *,struct fw_device *),void *dst,uint64_t capacity,
 uint64_t *bytes,uint32_t *block_size,uint64_t *staged) {
 struct fw_device d;const char *why;uint64_t size;
 *staged=0;*bytes=0;*block_size=0;
 if(busy)return "kernel busy";
 if(!fw_allowed(r->name))return "partition not allowlisted";
 if(!lookup(r->name,&d))return "GPT partition unavailable";
 if((why=fw_geometry(&d,&size)))return why;
 *bytes=size;*block_size=d.block_size;
 if(r->info)return 0;
 if(!r->length || r->length>FW_CAP || r->length>capacity)return "range exceeds staging cap";
 if(r->offset%d.block_size || r->length%d.block_size)return "range not block aligned";
 /* Subtraction avoids offset+length overflow. */
 if(r->offset>size || r->length>size-r->offset)return "range outside partition";
 if(!dst || (d.io_align>1 && (uint64_t)(uintptr_t)dst%d.io_align))return "staging alignment invalid";
 if(!d.read(d.opaque,r->offset/d.block_size,r->length,dst))return "firmware read failed";
 *staged=r->length;return 0;
}
#endif
