#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define EG(x) (x)
struct ieee80211_hw { unsigned id; };
struct ieee80211_vif { unsigned id; };
struct lx_radio { struct ieee80211_hw hw;struct ieee80211_vif *vif; };
static struct { struct ieee80211_hw *hw;unsigned active,start,start_status,stop,vdev; } echo;
static unsigned lx_awdl_roc_vdev(struct ieee80211_vif *v) { return v->id; }
#include "social_vif.inc"
int main(void) {
    struct ieee80211_vif primary={0},secondary={1},other={2};
    struct lx_radio r={.vif=&primary},foreign={0};
    assert(auto_social_vif(&r,&secondary)==&primary);
    echo.hw=&r.hw;echo.active=1;echo.start=1;echo.vdev=1;
    assert(auto_social_vif(&r,&secondary)==&secondary);
    echo.start_status=1;assert(auto_social_vif(&r,&secondary)==&primary);echo.start_status=0;
    echo.stop=1;assert(auto_social_vif(&r,&secondary)==&primary);echo.stop=0;
    echo.hw=&foreign.hw;assert(auto_social_vif(&r,&secondary)==&primary);echo.hw=&r.hw;
    assert(auto_social_vif(&r,&other)==&primary);
    assert(auto_social_vif(&r,0)==&primary);
    echo.start=0;assert(auto_social_vif(&r,&secondary)==&primary);
    return 0;
}
