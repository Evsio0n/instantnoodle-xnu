#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <notify.h>
#include <unistd.h>
#include <string.h>
#include "../../tools/nf/modem/mdm_radio_status.h"

#import <CoreLocation/CoreLocation.h>
#include <math.h>
/* Core Location delivers delegates on the run loop where the manager is created.
 * A service that stops replying must never hold UIKit's first frame or touch events. */
// Only this signed process publishes its own lifecycle and user action stages.
static dispatch_queue_t telemetryQueue(void) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{queue=dispatch_queue_create("com.hackios.gnss-monitor.observations", DISPATCH_QUEUE_SERIAL);});
    return queue;
}
static void markStage(unsigned stage) {
    if(stage>=32)return;
    dispatch_async(telemetryQueue(), ^{
        static int token; static dispatch_once_t once; static uint32_t stages;
        dispatch_once(&once, ^{if(notify_register_check("com.hackios.gnss-monitor.lifecycle",&token)!=NOTIFY_STATUS_OK)token=-1;});
        stages |= (uint32_t)1 << stage;
        if(token>=0)notify_set_state(token,((uint64_t)(uint32_t)getpid()<<32)|stages);
    });
}
static NSString *const workerNotification = @"GNSSMonitorLocationEvent";
static void locationEvent(NSString *event, NSNumber *state, NSString *message) {
    NSDictionary *info=@{@"event":event, @"state":state ?: @(-1), @"message":message ?: @""};
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:workerNotification object:nil userInfo:info];
    });
}
@interface GNSSLocationWorker : NSObject <CLLocationManagerDelegate>
@property(nonatomic,retain) CLLocationManager *manager;
@property(atomic,assign) BOOL visible;
@property(atomic,assign) NSUInteger consentGeneration;
@property(atomic,assign) NSUInteger positionGeneration;
@property(nonatomic,assign) BOOL waitingPosition;
- (void)serve;
- (void)permission:(NSDictionary *)action;
- (void)position:(NSDictionary *)action;
@end
@implementation GNSSLocationWorker
- (void)serve {
    @autoreleasepool {
        NSPort *port=[NSMachPort port];
        [NSRunLoop.currentRunLoop addPort:port forMode:NSDefaultRunLoopMode];
        while (!NSThread.currentThread.cancelled) {
            @autoreleasepool { [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:1]]; }
        }
        self.manager.delegate=nil;
        self.manager=nil;
    }
}
- (BOOL)valid:(NSDictionary *)a generation:(NSUInteger)generation {
    return self.visible && [a[@"generation"] unsignedIntegerValue]==generation &&
           NSProcessInfo.processInfo.systemUptime <= [a[@"expires"] doubleValue];
}
- (void)prepare {
    if (self.manager) return;
    markStage(5);locationEvent(@"init-enter",nil,nil);
    self.manager=[[[CLLocationManager alloc] init] autorelease];
    markStage(6);locationEvent(@"init-return",nil,nil);
    self.manager.delegate=self;
}
- (void)permission:(NSDictionary *)action {
    if (![self valid:action generation:self.consentGeneration]) {locationEvent(@"permission-done",nil,nil);return;}
    [self prepare];
    if (![self valid:action generation:self.consentGeneration]) {
        locationEvent(@"permission-done",nil,nil);return;
    }
    // iOS owns the current permission decision. The public request is a
    // no-op once determined. A separate status-read IPC must never gate it.
    markStage(9);locationEvent(@"request-enter",nil,nil);
    [self.manager requestWhenInUseAuthorization];
    markStage(10);locationEvent(@"request-return",nil,nil);
    locationEvent(@"permission-done",nil,nil);
}
- (void)position:(NSDictionary *)action {
    if (![self valid:action generation:self.positionGeneration]) {locationEvent(@"position-done",nil,nil);return;}
    [self prepare];
    CLAuthorizationStatus state=self.manager.authorizationStatus;
    markStage(8);locationEvent(@"status",@(state),nil);
    if ((state!=kCLAuthorizationStatusAuthorizedWhenInUse && state!=kCLAuthorizationStatusAuthorizedAlways) ||
        ![self valid:action generation:self.positionGeneration]) {
        locationEvent(@"position-done",@(state),@"請先使用「申請定位權限」。"); return;
    }
    self.waitingPosition=YES;
    self.manager.desiredAccuracy=kCLLocationAccuracyBest;
    NSLog(@"GNSSMonitor: authorized public requestLocation issued");
    [self.manager requestLocation];
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)manager {
    markStage(11);locationEvent(@"delegate-enter",nil,nil);
    locationEvent(@"callback-arrived",nil,nil);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        markStage(7);locationEvent(@"status-enter",nil,nil);
        CLAuthorizationStatus state=manager.authorizationStatus;
        markStage(8);locationEvent(@"callback",@(state),nil);
    });
}
- (void)locationManager:(CLLocationManager *)manager didFailWithError:(NSError *)error {
    (void)manager;
    if(self.waitingPosition) {
        self.waitingPosition=NO;
        locationEvent(@"position-done",nil,[NSString stringWithFormat:@"系統定位未取得結果（%ld）",(long)error.code]);
    }
}
- (void)locationManager:(CLLocationManager *)manager didUpdateLocations:(NSArray<CLLocation *> *)locations {
    if(!self.waitingPosition) return;
    self.waitingPosition=NO;
    CLAuthorizationStatus state=manager.authorizationStatus;
    if(!self.visible || (state!=kCLAuthorizationStatusAuthorizedAlways && state!=kCLAuthorizationStatusAuthorizedWhenInUse)) {
        locationEvent(@"position-done",@(state),@"驗證已停止");return;
    }
    CLLocation *fix=locations.lastObject;
    NSTimeInterval age=fix ? -fix.timestamp.timeIntervalSinceNow : INFINITY;
    if(!fix || !CLLocationCoordinate2DIsValid(fix.coordinate) || !isfinite(fix.horizontalAccuracy) ||
       fix.horizontalAccuracy<0 || !isfinite(age) || age < -2 || age > 15 || fix.sourceInformation.isSimulatedBySoftware) {
        locationEvent(@"position-done",@(state),@"回覆過期、無效或標示為模擬定位");return;
    }
    NSLog(@"GNSSMonitor: fresh public CLLocation reply; satellite source not established");
    locationEvent(@"position-done",@(state),[NSString stringWithFormat:@"系統位置 · 精度 %.0f 公尺 · %.1f 秒前",fix.horizontalAccuracy,MAX(0,age)]);
}
- (void)dealloc { [_manager release];[super dealloc]; }
@end

