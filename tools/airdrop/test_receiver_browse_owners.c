#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "receiver_browse_owners.h"
int main(void){
 struct hk_receiver_publishers p={0};assert(!hk_receiver_count(&p));
 assert(!hk_receiver_add(&p,NULL));assert(!hk_receiver_remove(&p,NULL));
 for(uintptr_t i=1;i<=HK_RECEIVER_PUBLISHERS;i++)assert(hk_receiver_add(&p,(void*)i));
 assert(hk_receiver_count(&p)==HK_RECEIVER_PUBLISHERS);
 struct hk_receiver_publishers full=p;
 assert(!hk_receiver_add(&p,(void*)99));assert(!memcmp(&p,&full,sizeof(p)));
 assert(hk_receiver_add(&p,(void*)1));assert(hk_receiver_count(&p)==HK_RECEIVER_PUBLISHERS);
 assert(!hk_receiver_remove(&p,(void*)99));assert(!memcmp(&p,&full,sizeof(p)));
 assert(hk_receiver_remove(&p,(void*)4));assert(!hk_receiver_remove(&p,(void*)4));
 assert(hk_receiver_add(&p,(void*)99));
 for(uintptr_t i=1;i<=HK_RECEIVER_PUBLISHERS;i++)hk_receiver_remove(&p,(void*)i);
 assert(hk_receiver_count(&p)==1);assert(hk_receiver_remove(&p,(void*)99));assert(!hk_receiver_count(&p));
 return 0;
}
