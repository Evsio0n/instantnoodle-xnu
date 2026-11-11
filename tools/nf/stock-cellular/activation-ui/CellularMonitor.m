#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <errno.h>
#include "hk_cellular_observation.h"
#ifdef HK_SHOW_CELLULAR_HISTORY
#include "history/hk_cellular_history.h"
#endif
#ifdef HK_SHOW_CELLULAR_SELECTION
#include "selection/hk_cellular_selection.h"
#endif

static NSString *const Unknown=@"Unknown";
static NSString *raw(NSString *label,unsigned n) { return [NSString stringWithFormat:@"%@ (%u)",label,n]; }
static NSString *choice(unsigned n,NSArray<NSString *> *items) {
    return n<items.count?items[n]:raw(@"Unknown",n);
}
static NSString *age(uint64_t now,uint64_t when) {
    if(!when || when>now)return @"age unknown";
    uint64_t sec=(now-when)/1000;
    return [NSString stringWithFormat:@"%llu s ago",(unsigned long long)sec];
}
static BOOL valid(const struct hk_cellular_observation *s,unsigned bit,uint64_t stamp) {
    return s->phase==HK_OBS_LIVE && (s->valid_mask&bit)==bit && stamp && stamp<=s->now_ms;
}
static NSString *appState(unsigned n) {
    return choice(n,@[@"unknown",@"detected",@"PIN required",@"PUK required",@"personalization required",@"PIN blocked",@"illegal",@"ready"]);
}
static NSString *states(uint32_t mask) {
    if(!mask)return @"none reported";
    NSMutableArray *list=[NSMutableArray array];
    for(unsigned i=0;i<32;i++)if(mask&(1u<<i))[list addObject:appState(i)];
    return [list componentsJoinedByString:@", "];
}
static NSString *withAge(NSString *value,const struct hk_cellular_observation *s,uint64_t t) {
    return [NSString stringWithFormat:@"%@ · %@",value,age(s->now_ms,t)];
}

#ifdef HK_SHOW_CELLULAR_SELECTION
static NSString *selectionValue(const struct hk_selection_report *report, uint64_t now) {
    if (!report->checked_ms || report->checked_ms > now)
        return Unknown;
    NSString *value = Unknown;
    if (report->status == HK_SELECTION_INDICATION_ERROR) {
        value = [NSString stringWithFormat:@"selection unknown (modem error %u)", report->indication_error];
    } else if (report->status == HK_SELECTION_COMPLETE) {
        NSString *active = report->active_present ? @"active profile" : @"no active profile reported";
        NSString *pending = report->pending_present ? @"pending profile" : @"no pending profile reported";
        value = [NSString stringWithFormat:@"%@, %@", active, pending];
    }
    return [NSString stringWithFormat:@"%@ · checked %@", value, age(now, report->checked_ms)];
}
#endif

@interface CellularController:UIViewController
@property(nonatomic,strong) UILabel *phase,*sample,*sim,*gw,*activation,*radio;
@property(nonatomic,strong) NSTimer *timer;
@property(nonatomic) BOOL reading;
#ifdef HK_SHOW_CELLULAR_HISTORY
@property(nonatomic,strong) UIStackView *liveRows,*historyRows;
@property(nonatomic,strong) UILabel *historyStamp,*historyCard,*historyBinding,*historyRadio,*historyOnline,*historyFault;
#endif
#ifdef HK_SHOW_CELLULAR_SELECTION
@property(nonatomic,strong) UILabel *liveConfiguration, *historyConfiguration;
#endif
@end