@interface GNSSMonitorController : UIViewController
@property(nonatomic,retain) GNSSLocationWorker *worker;
@property(nonatomic,retain) NSThread *locationThread;
@property(nonatomic,retain) UILabel *clientLabel,*permissionLabel,*radioHeading,*statusLabel,*detailLabel;
@property(nonatomic,retain) NSTimer *timer;
@property(nonatomic,retain) UIButton *permissionButton;
@property(nonatomic,copy) NSString *phase;
@property(nonatomic,assign) NSInteger authState,serviceState;
@property(nonatomic,assign) BOOL permissionPending,permissionInFlight,positionPending,serviceQueryPending,radioPending;
@property(nonatomic,assign) unsigned permissionAttempts,permissionCalls,permissionCallbacks;
@end

static NSString *radioField(struct hk_radio_field f, unsigned kind, uint64_t now)
{
    switch (f.state) {
        case HK_RADIO_ABSENT:return @"等待服務";
        case HK_RADIO_ENDPOINT:return @"已發現，等待查詢";
        case HK_RADIO_QUERYING:return @"查詢中";
        case HK_RADIO_TRANSPORT_FAILURE:return [NSString stringWithFormat:@"傳輸失敗 (%d)",f.error];
        case HK_RADIO_QMI_FAILURE:return [NSString stringWithFormat:@"數據機回報錯誤 (%d)",f.error];
        case HK_RADIO_NO_FIELD:return @"回覆缺少欄位";
        case HK_RADIO_VALUE:break;
        default:return @"未知狀態";
    }
    NSString *value=nil;
    if (kind==HK_RADIO_MODE) {
        NSArray *modes=@[@"線上",@"低功耗",@"工廠測試",@"離線",@"重設中",@"關機中",@"持續低功耗",@"模式限定"];
        if(f.value>=0 && (NSUInteger)f.value<[modes count]) value=[modes objectAtIndex:(NSUInteger)f.value];
        if (value && (f.detail & HK_RADIO_REASON_PRESENT))
            value=[value stringByAppendingFormat:@" · 離線原因 0x%04x", (unsigned)f.detail & 0xffff];
        if (value && (f.detail & HK_RADIO_RESTRICTION_PRESENT))
            value=[value stringByAppendingString:(f.detail & HK_RADIO_RESTRICTED) ? @" · 硬體受限" : @" · 硬體未回報限制"];
    } else if (kind==HK_RADIO_PACKET) {
        NSArray *modes=@[@"已中斷",@"已連接",@"暫停",@"驗證中"];
        if(f.value>=1 && f.value<=4)value=[modes objectAtIndex:(NSUInteger)f.value-1];
    } else if(kind==HK_RADIO_SIGNAL) {
        NSDictionary *rat=@{@1:@"CDMA 1x",@2:@"EV-DO",@4:@"GSM",@5:@"UMTS",@8:@"LTE",@12:@"NR"};
        NSString *iface=[rat objectForKey:@(f.detail)] ?: [NSString stringWithFormat:@"介面 %d",f.detail];
        value=f.detail==0 ? @"無無線介面" : [NSString stringWithFormat:@"%d dBm · %@",f.value,iface];
    }
    if (!value)value=[NSString stringWithFormat:@"代碼 %d",f.value];
    if(now<f.observed_ms)return @"樣本時間無效";
    return [NSString stringWithFormat:@"%@（%llu 秒前）",value,
            (unsigned long long)((now-f.observed_ms)/1000)];
}

