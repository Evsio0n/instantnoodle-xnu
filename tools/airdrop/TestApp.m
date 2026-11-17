#import <UIKit/UIKit.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#import "IconObservation.h"

static NSString *hashOfContents(NSData *contents) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(contents.bytes, (CC_LONG)contents.length, digest);
    char hex[2*sizeof(digest)+1];
    for (size_t i=0;i<sizeof(digest);++i) snprintf(hex+2*i,3,"%02x",digest[i]);
    return [NSString stringWithUTF8String:hex];
}

@interface HKShareController : UIViewController
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) NSURL *lastDirectory;
@end
@implementation HKShareController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor=UIColor.systemBackgroundColor;
    self.title=@"AirDrop Test";

    UIButtonConfiguration *configuration=[UIButtonConfiguration filledButtonConfiguration];
    configuration.title=@"Share test file";
    self.button=[UIButton buttonWithConfiguration:configuration primaryAction:nil];
    [self.button addTarget:self action:@selector(share) forControlEvents:UIControlEventTouchUpInside];
    [self.button.heightAnchor constraintGreaterThanOrEqualToConstant:48].active=YES;
    self.status=[UILabel new];
    self.status.numberOfLines=0;
    self.status.lineBreakMode=NSLineBreakByCharWrapping;
    self.status.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.status.textColor=UIColor.secondaryLabelColor;
    self.status.textAlignment=NSTextAlignmentCenter;
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:@[self.button,self.status]];
    stack.axis=UILayoutConstraintAxisVertical;
    stack.spacing=16;
    stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack];
    UILayoutGuide *guide=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:28],
        [stack.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-28],
        [stack.centerYAnchor constraintEqualToAnchor:guide.centerYAnchor]
    ]];
}

- (void)share {
    if (self.presentedViewController || !self.button.enabled ||
        !self.view.window || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return;
    NSString *nonce=NSUUID.UUID.UUIDString;
    NSData *data=[[NSString stringWithFormat:@"HackiOS AirDrop test\n%@\n",nonce] dataUsingEncoding:NSUTF8StringEncoding];
    NSFileManager *fm=NSFileManager.defaultManager;
    NSURL *dir=[fm.temporaryDirectory URLByAppendingPathComponent:nonce isDirectory:YES];
    NSError *error=nil;
    if (![fm createDirectoryAtURL:dir withIntermediateDirectories:NO attributes:nil error:&error]) {
        self.status.text=error.localizedDescription;
        return;
    }
    NSURL *file=[dir URLByAppendingPathComponent:@"AirDrop-test.txt" isDirectory:NO];
    if (![data writeToURL:file options:NSDataWritingAtomic error:&error]) {
        [fm removeItemAtURL:dir error:nil];
        self.status.text=error.localizedDescription;
        return;
    }
    if (self.lastDirectory) [fm removeItemAtURL:self.lastDirectory error:nil];
    self.lastDirectory=dir;
    NSString *hash=hashOfContents(data);
    NSString *receipt=[NSString stringWithFormat:@"AirDrop-test.txt · %lu bytes\nSHA-256 %@",(unsigned long)data.length,hash];
    self.status.text=receipt;
    NSLog(@"airdrop-test: selected_readonly_file bytes=%lu sha256=%@",(unsigned long)data.length,hash);

    UIActivityViewController *sheet=[[UIActivityViewController alloc] initWithActivityItems:@[file] applicationActivities:nil];
    sheet.popoverPresentationController.sourceView=self.button;
    sheet.popoverPresentationController.sourceRect=self.button.bounds;
    self.button.enabled=NO;
    __weak HKShareController *weakSelf=self;
    sheet.completionWithItemsHandler=^(UIActivityType type, BOOL completed, NSArray *returnedItems, NSError *failure) {
        BOOL isAirDrop=[type isEqualToString:UIActivityTypeAirDrop];
        NSLog(@"airdrop-test: share_closed type=%@ completed=%d error_domain=%@ error_code=%ld",
              !type?@"none":isAirDrop?@"airdrop":@"other",completed,failure.domain ?: @"none",(long)failure.code);
        dispatch_async(dispatch_get_main_queue(), ^{
            HKShareController *controller=weakSelf;
            controller.button.enabled=YES;
            NSString *outcome=failure.localizedDescription;
            if (!outcome.length && isAirDrop && completed) outcome=@"AirDrop share completed";
            if (!outcome.length && !completed) outcome=@"Share cancelled";
            controller.status.text=outcome.length?[NSString stringWithFormat:@"%@\n%@",outcome,receipt]:receipt;
        });
    };
    [self presentViewController:sheet animated:YES completion:^{
        for (unsigned delay=1;delay<=4;delay+=3) dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)delay*NSEC_PER_SEC),dispatch_get_main_queue(),^{ if(sheet.view.window) HKRecordIcons(sheet); });
        NSLog(@"airdrop-test: native_share_sheet_presented bytes=%lu sha256=%@",(unsigned long)data.length,hash);
    }];
}
@end

@interface HKShareDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation HKShareDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    app.idleTimerDisabled=YES;
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[[UINavigationController alloc] initWithRootViewController:[HKShareController new]];
    [self.window makeKeyAndVisible];
    return YES;
}
- (void)applicationDidBecomeActive:(UIApplication *)app { app.idleTimerDisabled=YES; }
- (void)applicationDidEnterBackground:(UIApplication *)app { app.idleTimerDisabled=NO; }
@end
int main(int argc,char **argv) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(HKShareDelegate.class)); }
}