@implementation CellularController
- (UILabel *)label:(CGFloat)size weight:(UIFontWeight)weight color:(UIColor *)color {
    UILabel *v=[[UILabel alloc] init];v.font=[UIFont systemFontOfSize:size weight:weight];
    v.numberOfLines=0;v.textColor=color;return v;
}
- (UILabel *)section:(NSString *)title in:(UIStackView *)stack {
    UILabel *heading=[self label:15 weight:UIFontWeightSemibold color:UIColor.labelColor];
    heading.text=title;[stack addArrangedSubview:heading];[stack setCustomSpacing:2 afterView:heading];
    UILabel *value=[self label:14 weight:UIFontWeightRegular color:UIColor.secondaryLabelColor];
    value.text=Unknown;[stack addArrangedSubview:value];return value;
}
- (void)viewDidLoad {
    [super viewDidLoad];self.view.backgroundColor=UIColor.systemBackgroundColor;
    UIScrollView *scroll=[[UIScrollView alloc] init];scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:scroll];
    UIStackView *stack=[[UIStackView alloc] init];stack.axis=UILayoutConstraintAxisVertical;
    stack.spacing=11;stack.translatesAutoresizingMaskIntoConstraints=NO;[scroll addSubview:stack];
    UILabel *title=[self label:29 weight:UIFontWeightBold color:UIColor.labelColor];
    title.text=@"Cellular Monitor";[stack addArrangedSubview:title];
    self.phase=[self label:18 weight:UIFontWeightSemibold color:UIColor.systemBlueColor];
    self.phase.text=@"Waiting for modem";[stack addArrangedSubview:self.phase];
    self.sample=[self label:13 weight:UIFontWeightRegular color:UIColor.secondaryLabelColor];
    self.sample.text=@"Sample unavailable";[stack addArrangedSubview:self.sample];
    UIStackView *liveStack=stack;
#ifdef HK_SHOW_CELLULAR_HISTORY
    self.liveRows=[[UIStackView alloc] init];self.liveRows.axis=UILayoutConstraintAxisVertical;self.liveRows.spacing=9;
    [stack addArrangedSubview:self.liveRows];liveStack=self.liveRows;
#endif
    self.sim=[self section:@"SIM and USIM" in:liveStack];
    self.gw=[self section:@"Primary GW" in:liveStack];
    self.activation=[self section:@"Activation" in:liveStack];
    self.radio=[self section:@"Radio and data" in:liveStack];
#ifdef HK_SHOW_CELLULAR_SELECTION
    self.liveConfiguration = [self section:@"Carrier configuration · last report" in:liveStack];
#endif
#ifdef HK_SHOW_CELLULAR_HISTORY
    self.historyRows=[[UIStackView alloc] init];self.historyRows.axis=UILayoutConstraintAxisVertical;self.historyRows.spacing=8;
    [stack addArrangedSubview:self.historyRows];
    self.historyStamp=[self section:@"Last valid this boot" in:self.historyRows];
    self.historyCard=[self section:@"SIM and USIM" in:self.historyRows];
    self.historyBinding=[self section:@"Primary GW" in:self.historyRows];
    self.historyRadio=[self section:@"Radio and data" in:self.historyRows];
#ifdef HK_SHOW_CELLULAR_SELECTION
    self.historyConfiguration = [self section:@"Carrier configuration · last report" in:self.historyRows];
#endif
    self.historyOnline=[self section:@"Last DMS ONLINE request" in:stack];
    self.historyFault=[self section:@"Fault / stop" in:stack];