static NSString *radioStatus(void)
{
    struct hk_radio_snapshot s={0};
    size_t n=sizeof(s);
    if(sysctlbyname("kern.hackios.modem_radio_status",&s,&n,NULL,0)!=0)
        return @"此核心未提供行動網路狀態";
    if(n!=sizeof(s)||s.size!=sizeof(s)||s.version!=HK_RADIO_VERSION)return @"狀態格式不相容";
    switch(s.phase) {
        case HK_RADIO_OFF:return @"此輪未啟用數據機查詢";
        case HK_RADIO_STARTING:return @"數據機啟動中";
        case HK_RADIO_INIT_FAILURE:return @"數據機查詢服務啟動失敗";
        case HK_RADIO_STOPPED:return @"數據機查詢已停止";
        case HK_RADIO_ACTIVE:break;
        default:return @"未知階段";
    }
    return [NSString stringWithFormat:@"運作模式　%@\n無線訊號　%@\n封包服務　%@",
            radioField(s.fields[HK_RADIO_MODE],HK_RADIO_MODE,s.read_ms),
            radioField(s.fields[HK_RADIO_SIGNAL],HK_RADIO_SIGNAL,s.read_ms),
            radioField(s.fields[HK_RADIO_PACKET],HK_RADIO_PACKET,s.read_ms)];
}

