#include <stdbool.h>
#include <assert.h>
struct lx_radio {bool eligible[150];};
static bool auto_channel(struct lx_radio *r,unsigned ch,bool tx){
    assert(tx);return r->eligible[ch];
}
#include "social_visit.inc"
int main(void){
    struct lx_radio r={0};assert(!social_visit(&r,48));
    r.eligible[6]=true;r.eligible[44]=true;r.eligible[149]=true;
    assert(social_visit(&r,48)==6);assert(social_visit(&r,48)==149);
    assert(social_visit(&r,48)==44);assert(social_visit(&r,6)==149);
    r.eligible[149]=false;assert(social_visit(&r,6)==44);assert(social_visit(&r,6)==44);
    return 0;
}
