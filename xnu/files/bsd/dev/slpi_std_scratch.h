/* Volatile registry output, scoped to one SensorsPD session. Include after
 * ssc_output_buf. Allocate this state in zeroed storage, not a thread stack. */
#ifndef SLPI_STD_SCRATCH_H
#define SLPI_STD_SCRATCH_H
#define SSC_SCR_ROOT "/mnt/vendor/persist/sensors/registry"
#define SSC_SCR_DIR_PREFIX UINT64_C(0x5352434400000000)
enum { SCR_NODES=1024, SCR_BLOCKS=4096, SCR_UNIT=512, SCR_DEPTH=16,
       SCR_NAME=96, SCR_FDS=16, SCR_DIRS=4, SCR_NOENT=2, SCR_EXIST=17,
       SCR_NOTDIR=20, SCR_ISDIR=21, SCR_NOSPACE=28, SCR_NOTEMPTY=39 };
struct scr_node {
    bool used, linked, dir;
    uint16_t parent, head, refs;
    uint32_t size, ino;
    char name[SCR_NAME];
};
struct scr_fd {unsigned token, node, pos; bool read, write, append, eof;};
struct scr_dir {uint64_t token; unsigned node, pos;};
struct ssc_scratch {
    struct scr_node nodes[SCR_NODES];
    struct scr_fd fds[SCR_FDS];
    struct scr_dir dirs[SCR_DIRS];
    uint8_t data[SCR_BLOCKS][SCR_UNIT];
    uint16_t next[SCR_BLOCKS+1]; /* block zero is the end of a chain */
    uint16_t free_head, free_count;
    uint32_t next_ino, generation;
};
struct scr_path {unsigned count; struct ssc_buf parts[SCR_DEPTH];};
static inline void scr_init(struct ssc_scratch *fs)
{
    if(fs->next_ino)return;
    fs->nodes[0]=(struct scr_node){.used=true,.linked=true,.dir=true,.ino=0x62000001};
    fs->nodes[1]=(struct scr_node){.used=true,.linked=true,.dir=true,.ino=0x62000002,.name="registry"};
    fs->next_ino=0x62000002;fs->free_head=1;fs->free_count=SCR_BLOCKS;
    for(unsigned i=1;i<SCR_BLOCKS;i++)fs->next[i]=(uint16_t)(i+1);
}
static inline bool scr_owned(const struct ssc_buf b)
{
    const unsigned root=sizeof(SSC_SCR_ROOT)-1;
    return b.n>root && !memcmp(b.p,SSC_SCR_ROOT,root) && (!b.p[root] || b.p[root]=='/');
}
static inline unsigned scr_parse(struct ssc_buf b, struct scr_path *p)
{
    p->count=0;
    if(!ssc_string(b,b.n)||!scr_owned(b))return SSC_BADARG;
    unsigned i=sizeof(SSC_SCR_ROOT)-1;
    while(i<b.n-1){
        if(b.p[i++]!='/')return SSC_BADARG;
        unsigned at=i;
        while(i<b.n-1&&b.p[i]!='/'){
            unsigned c=b.p[i++];
            if(c<0x20 || c>=0x7f || c=='\\')return SSC_BADARG;
        }
        unsigned n=i-at;
        if(!n || (n==1 && b.p[at]=='.'))continue;
        if(n==2&&b.p[at]=='.'&&b.p[at+1]=='.'){
            if(!p->count)return SSC_BADARG;
            p->count--;continue;
        }
        if(n>=SCR_NAME || p->count>=SCR_DEPTH)return SSC_BADARG;
        p->parts[p->count++]=(struct ssc_buf){b.p+at,n};
    }
    return 0;
}
static inline int scr_child(const struct ssc_scratch *fs,unsigned parent,struct ssc_buf part)
{
    for(unsigned i=1;i<SCR_NODES;i++){
        const struct scr_node *e=&fs->nodes[i];
        if(e->linked&&e->parent==parent&&strlen(e->name)==part.n&&
           !memcmp(e->name,part.p,part.n))return (int)i;
    }
    return -1;
}
static inline unsigned scr_lookup(struct ssc_scratch *fs,const struct scr_path *p,
                                  unsigned count,unsigned *index)
{
    unsigned node=0;
    for(unsigned i=0;i<count;i++){
        if(!fs->nodes[node].dir)return SCR_NOTDIR;
        int child=scr_child(fs,node,p->parts[i]);
        if(child<0)return SCR_NOENT;
        node=(unsigned)child;
    }
    *index=node;return 0;
}
static inline unsigned scr_parent(struct ssc_scratch *fs,const struct scr_path *p,unsigned *parent)
{
    if(!p->count)return SSC_BADARG;
    unsigned e=scr_lookup(fs,p,p->count-1,parent);
    if(e)return e;
    return fs->nodes[*parent].dir?0:SCR_NOTDIR;
}
static inline unsigned scr_create(struct ssc_scratch *fs,const struct scr_path *p,bool dir,unsigned *idx)
{
    unsigned parent=0,err=scr_parent(fs,p,&parent);
    if(err)return err;
    struct ssc_buf part=p->parts[p->count-1];
    if(scr_child(fs,parent,part)>=0)return SCR_EXIST;
    if(fs->next_ino==UINT32_MAX)return SCR_NOSPACE;
    for(unsigned i=1;i<SCR_NODES;i++)if(!fs->nodes[i].used){
        struct scr_node *node=&fs->nodes[i];
        *node=(struct scr_node){.used=true,.linked=true,.dir=dir,
                                .parent=(uint16_t)parent,.ino=++fs->next_ino};
        memcpy(node->name,part.p,part.n);*idx=i;return 0;
    }
    return SCR_NOSPACE;
}
static inline unsigned scr_resize(struct ssc_scratch *fs,struct scr_node *node,unsigned len)
{
    if(len>SCR_BLOCKS*SCR_UNIT)return SCR_NOSPACE;
    unsigned old=(node->size+SCR_UNIT-1)/SCR_UNIT,now=(len+SCR_UNIT-1)/SCR_UNIT;
    if(now>old && now-old>fs->free_count)return SCR_NOSPACE;
    uint16_t *end=&node->head;
    for(unsigned i=0;i<(now<old?now:old);i++)end=&fs->next[*end];
    for(unsigned i=old;i<now;i++){
        unsigned b=fs->free_head;
        fs->free_head=fs->next[b];fs->free_count--;
        fs->next[b]=0;*end=(uint16_t)b;end=&fs->next[b];
        memset(fs->data[b-1],0,SCR_UNIT);
    }
    uint16_t drop=*end;*end=0;
    while(drop){
        uint16_t next=fs->next[drop];
        fs->next[drop]=fs->free_head;fs->free_head=drop;fs->free_count++;drop=next;
    }
    /* Clear the discarded suffix of a retained partial block so later growth
     * (including a seek beyond EOF followed by write) reads as zeroes. */
    if(len<node->size && len%SCR_UNIT){
        unsigned b=node->head;for(unsigned i=1;i<now;i++)b=fs->next[b];
        memset(fs->data[b-1]+len%SCR_UNIT,0,SCR_UNIT-len%SCR_UNIT);
    }
    node->size=len;return 0;
}
static inline void scr_copy(struct ssc_scratch *fs,const struct scr_node *node,unsigned pos,
                            uint8_t *dst,const uint8_t *src,unsigned count)
{
    if(!count)return;
    unsigned b=node->head;
    for(unsigned i=0;i<pos/SCR_UNIT;i++)b=fs->next[b];
    while(count){
        unsigned off=pos%SCR_UNIT,n=SCR_UNIT-off;if(n>count)n=count;
        if(src){memcpy(fs->data[b-1]+off,src,n);src+=n;}
        else {memcpy(dst,fs->data[b-1]+off,n);dst+=n;}
        pos+=n;count-=n;b=fs->next[b];
    }
}
static inline void scr_reap(struct ssc_scratch *fs,unsigned idx)
{
    struct scr_node *n=&fs->nodes[idx];
    if(n->linked||n->refs)return;
    (void)scr_resize(fs,n,0);memset(n,0,sizeof(*n));
}
static inline bool scr_empty(const struct ssc_scratch *fs,unsigned idx)
{
    for(unsigned i=1;i<SCR_NODES;i++)if(fs->nodes[i].linked&&fs->nodes[i].parent==idx)return false;
    return true;
}
static inline unsigned scr_unlink(struct ssc_scratch *fs,const struct scr_path *p,bool dir)
{
    unsigned idx,err=scr_lookup(fs,p,p->count,&idx);
    if(err)return err;
    if(!idx)return SSC_BADARG;
    struct scr_node *n=&fs->nodes[idx];
    if(n->dir!=dir)return dir?SCR_NOTDIR:SCR_ISDIR;
    if(dir&&!scr_empty(fs,idx))return SCR_NOTEMPTY;
    n->linked=false;scr_reap(fs,idx);return 0;
}
static inline unsigned scr_rename(struct ssc_scratch *fs,const struct scr_path *from,const struct scr_path *to)
{
    unsigned src=0,parent=0,e=scr_lookup(fs,from,from->count,&src);
    if(e)return e;
    if(!src)return SSC_BADARG;
    if((e=scr_parent(fs,to,&parent)))return e;
    if(fs->nodes[src].dir)
        for(unsigned p=parent;p;p=fs->nodes[p].parent)if(p==src)return SSC_BADARG;
    struct ssc_buf leaf=to->parts[to->count-1];
    int dst=scr_child(fs,parent,leaf);
    if(dst==(int)src)return 0;
    if(dst>=0){
        if(fs->nodes[dst].dir!=fs->nodes[src].dir)return fs->nodes[dst].dir?SCR_ISDIR:SCR_NOTDIR;
        if(fs->nodes[dst].dir&&!scr_empty(fs,(unsigned)dst))return SCR_NOTEMPTY;
        fs->nodes[dst].linked=false;scr_reap(fs,(unsigned)dst);
    }
    fs->nodes[src].parent=(uint16_t)parent;
    memcpy(fs->nodes[src].name,leaf.p,leaf.n);fs->nodes[src].name[leaf.n]=0;return 0;
}
static inline unsigned scr_token(struct ssc_scratch *fs)
{
    if(fs->generation==0xffffffu)return 0;
    return 0x63000000u|++fs->generation;
}
static inline unsigned scr_open(struct ssc_scratch *fs,const struct scr_path *path,struct ssc_buf mode,unsigned *token)
{
    if(!ssc_string(mode,mode.n)||mode.n<2||mode.n>4)return SSC_BADARG;
    unsigned c=mode.p[0];
    if(c!='r'&&c!='w'&&c!='a')return SSC_BADARG;
    bool plus=false,binary=false;
    for(unsigned i=1;i+1<mode.n;i++){
        if(mode.p[i]=='+'&&!plus)plus=true;
        else if(mode.p[i]=='b'&&!binary)binary=true;
        else return SSC_BADARG;
    }
    struct scr_fd *fd=NULL;
    for(unsigned i=0;i<SCR_FDS;i++)if(!fs->fds[i].token){fd=&fs->fds[i];break;}
    if(!fd || fs->generation==0xffffffu)return SSC_FULL;
    unsigned idx=0,e=scr_lookup(fs,path,path->count,&idx);
    if(e==SCR_NOENT && c!='r')e=scr_create(fs,path,false,&idx);
    if(e)return e;
    struct scr_node *node=&fs->nodes[idx];
    if(node->dir)return SCR_ISDIR;
    if(c=='w')scr_resize(fs,node,0);
    *fd=(struct scr_fd){.token=scr_token(fs),.node=idx,.pos=c=='a'?node->size:0,
                        .read=c=='r'||plus,.write=c!='r'||plus,.append=c=='a'};
    node->refs++;*token=fd->token;return 0;
}
static inline unsigned scr_directory(struct ssc_scratch *fs,uint32_t sc,
                                      const struct ssc_inv *a,struct ssc_output *out,
                                      const struct scr_path *path)
{
    if(sc==SSC_OPENDIR){
        if(a->out[0]!=8)return SSC_BADARG;
        unsigned node,e=scr_lookup(fs,path,path->count,&node);
        if(e)return e;
        if(!fs->nodes[node].dir)return SCR_NOTDIR;
        for(unsigned i=0;i<SCR_DIRS;i++)if(!fs->dirs[i].token){
            unsigned token=scr_token(fs);if(!token)return SSC_FULL;
            uint64_t t=SSC_SCR_DIR_PREFIX|token;
            fs->dirs[i]=(struct scr_dir){t,node,0};fs->nodes[node].refs++;
            memcpy(ssc_output_buf(out,8),&t,8);return 0;
        }
        return SSC_FULL;
    }
    uint64_t t;memcpy(&t,a->in[0].p,8);
    for(unsigned i=0;i<SCR_DIRS;i++)if(fs->dirs[i].token==t){
        struct scr_dir *d=&fs->dirs[i];struct scr_node *n=&fs->nodes[d->node];
        if(sc==SSC_CLOSEDIR){
            n->refs--;scr_reap(fs,d->node);memset(d,0,sizeof(*d));return 0;
        }
        if(a->out[0]<264)return SSC_BADARG;
        uint8_t *p=ssc_output_buf(out,a->out[0]);if(!p)return SSC_SMALL;
        if(n->linked && d->pos<2){
            bool self=d->pos++==0;ssc_put(p,self?n->ino:fs->nodes[n->parent].ino);
            memcpy(p+4,self?".":"..",self?2:3);return 0;
        }
        while(n->linked && d->pos-2<SCR_NODES){
            unsigned k=d->pos++-2;
            if(k && fs->nodes[k].linked && fs->nodes[k].parent==d->node){
                ssc_put(p,fs->nodes[k].ino);memcpy(p+4,fs->nodes[k].name,strlen(fs->nodes[k].name)+1);
                return 0;
            }
        }
        ssc_put(p+260,1);return 0;
    }
    return SSC_BADHANDLE;
}
static inline unsigned scr_by_path(struct ssc_scratch *fs,uint32_t sc,const struct ssc_inv *a,
                                   struct ssc_output *out,unsigned path_arg,bool rename)
{
    struct scr_path path;unsigned e=scr_parse(a->in[path_arg],&path);
    if(e)return e;
    const struct ssc_buf *in=a->in;
    if(rename){
        if(in[0].n!=12||ssc_word(in[0].p+4)!=in[1].n||ssc_word(in[0].p+8)!=in[2].n)return SSC_BADARG;
        struct scr_path dest;e=scr_parse(in[2],&dest);
        return e?e:scr_rename(fs,&path,&dest);
    }
    if(sc==0x30100 || sc==0x13050100){
        if(in[0].n!=4*(a->ni-1)||a->out[0]!=4)return SSC_BADARG;
        for(unsigned k=1;k<a->ni;k++)if(!ssc_string(in[k],ssc_word(in[0].p+4*(k-1))))return SSC_BADARG;
        unsigned token=0;e=scr_open(fs,&path,in[path_arg+1],&token);
        if(!e)ssc_put(ssc_output_buf(out,4),token);
        return e;
    }
    if(sc==0x1f020100){
        if(in[0].n!=8||ssc_word(in[0].p)!=31||ssc_word(in[0].p+4)!=in[1].n||a->out[0]!=96)return SSC_BADARG;
        unsigned idx;e=scr_lookup(fs,&path,path.count,&idx);
        if(e)return e;
        struct scr_node *n=&fs->nodes[idx];uint8_t *p=ssc_output_buf(out,96);
        ssc_put(p+16,n->ino);ssc_put(p+24,n->dir?0040700:0100600);
        ssc_put(p+28,n->dir?2:1);ssc_put(p+40,n->size);return 0;
    }
    if(in[0].n!=(sc==0x1d020000?8u:4u)||ssc_word(in[0].p)!=in[1].n)return SSC_BADARG;
    if(sc==SSC_OPENDIR)return scr_directory(fs,sc,a,out,&path);
    if(sc==0x1d020000){
        if(!path.count)return SCR_EXIST;
        unsigned idx;
        return scr_create(fs,&path,true,&idx);
    }
    return scr_unlink(fs,&path,sc==0x1e020000);
}
static inline unsigned scr_by_fd(struct ssc_scratch *fs,uint32_t sc,const struct ssc_inv *a,
                                 struct ssc_output *out,unsigned token)
{
    struct scr_fd *fd=NULL;
    for(unsigned i=0;i<SCR_FDS;i++)if(fs->fds[i].token==token)fd=&fs->fds[i];
    if(!fd)return SSC_BADHANDLE;
    struct scr_node *n=&fs->nodes[fd->node];
    unsigned count,e;uint8_t *prim,*data;
    switch(sc){
    case 0x02010000:case 0x17010000: /* fflush / fsync: RAM backend writes synchronously */
        return a->in[0].n==4?0:SSC_BADARG;
    case 0x03010000:
        if(a->in[0].n!=4)return SSC_BADARG;
        n->refs--;scr_reap(fs,fd->node);memset(fd,0,sizeof(*fd));return 0;
    case 0x04010200:
        if(!fd->read||a->in[0].n!=8||a->out[0]!=8||ssc_word(a->in[0].p+4)!=a->out[1])return SSC_BADARG;
        prim=ssc_output_buf(out,8);data=ssc_output_buf(out,a->out[1]);
        if(!prim||!data)return SSC_SMALL;
        count=fd->pos<n->size?n->size-fd->pos:0;if(count>a->out[1])count=a->out[1];
        scr_copy(fs,n,fd->pos,data,NULL,count);fd->pos+=count;fd->eof=count<a->out[1];
        ssc_put(prim,count);ssc_put(prim+4,fd->eof);return 0;
    case 0x05020100:
        if(!fd->write||a->in[0].n!=8||a->out[0]!=8||ssc_word(a->in[0].p+4)!=a->in[1].n)return SSC_BADARG;
        count=a->in[1].n;
        {unsigned pos=fd->append?n->size:fd->pos;
         if(count>SCR_BLOCKS*SCR_UNIT-pos)return SCR_NOSPACE;
         if(count && pos+count>n->size){e=scr_resize(fs,n,pos+count);if(e)return e;}
         scr_copy(fs,n,pos,NULL,a->in[1].p,count);fd->pos=pos+count;}
        prim=ssc_output_buf(out,8);ssc_put(prim,count);return 0;
    case 0x08010100:case 0x0c010100:case 0x0d010100: /* ftell, feof, ferror */
        if(a->in[0].n!=4||a->out[0]!=4)return SSC_BADARG;
        ssc_put(ssc_output_buf(out,4),sc==0x08010100?fd->pos:sc==0x0c010100?fd->eof:0);return 0;
    case 0x09010000: {
        if(a->in[0].n!=12)return SSC_BADARG;
        unsigned whence=ssc_word(a->in[0].p+8);if(whence>2)return SSC_BADARG;
        int64_t pos=whence==0?0:whence==1?fd->pos:n->size;
        pos+=(int32_t)ssc_word(a->in[0].p+4);
        if(pos<0 || pos>SCR_BLOCKS*SCR_UNIT)return SSC_BADARG;
        fd->pos=(unsigned)pos;fd->eof=false;return 0;}
    case 0x0a010100:
        if(a->in[0].n!=4||a->out[0]!=8)return SSC_BADARG;
        ssc_put(ssc_output_buf(out,8),n->size);return 0;
    case 0x0b010000:case 0x0e010000:
        if(a->in[0].n!=4)return SSC_BADARG;
        if(sc==0x0b010000)fd->pos=0;
        fd->eof=false;return 0;
    case 0x1f010000: /* extended ftrunc: opcode32, fd, target length */
        if(!fd->write||a->in[0].n!=12||ssc_word(a->in[0].p)!=32)return SSC_BADARG;
        if((int32_t)ssc_word(a->in[0].p+8)<0)return SSC_BADARG;
        return scr_resize(fs,n,ssc_word(a->in[0].p+8));
    default:return SSC_UNSUPPORTED;
    }
}
/* Called after module handle authentication and wire unpacking, before the
 * immutable filesystem. False passes the request to the next backend. */