static NSString *gnssStatus(void)
{
    char buf[768]={0};size_t size=sizeof(buf);
    if(sysctlbyname("kern.hackios.modem_gnss",buf,&size,NULL,0)!=0)return @"此核心未提供 GNSS 狀態";
    if(!size || size>sizeof(buf) || buf[size-1])return @"GNSS 狀態格式不相容";
    NSString *raw=[NSString stringWithUTF8String:buf];
    NSMutableDictionary *fields=[NSMutableDictionary dictionary];
    for(NSString *part in [raw componentsSeparatedByString:@" "]){
        NSArray *pair=[part componentsSeparatedByString:@"="];
        if([pair count]==2)[fields setObject:[pair objectAtIndex:1] forKey:[pair objectAtIndex:0]];
    }
    if([fields[@"enabled"] isEqual:@"0"])return @"此輪未啟用衛星定位";
    NSString *fixes=fields[@"fixes"], *reports=fields[@"position_reports"];
    if(!fixes || !reports)return @"尚未取得定位回報";
    NSString *fix=[fixes isEqual:@"0"]?@"本次尚未取得有效定位":
        [NSString stringWithFormat:@"數據機回報成功 %@ 次",fixes];
    return [NSString stringWithFormat:@"%@\n位置回報 %@ 筆 · 峰值可見衛星 %@ 顆",
            fix,reports,fields[@"listed"] ?: @"—"];
}
static UILabel *label(CGFloat size,BOOL bold)
{
    UILabel *l=[[[UILabel alloc] initWithFrame:(CGRect){0}] autorelease];
    l.font=[UIFont systemFontOfSize:size weight:bold?UIFontWeightSemibold:UIFontWeightRegular];
    l.numberOfLines=0;l.textColor=[UIColor whiteColor];
    return l;
}
static NSString *fixQuality(void)
{
    struct {uint32_t version,state,accepted,bits;uint64_t age;} v={0};size_t n=sizeof(v);
    if(sysctlbyname("kern.hackios.modem_fix_quality",&v,&n,NULL,0) || n!=sizeof(v) || v.version!=1)return nil;
    NSArray *states=@[@"未啟用",@"等待位置",@"新鮮衛星定位",@"定位中",@"數據機未定位",@"缺少必要欄位",@"封包無效",@"衛星來源未確認",@"位置已過期",@"缺少可信時間"];
    if(v.state!=2)return v.state<states.count ? states[v.state] : @"狀態未知";
    float meters=0;memcpy(&meters,&v.bits,sizeof(meters));
    return [NSString stringWithFormat:@"衛星位置已驗證 · 水平不確定度 %.1f 公尺 · %.1f 秒前",meters,v.age/1000.0];
}
@implementation GNSSMonitorController
static void saveSettingsAttempt(unsigned attempt, uint64_t tapped, uint64_t ended, int accepted)
{
    // The observer can read this app's own Caches entry. Keep only UI events.
    NSString *home=NSHomeDirectory().stringByStandardizingPath;
    NSString *parent=home.stringByDeletingLastPathComponent;
    if (![parent isEqualToString:@"/var/mobile/Containers/Data/Application"] &&
        ![parent isEqualToString:@"/private/var/mobile/Containers/Data/Application"]) return;
    NSURL *cache=[[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    if (!cache) return;
    [[NSFileManager defaultManager] createDirectoryAtURL:cache withIntermediateDirectories:YES attributes:nil error:NULL];
    NSDictionary *facts=@{@"v":@1,@"pid":@([NSProcessInfo processInfo].processIdentifier),@"attempt":@(attempt),
                          @"tap":@(tapped),@"ended":@(ended),@"accepted":@(accepted)};
    [facts writeToURL:[cache URLByAppendingPathComponent:@"com.hackios.gnss-monitor.settings.plist"] atomically:YES];
}
- (void)openPermissionSettings
{
    if (!self.isViewLoaded || !self.view.window ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    NSURL *settings = [NSURL URLWithString:UIApplicationOpenSettingsURLString];
    if (!settings) return;
    static unsigned attempt;
    markStage(14);unsigned token=++attempt;
    uint64_t began=(uint64_t)(NSProcessInfo.processInfo.systemUptime*1000);
    saveSettingsAttempt(token,began,0,-1);
    NSLog(@"GNSSMonitor: foreground user requested own Settings");
    [UIApplication.sharedApplication openURL:settings options:@{} completionHandler:^(BOOL accepted) {
        markStage(accepted?15:16);if(token==attempt)
            saveSettingsAttempt(token,began,(uint64_t)(NSProcessInfo.processInfo.systemUptime*1000),accepted?1:0);
        NSLog(@"GNSSMonitor: public own Settings open accepted=%d", (int)accepted);
        if (!accepted)
            self.clientLabel.text = NSLocalizedString(@"settings.open.failed", @"iOS could not open the app's Settings");
    }];
}
- (void)showPermission
{
    NSString *system = self.serviceState < 0 ? (self.serviceQueryPending ? @"查詢中" : @"尚未查詢") : (self.serviceState ? @"已開啟" : @"已關閉");
    NSString *status = @"尚未讀取";
    if (self.authState >= 0) {
        switch (self.authState) {
            case kCLAuthorizationStatusNotDetermined: status = @"尚未決定"; break;
            case kCLAuthorizationStatusRestricted: status = @"受限制"; break;
            case kCLAuthorizationStatusDenied: status = @"已拒絕"; break;
            case kCLAuthorizationStatusAuthorizedAlways: status = @"永遠允許"; break;
            case kCLAuthorizationStatusAuthorizedWhenInUse: status = @"使用期間允許"; break;
            default: status = @"未知"; break;
        }
    }
    // App-reported counters only. Permission is always controlled by Core Location.
    unsigned state=self.authState<0 ? 15 : (unsigned)self.authState;
    uint64_t value=((uint64_t)(uint32_t)getpid()<<32) | ((uint64_t)MIN(self.permissionAttempts,255u)<<24) |
        ((uint64_t)MIN(self.permissionCalls,255u)<<16) | ((uint64_t)MIN(self.permissionCallbacks,255u)<<8) |
        ((uint64_t)(self.serviceState+1)<<4) | (state & 15u);
    dispatch_async(telemetryQueue(), ^{
        static int token;static dispatch_once_t once;
        dispatch_once(&once, ^{ if(notify_register_check("com.hackios.gnss-monitor.permission-observation",&token)!=NOTIFY_STATUS_OK)token=-1; });
        if(token>=0)notify_set_state(token,value);
    });
    self.permissionLabel.text = [NSString stringWithFormat:@"系統定位：%@　App 授權：%@\n按鈕 %u · 授權請求 %u · 回呼 %u\n%@",system,status,self.permissionAttempts,self.permissionCalls,self.permissionCallbacks,self.phase ?: @"就緒"];
}
- (void)publishTestTarget:(BOOL)active {
    UIButton *button=self.permissionButton;
    UIWindow *w=self.viewIfLoaded.window;
    uint32_t point=0;
    if(active && w && button && UIApplication.sharedApplication.applicationState==UIApplicationStateActive &&
       !self.permissionAttempts && !self.permissionPending && !self.permissionInFlight && !self.presentedViewController) {
        CGPoint center=[button convertPoint:CGPointMake(button.bounds.origin.x+button.bounds.size.width/2,button.bounds.origin.y+button.bounds.size.height/2) toView:w];
        UIView *hit=[w hitTest:center withEvent:nil];
        CGFloat x=center.x/w.bounds.size.width, y=center.y/w.bounds.size.height;
        // The test tap only targets our own visible top control, above all
        // choices in the standard system alert. A modal or a scrolled view
        // outside that small region advertises no target.
        if((hit==button || [hit isDescendantOfView:button]) && x>0.40 && x<0.60 && y>0.075 && y<0.235)
            point=((uint32_t)lround(x*10000)<<16)|(uint32_t)lround(y*10000);
    }
    uint64_t value=((uint64_t)(uint32_t)getpid()<<32)|point;
    dispatch_async(telemetryQueue(), ^{
        static int token;static dispatch_once_t once;
        dispatch_once(&once, ^{if(notify_register_check("com.hackios.gnss-monitor.test-button",&token)!=NOTIFY_STATUS_OK)token=-1;});
        if(token>=0){
            notify_set_state(token,value);
            notify_post("com.hackios.gnss-monitor.test-button");
        }
    });
}
- (void)discardTestTarget:(NSNotification *)note { (void)note;[self publishTestTarget:NO]; }
- (void)queryGlobalStatus {
    if (self.serviceQueryPending) return;
    self.serviceQueryPending=YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        markStage(12);BOOL enabled=CLLocationManager.locationServicesEnabled;markStage(13);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.serviceQueryPending=NO;
            self.serviceState=enabled ? 1 : 0;
            [self showPermission];
        });
    });
}
- (void)handleWorker:(NSNotification *)note {
    NSDictionary *info=note.userInfo;
    NSString *event=info[@"event"];
    NSInteger state=[info[@"state"] integerValue];
    if(state>=0) self.authState=state;
    NSDictionary *stages=@{@"init-enter":@"正在連接 Core Location",@"init-return":@"已建立定位管理器",
          @"status-enter":@"正在讀取 App 授權",@"status":@"已取得 App 授權狀態",
          @"request-enter":@"正在呼叫系統授權",@"request-return":@"系統已收取授權呼叫",
          @"delegate-enter":@"正在讀取授權回呼",@"callback":@"已收到授權回呼"};
    if(stages[event]) self.phase=stages[event];
    if([event isEqualToString:@"request-enter"]) self.permissionCalls++;
    if([event isEqualToString:@"callback-arrived"]) self.permissionCallbacks++;
    if([event isEqualToString:@"request-return"]) [self queryGlobalStatus];
    if([event isEqualToString:@"permission-done"]) self.permissionInFlight=NO;
    if(state>0 && self.permissionPending) {
        self.permissionPending=NO;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(permissionTimedOut) object:nil];
        self.clientLabel.text=@"系統的授權狀態顯示在上方。";
    }
    if([event isEqualToString:@"position-done"] && self.positionPending) {
        self.positionPending=NO;
        self.clientLabel.text=info[@"message"];
    }
    [self showPermission];
}
- (void)permissionTimedOut {
    if(!self.permissionPending)return;
    self.permissionPending=NO;
    self.worker.consentGeneration++;
    self.clientLabel.text=@"15 秒內系統未回覆。可重新查詢，或打開設定。";
    [self showPermission];
}
- (void)locationTimedOut {
    if (!self.positionPending) return;
    self.positionPending=NO; self.worker.positionGeneration++;
    self.clientLabel.text=@"30 秒內未收到新位置";
}
- (void)beginLocationCheck {
    if(!self.view.window || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive || self.positionPending)return;
    if(self.authState!=kCLAuthorizationStatusAuthorizedWhenInUse && self.authState!=kCLAuthorizationStatusAuthorizedAlways) {
        self.clientLabel.text=@"請先使用「申請定位權限」。";return;
    }
    self.positionPending=YES;self.worker.visible=YES;
    NSUInteger generation=self.worker.positionGeneration+1;self.worker.positionGeneration=generation;
    NSDictionary *action=@{@"generation":@(generation),@"expires":@(NSProcessInfo.processInfo.systemUptime+30)};
    NSLog(@"GNSSMonitor: foreground location check requested by user");
    [self performSelector:@selector(locationTimedOut) withObject:nil afterDelay:30];
    [self.worker performSelector:@selector(position:) onThread:self.locationThread withObject:action waitUntilDone:NO];
}
- (void)requestPermission {
    if(!self.view.window || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
    markStage(4);self.worker.visible=YES; self.permissionAttempts++;[self publishTestTarget:NO];
    [self showPermission];
    if(self.permissionInFlight||self.permissionPending) {
        self.clientLabel.text=@"上次呼叫仍在等待系統回覆。";return;
    }
    self.permissionPending=YES; self.permissionInFlight=YES;
    NSUInteger generation=self.worker.consentGeneration+1;self.worker.consentGeneration=generation;
    self.clientLabel.text=@"已點選申請定位權限。";
    [self performSelector:@selector(permissionTimedOut) withObject:nil afterDelay:15];
    NSDictionary *action=@{@"generation":@(generation),@"expires":@(NSProcessInfo.processInfo.systemUptime+15)};
    [self.worker performSelector:@selector(permission:) onThread:self.locationThread withObject:action waitUntilDone:NO];
}
- (void)locationBecameActive:(NSNotification *)note {
    (void)note; self.worker.visible=self.view.window!=nil; [self showPermission];
}
- (void)locationBackground:(NSNotification *)note {
    (void)note;self.worker.visible=NO;self.worker.consentGeneration++;self.worker.positionGeneration++;
}
- (void)viewDidLoad
{
    [super viewDidLoad];markStage(1);
    self.view.backgroundColor=[UIColor colorWithWhite:0.08 alpha:1];
    UIScrollView *scroll=[[[UIScrollView alloc] init] autorelease];scroll.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:scroll];
    UIStackView *stack=[[[UIStackView alloc] init] autorelease];stack.axis=UILayoutConstraintAxisVertical;
    stack.spacing=18;stack.translatesAutoresizingMaskIntoConstraints=NO;[scroll addSubview:stack];
    UILabel *heading=label(28,YES);heading.text=@"無線通訊";[stack addArrangedSubview:heading];
    UIButton *permission=[UIButton buttonWithType:UIButtonTypeSystem];
    [permission setTitle:@"申請定位權限" forState:UIControlStateNormal];
    [permission addTarget:self action:@selector(requestPermission) forControlEvents:UIControlEventTouchUpInside];
    [permission.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;
    [stack addArrangedSubview:permission];self.permissionButton=permission;
    self.radioHeading=label(19,YES);self.radioHeading.text=@"行動網路 · 最近查詢";[stack addArrangedSubview:self.radioHeading];
    self.statusLabel=label(16,NO);self.statusLabel.text=@"正在讀取行動網路狀態";[stack addArrangedSubview:self.statusLabel];
    UILabel *gnss=label(19,YES);gnss.text=@"衛星定位";[stack addArrangedSubview:gnss];
    self.detailLabel=label(16,NO);self.detailLabel.text=@"正在讀取衛星定位狀態";[stack addArrangedSubview:self.detailLabel];
    self.serviceState = -1; self.authState=-1;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(discardTestTarget:) name:UIApplicationWillResignActiveNotification object:nil];
    self.worker=[[[GNSSLocationWorker alloc] init] autorelease];
    self.locationThread=[[[NSThread alloc] initWithTarget:self.worker selector:@selector(serve) object:nil] autorelease];
    self.locationThread.name=@"GNSSMonitor Core Location";
    [self.locationThread start];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(handleWorker:) name:workerNotification object:nil];
    self.permissionLabel=label(16,NO);
    [stack addArrangedSubview:self.permissionLabel];
    UIButton *button=[UIButton buttonWithType:UIButtonTypeSystem];
    [button setTitle:@"驗證系統定位" forState:UIControlStateNormal];
    [button addTarget:self action:@selector(beginLocationCheck) forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;
    [stack addArrangedSubview:button];self.clientLabel=label(16,NO);[stack addArrangedSubview:self.clientLabel];
    self.clientLabel.text=@"先申請權限，再按需要驗證系統定位。";
    UIButton *settingsButton=[UIButton buttonWithType:UIButtonTypeSystem];
    [settingsButton setTitle:NSLocalizedString(@"settings.open.title", @"Open this app's Settings") forState:UIControlStateNormal];
    [settingsButton addTarget:self action:@selector(openPermissionSettings) forControlEvents:UIControlEventTouchUpInside];
    [settingsButton.heightAnchor constraintGreaterThanOrEqualToConstant:44].active=YES;
    [stack addArrangedSubview:settingsButton];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(locationBecameActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(locationBackground:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:36],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:22],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-22],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-44]]];
}
- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];markStage(3);NSLog(@"GNSSMonitor: private-fix profile active; system request requires a foreground tap");
}
- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];self.worker.visible=YES;[self showPermission];
    self.timer=[NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(refresh:) userInfo:nil repeats:YES];
}
- (void)viewDidDisappear:(BOOL)animated
{
    [self locationBackground:nil];[super viewDidDisappear:animated];[self.timer invalidate];self.timer=nil;
}
- (void)refresh:(NSTimer *)timer {
    (void)timer;
    [self publishTestTarget:YES];
    if(self.radioPending)return;
    self.radioPending=YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        id setting=[NSUserDefaults.standardUserDefaults objectForKey:@"ShowRadioSummary"];
        BOOL show=setting==nil || [setting boolValue];
        NSString *radio=show ? radioStatus() : @"";
        NSString *quality=fixQuality(), *status=gnssStatus();
        NSString *gnss=quality ? [NSString stringWithFormat:@"%@\n%@",status,quality] : status;
        dispatch_async(dispatch_get_main_queue(), ^{
            self.radioPending=NO;
            self.radioHeading.hidden=!show;self.statusLabel.hidden=!show;
            self.statusLabel.text=radio;self.detailLabel.text=gnss;
        });
    });
}
- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [_locationThread cancel]; [_locationThread release];[_worker release];
    [_permissionButton release];[_phase release];[_permissionLabel release];[_radioHeading release];[_clientLabel release];[_statusLabel release];[_detailLabel release];[_timer release];[super dealloc];
}
@end

@interface GNSSMonitorDelegate : NSObject <UIApplicationDelegate>
@property(nonatomic, retain) UIWindow *window;
@end

@implementation GNSSMonitorDelegate
- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary *)options
{
    (void)application;
    (void)options;
    if (![url.scheme isEqualToString:@"hackios-gnss-monitor"] || ![url.host isEqualToString:@"open"]) return NO;
    NSLog(@"GNSSMonitor: own URL delivered by system");
    return YES;
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
    markStage(0);
    self.window = [[[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]] autorelease];
    self.window.rootViewController = [[[GNSSMonitorController alloc] init] autorelease];
    [self.window makeKeyAndVisible];markStage(2);
    return YES;
}
- (void)dealloc { [_window release]; [super dealloc]; }
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([GNSSMonitorDelegate class]));
    }
}
