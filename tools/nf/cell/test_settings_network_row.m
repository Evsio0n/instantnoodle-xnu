/* Host regression for the app-independent list assembly and live public-state projection. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stddef.h>
#include <string.h>
#include <sys/sysctl.h>

static int sim,reg,up;
static const char *operator_value;
static int fixture_sysctl(const char *key, void *out, size_t *len, void *newp, size_t newlen) {
    if (newp || newlen) return -1;
    if (!strcmp(key,"kern.hackios.cellular_operator")) {
        if (!operator_value || *len<strlen(operator_value)+1)return -1;
        strcpy(out,operator_value);*len=strlen(operator_value)+1;return 0;
    }
    if (*len<sizeof(int))return -1;
    *(int *)out=!strcmp(key,"kern.hackios.cellular_sim_state")?sim:!strcmp(key,"kern.hackios.cellular_registered")?reg:up;
    *len=sizeof(int);return 0;
}
static int fixture_open(const char *file,int flags,...){(void)file;(void)flags;return -1;}
#include <fcntl.h>
#define open fixture_open
#define sysctlbyname fixture_sysctl
#import "../../../userland/src/settings_cellular_network.m"
#undef sysctlbyname
#undef open

@interface PSSpecifier : NSObject<HKSpecifier>
@property(nonatomic) NSInteger cellType;
@property(nonatomic,copy) NSString *identifier;
@property(nonatomic,weak) id target;
@property(nonatomic) SEL getter,setter;
@property(nonatomic,strong) NSString *name;
@end
@implementation PSSpecifier
+(id)preferenceSpecifierNamed:(NSString *)name target:(id)target set:(SEL)set get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit {
    PSSpecifier *s=[self new];s.target=target;s.getter=get;s.setter=set;s.cellType=cell;s.name=name;return s;
}
+(id)emptyGroupSpecifier {return [self new];}
-(void)setProperty:(id)value forKey:(NSString *)key {if([key isEqualToString:@"id"])self.identifier=value;}
@end
@interface FakeList:NSObject<HKCellularList>
@property (nonatomic,strong) NSMutableArray *list;
@property (nonatomic) unsigned reloads;
@end
@implementation FakeList
-(id)specifierForID:(NSString *)identifier {for(PSSpecifier *s in self.list) if([s.identifier isEqualToString:identifier])return s;return nil;}
-(void)reloadSpecifier:(id)spec animated:(BOOL)animated {self.reloads++;}
@end
static unsigned originals;
static void take_list(id owner,SEL ignored,NSMutableArray *a){originals++;((FakeList *)owner).list=a;}
#define CHECK(a) do{if(!(a)){fprintf(stderr,"failed at line %d\n",__LINE__);return 1;}tested++;}while(0)
int main(void){@autoreleasepool{
    unsigned tested=0;originalFinishList=take_list;
    FakeList *owner=[FakeList new];NSMutableArray *a=[NSMutableArray arrayWithObjects:[PSSpecifier emptyGroupSpecifier],[PSSpecifier preferenceSpecifierNamed:@"Data" target:nil set:NULL get:NULL detail:Nil cell:6 edit:Nil],nil];
    finishList(owner,NULL,a);PSSpecifier *row=owner.list[1];
    CHECK(originals==1 && row.cellType==4 && row.setter==NULL && row.getter==@selector(networkValue:));
    CHECK([row.name isEqualToString:@"Network"]);CHECK([row.identifier isEqualToString:@"HKServingNetwork"]);
    CHECK([owner.list[2] name] && [[owner.list[2] name] isEqualToString:@"Data"]);
    finishList(owner,NULL,a);CHECK(owner.list.count==3 && originals==2);
    HKCellularNetworkRow *provider=row.target;CHECK(provider!=nil);
    sim=3;reg=1;up=1;operator_value="Fixture operator A";CHECK([[provider networkValue:row] isEqual:@"Fixture operator A"]);
    reg=0;CHECK([[provider networkValue:row] isEqual:@"Unknown"]);
    sim=1;CHECK([[provider networkValue:row] isEqual:@"No SIM"]);
    sim=0;CHECK([[provider networkValue:row] isEqual:@"Unknown"]);
    sim=3;reg=1;up=0;CHECK([[provider networkValue:row] isEqual:@"Unknown"]);up=1;operator_value=NULL;CHECK([[provider networkValue:row] isEqual:@"Unknown"]);
    operator_value="Fixture operator B";CHECK([[provider networkValue:row] isEqual:@"Fixture operator B"]);
    NSMutableArray *empty=[NSMutableArray array];finishList(owner,NULL,empty);CHECK(empty.count==2 && [empty[0] cellType]==0 && [empty[1] cellType]==4);
    printf("%u native row assertions passed\n",tested);return 0;
}}
