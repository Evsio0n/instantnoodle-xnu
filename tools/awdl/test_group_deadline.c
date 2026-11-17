#include <assert.h>
#include "GroupDeadline.h"
int main(void)
{
    assert(!hk_awdl_group_yield(2999999,1000000,true,false));
    assert(hk_awdl_group_yield(3000000,1000000,true,false));
    assert(!hk_awdl_group_yield(3000000,1000000,true,true));
    assert(!hk_awdl_group_yield(3000000,1000000,false,false));
    assert(!hk_awdl_group_yield(999999,1000000,true,false));
    assert(hk_awdl_group_yield(UINT64_MAX,UINT64_MAX-2000000,true,false));
    return 0;
}
