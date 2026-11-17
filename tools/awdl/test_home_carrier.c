#include <assert.h>
#include "HomeCarrier.h"
int main(void)
{
    uint8_t slots[16]={48,0,149,0,0,149,48,48,6,0,149,0,0,149,48,48};
    assert(hk_awdl_home_carrier(slots,48,true)==48);
    assert(hk_awdl_home_carrier(slots,149,true)==149);
    assert(hk_awdl_home_carrier(slots,44,true)==0);
    assert(hk_awdl_home_carrier(slots,48,false)==0);
    assert(hk_awdl_home_carrier(slots,0,true)==0);
    assert(hk_awdl_home_carrier(0,48,true)==0);
    for(unsigned i=0;i<16;i++)slots[i]=149;
    assert(hk_awdl_home_carrier(slots,48,true)==0);
    slots[15]=44;assert(hk_awdl_home_carrier(slots,44,true)==44);
    return 0;
}
