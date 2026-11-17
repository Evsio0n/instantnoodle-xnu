/* Ask FrontBoard to perform its normal test-device unlock/foreground flow.
 * PromptUnlockDevice remains enabled: no passcode or authentication result
 * is supplied or overridden. The keys are exported by the pinned16.5 SDK. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
@interface NSObject (HKFrontBoardLaunch)
+ (id)serviceWithDefaultShellEndpoint;
+ (id)optionsWithDictionary:(NSDictionary *)dictionary;
- (void)openApplication:(NSString *)bundle withOptions:(id)options completion:(void (^)(id,NSError *))completion;
@end
int main(int argc,char **argv)
{
    @autoreleasepool {
        if(argc!=2 || strcmp(argv[1],"com.hackios.airdrop-test"))return 2;
        setvbuf(stdout,NULL,_IOLBF,0);
        void *handle=dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",RTLD_NOW);
        NSString *const *unlock=handle?(NSString *const *)dlsym(handle,"FBSOpenApplicationOptionKeyUnlockDevice"):NULL;
        NSString *const *prompt=handle?(NSString *const *)dlsym(handle,"FBSOpenApplicationOptionKeyPromptUnlockDevice"):NULL;
        Class cls=NSClassFromString(@"FBSOpenApplicationService"),optionsClass=NSClassFromString(@"FBSOpenApplicationOptions");
        if(!unlock || !prompt || !cls || ![optionsClass respondsToSelector:@selector(optionsWithDictionary:)]) {
            puts("airdrop-launch: system_unlock_options_unavailable");return 1;
        }
        id service=[cls serviceWithDefaultShellEndpoint];
        id options=[optionsClass optionsWithDictionary:@{*unlock:@YES,*prompt:@YES}];
        dispatch_semaphore_t done=dispatch_semaphore_create(0);__block int result=1;
        [service openApplication:@"com.hackios.airdrop-test" withOptions:options completion:^(id process,NSError *error) {
            (void)process;
            if(!error) {puts("airdrop-launch: native_foreground_accepted unlock_requested=1 prompt_allowed=1");result=0;}
            else printf("airdrop-launch: refused domain=%s code=%ld reason=%s\n",error.domain.UTF8String,(long)error.code,error.localizedDescription.UTF8String);
            dispatch_semaphore_signal(done);
        }];
        if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,20*NSEC_PER_SEC))) {
            puts("airdrop-launch: outcome_pending");return 1;
        }
        return result;
    }
}
