#include <assert.h>
#include "UnicastHold.h"
int main(void){
 /* Reproduce periodic 3s discovery after a native connection: the native
  * hold expires at10s despite repeated real DNS renewals through18s. */
 uint64_t hold=hk_awdl_unicast_hold(0,1000000,true,false);
 for(uint64_t now=4000000;now<=19000000;now+=3000000){
  hold=hk_awdl_unicast_hold(hold,now,true,true);assert(hold==11000000);
  if(now>=11000000)assert(now>=hold);
 }
 assert(hk_awdl_unicast_hold(hold,20000000,false,false)==hold);
 assert(hk_awdl_unicast_hold(hold,20000000,true,false)==30000000);
 assert(hk_awdl_unicast_hold(0,20000000,true,true)==0);
 return 0;
}
