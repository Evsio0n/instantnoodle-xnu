#import <Foundation/Foundation.h>
#import "UnknownReceiverPolicy.h"
int main(void){@autoreleasepool{
 NSString *i=@"observed-native-service",*n=@"Owned test computer",*t=@"com.apple.UIKit.activity.AirDrop";
 NSCAssert(HKListUnknownReceiver(YES,NO,nil,nil,nil),@"original positive decision retained");
 NSCAssert(HKListUnknownReceiver(NO,YES,i,n,t),@"real unknown AirDrop model can be listed");
 NSCAssert(!HKListUnknownReceiver(NO,NO,i,n,t),@"other negative decisions retained");
 NSCAssert(!HKListUnknownReceiver(NO,YES,nil,n,t),@"no identifier");
 NSCAssert(!HKListUnknownReceiver(NO,YES,i,nil,t),@"no display name");
 NSCAssert(!HKListUnknownReceiver(NO,YES,i,n,@"other-transport"),@"other provider unchanged");
 NSCAssert(!HKListUnknownReceiver(NO,YES,[@"x" stringByPaddingToLength:256 withString:@"x" startingAtIndex:0],n,t),@"bounded identifier");
 NSCAssert([i isEqual:@"observed-native-service"] && [n isEqual:@"Owned test computer"],@"native identity inputs unchanged");
 puts("PASS UI inclusion: original decision retained, real unknown AirDrop only, missing/other fields preserved, identities immutable");
}return 0;}