static inline bool ssc_scratch_try(struct ssc_scratch *fs,uint32_t sc,const struct ssc_inv *a,
                                   struct ssc_output *out,unsigned *result)
{
    unsigned pa=0;bool rename=false;
    switch(sc){
    case 0x30100:case 0x18020000:case 0x1d020000:case 0x1e020000:
    case SSC_OPENDIR:case 0x1f020100:pa=1;break;
    case 0x13050100:pa=3;break;
    case 0x1f030000:
        if(a->in[0].n<4||ssc_word(a->in[0].p)!=33)return false;
        pa=1;rename=true;break;
    default:break;
    }
    if(pa){
        if(!scr_owned(a->in[pa]) && !(rename&&scr_owned(a->in[2])))return false;
        scr_init(fs);*result=scr_by_path(fs,sc,a,out,pa,rename);return true;
    }
    if(sc==SSC_READDIR||sc==SSC_CLOSEDIR){
        uint64_t t=0;if(a->in[0].n<8)return false;memcpy(&t,a->in[0].p,8);
        if((t&UINT64_C(0xffffffff00000000))!=SSC_SCR_DIR_PREFIX)return false;
        *result=a->in[0].n==8?scr_directory(fs,sc,a,out,NULL):SSC_BADARG;return true;
    }
    if(!a->ni || a->in[0].n<4)return false;
    unsigned token=ssc_word(a->in[0].p);
    if(sc==0x1f010000){
        if(token!=32 || a->in[0].n<8)return false;
        token=ssc_word(a->in[0].p+4);
    }
    if((token&0xff000000u)!=0x63000000u)return false;
    *result=scr_by_fd(fs,sc,a,out,token);return true;
}
#endif
