#ifndef HK_RECEIVER_BROWSE_OWNERS_H
#define HK_RECEIVER_BROWSE_OWNERS_H
#include <stdbool.h>
#include <stddef.h>
#define HK_RECEIVER_PUBLISHERS 8
struct hk_receiver_publishers {const void *refs[HK_RECEIVER_PUBLISHERS];};
static inline unsigned hk_receiver_count(const struct hk_receiver_publishers *p){
 unsigned n=0;for(unsigned i=0;i<HK_RECEIVER_PUBLISHERS;i++)n+=p->refs[i]!=NULL;return n;
}
static inline bool hk_receiver_add(struct hk_receiver_publishers *p,const void *ref){
 if(!ref)return false;
 for(unsigned i=0;i<HK_RECEIVER_PUBLISHERS;i++)if(p->refs[i]==ref)return true;
 for(unsigned i=0;i<HK_RECEIVER_PUBLISHERS;i++)if(!p->refs[i]){p->refs[i]=ref;return true;}
 return false;
}
static inline bool hk_receiver_remove(struct hk_receiver_publishers *p,const void *ref){
 for(unsigned i=0;i<HK_RECEIVER_PUBLISHERS;i++)if(ref && p->refs[i]==ref){p->refs[i]=NULL;return true;}return false;
}
#endif
