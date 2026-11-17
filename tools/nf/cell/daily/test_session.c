#include <assert.h>
#include "calibration_session.h"
int main(void){
 struct hk_als_completion r={.result='1',.version=1,.magic=0x534c4143,.boot_sec=100,.boot_usec=54321};
 assert(hk_als_completion_matches(&r,100,54321));assert(!hk_als_completion_matches(&r,99,54321));assert(!hk_als_completion_matches(&r,100,54322));
 r.result='0';assert(hk_als_completion_matches(&r,100,54321));r.result='2';assert(!hk_als_completion_matches(&r,100,54321));r.result='1';
 r.version=0;assert(!hk_als_completion_matches(&r,100,54321));r.version=1;r.reserved=1;assert(!hk_als_completion_matches(&r,100,54321));r.reserved=0;
 r.magic++;assert(!hk_als_completion_matches(&r,100,54321));r.magic--;r.reserved2=1;assert(!hk_als_completion_matches(&r,100,54321));r.reserved2=0;
 assert(!hk_als_completion_matches(&r,0,0));assert(!hk_als_completion_matches(0,100,54321));return 0;
}