#endif
    UIButton *settings=[UIButton buttonWithType:UIButtonTypeSystem];
    [settings setTitle:@"Open Settings" forState:UIControlStateNormal];
    [settings addTarget:self action:@selector(openSettings) forControlEvents:UIControlEventTouchUpInside];
    [settings.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;[stack addArrangedSubview:settings];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-12],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:18],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-18],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-36]]];
}
#ifdef HK_SHOW_CELLULAR_HISTORY
#include "history/HistoryView.inc"
#endif
- (void)openSettings {
    if(!self.view.window || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
    NSURL *target=[NSURL URLWithString:UIApplicationOpenSettingsURLString];
    if(target)[UIApplication.sharedApplication openURL:target options:@{} completionHandler:nil];
}
- (void)render:(const struct hk_cellular_observation *)s available:(BOOL)available {
    if(!available || s->phase!=HK_OBS_LIVE) {
        self.phase.text=available && s->phase==HK_OBS_STOPPED?@"Modem stopped":@"Waiting for modem";
        self.sample.text=available?[NSString stringWithFormat:@"Sample %u · current fields unavailable",s->sequence]:@"Sample unavailable";
        self.sim.text=Unknown;self.gw.text=Unknown;self.activation.text=Unknown;self.radio.text=Unknown;return;
    }
    self.phase.text=@"Live modem";self.sample.text=[NSString stringWithFormat:@"Sample %u · UIM generation %u",s->sequence,s->uim_generation];
    NSString *physical=Unknown,*card=Unknown,*apps=Unknown,*atr=Unknown;
    if(valid(s,HK_OBS_PHYSICAL,s->physical_ms)){
        NSString *slot=s->physical_slot&&s->logical_slot?[NSString stringWithFormat:@"slot %u → logical %u",s->physical_slot,s->logical_slot]:@"slot mapping unknown";
        physical=withAge([NSString stringWithFormat:@"%@, %@",slot,s->card_present?@"present":@"absent"],s,s->physical_ms);
    }
    if(valid(s,HK_OBS_CARD,s->card_ms)){
        card=withAge(choice(s->card_state,@[@"absent",@"present",@"error",@"unknown"]),s,s->card_ms);
        apps=[NSString stringWithFormat:@"%u apps · SIM %@ · USIM %@",s->apps,states(s->sim_states),states(s->usim_states)];
    }
    if(valid(s,HK_OBS_ATR,s->atr_ms))atr=withAge(s->atr_present?@"present":@"absent",s,s->atr_ms);
    self.sim.text=[NSString stringWithFormat:@"%@\nCard %@ · ATR %@\n%@",physical,card,atr,apps];
    if(valid(s,HK_OBS_GW,s->card_ms)){
        NSString *gw=choice(s->gw_status,@[@"Unassigned",@"Mapped",@"Invalid mapping",@"Outside reported range"]);
        if(s->gw_status==1 && s->gw_slot && s->gw_app)
            gw=[NSString stringWithFormat:@"Mapped · slot %u, app %u · %@ %@",s->gw_slot,s->gw_app,choice(s->gw_type,@[@"unknown",@"SIM",@"USIM",@"RUIM",@"CSIM",@"ISIM"]),appState(s->gw_state)];
        self.gw.text=withAge(gw,s,s->card_ms);
    }else self.gw.text=Unknown;
    NSString *decision=choice(s->activation,@[@"Not submitted",@"Attempt uncertain",@"Submitted",@"Modem accepted",@"Modem refused",@"Already bound",@"Verified by readback"]);
    if(s->activation==HK_OBS_READBACK_BOUND && (!s->verified || !s->post_reads))decision=@"Readback unconfirmed";
    if(s->activation==HK_OBS_MODEM_ACK && (!s->ack_seen || s->ack_result))decision=@"Acknowledgement unconfirmed";
    if(s->activation==HK_OBS_MODEM_ERROR && s->ack_seen)
        decision=[NSString stringWithFormat:@"Modem refused · result %u, error %u",s->ack_result,s->ack_error];
    NSString *ack=s->ack_seen?[NSString stringWithFormat:@"ACK %u/%u",s->ack_result,s->ack_error]:@"No ACK";
    NSString *verified=(s->verified && s->post_reads)?[NSString stringWithFormat:@"verified · %@",age(s->now_ms,s->verified_ms)]:@"not verified";
    self.activation.text=[NSString stringWithFormat:@"%@\nAttempt %u · sent %u · %@\n%u independent reads · %@",decision,s->attempted,s->submitted,ack,s->post_reads,verified];
    NSString *dms=Unknown,*nas=Unknown,*wds=Unknown;
    if(valid(s,HK_OBS_DMS,s->dms_ms))
        dms=withAge(choice(s->dms_mode,@[@"online",@"low power",@"factory test",@"offline",@"resetting",@"shutting down",@"persistent low power",@"mode-only low power"]),s,s->dms_ms);
    if(valid(s,HK_OBS_NAS,s->nas_ms)){
        NSString *reg=choice(s->nas_registration,@[@"unregistered",@"registered",@"searching",@"denied",@"unknown"]);
        NSArray *attach=@[@"unknown",@"attached",@"detached"];
        nas=withAge([NSString stringWithFormat:@"%@ · CS %@ · PS %@",reg,choice(s->cs_attach,attach),choice(s->ps_attach,attach)],s,s->nas_ms);
    }
    if(valid(s,HK_OBS_WDS,s->wds_ms))
        wds=withAge(choice(s->wds_state,@[@"unknown",@"disconnected",@"connected",@"suspended",@"authenticating"]),s,s->wds_ms);
    self.radio.text=[NSString stringWithFormat:@"DMS %@\nNAS %@\nWDS %@",dms,nas,wds];
}
- (void)refresh {
    if(self.reading)return;self.reading=YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        struct hk_cellular_observation s={0};
        size_t n=sizeof(s);
        BOOL ok=sysctlbyname(HK_CELLULAR_OBSERVATION_NAME,&s,&n,NULL,0)==0 && n==sizeof(s) &&
                s.version==HK_CELLULAR_OBSERVATION_VERSION && s.size==sizeof(s) && s.phase<=HK_OBS_STOPPED;
#ifdef HK_SHOW_CELLULAR_HISTORY
        struct hk_cellular_history h={0};size_t hsize=sizeof(h);
        BOOL historyOK=sysctlbyname(HK_CELLULAR_HISTORY_NAME,&h,&hsize,NULL,0)==0 &&
            hsize==sizeof(h) && h.size==sizeof(h) && h.version==HK_CELLULAR_HISTORY_VERSION;
        BOOL historyStopped=historyOK && h.phase==HK_OBS_STOPPED;
#ifdef HK_SHOW_CELLULAR_SELECTION
        struct hk_cellular_selection config = {0};
        size_t configSize = sizeof(config);
        BOOL configOK = sysctlbyname(HK_CELLULAR_SELECTION_NAME, &config, &configSize, NULL, 0) == 0 &&
            configSize == sizeof(config) && config.size == sizeof(config) &&
            config.version == HK_CELLULAR_SELECTION_VERSION;
#endif
        dispatch_async(dispatch_get_main_queue(), ^{
            self.reading=NO;
            BOOL live=ok && s.phase==HK_OBS_LIVE && !historyStopped;
            [self render:&s available:ok && !historyStopped];
            if(!live){
                self.phase.text=(historyStopped || (ok && s.phase==HK_OBS_STOPPED))?@"Modem stopped":@"Waiting for modem";
                self.sample.text=@"Current readings: Unknown";
            }
            [self renderHistory:&h available:historyOK live:live];
#ifdef HK_SHOW_CELLULAR_SELECTION
            UILabel *label = live ? self.liveConfiguration : self.historyConfiguration;
            BOOL samePhase = live ? config.phase == HK_OBS_LIVE :
                (historyStopped && config.phase == HK_OBS_STOPPED);
            label.text = Unknown;
            if (configOK && samePhase) {
                label.text = [NSString stringWithFormat:@"Software (index 0): %@\nPlatform: %@",
                              selectionValue(&config.software_sub0, config.now_ms),
                              selectionValue(&config.platform, config.now_ms)];
            }
#endif
        });
#else
        dispatch_async(dispatch_get_main_queue(), ^{self.reading=NO;[self render:&s available:ok];});
#endif
    });
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];[self refresh];
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *unused){(void)unused;[self refresh];}];
}
- (void)viewDidDisappear:(BOOL)animated {
    [self.timer invalidate];self.timer=nil;[super viewDidDisappear:animated];
}
@end

@interface CellularDelegate:UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation CellularDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    (void)app;(void)opts;self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[[CellularController alloc] init];[self.window makeKeyAndVisible];return YES;
}
@end
int main(int argc,char **argv) {
    @autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(CellularDelegate.class));}
}
