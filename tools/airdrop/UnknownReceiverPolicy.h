#import <Foundation/Foundation.h>
/* UI inclusion only. The original model's Unknown/contact/trust state and
 * the native Discover/Ask/Upload protocol are never changed by this policy. */
static BOOL HKListUnknownReceiver(BOOL original,BOOL unknown,NSString *identifier,
                                  NSString *displayName,NSString *transport){
 if(original)return YES;
 return unknown && [identifier isKindOfClass:NSString.class] && identifier.length>0 && identifier.length<=255 &&
  [displayName isKindOfClass:NSString.class] && displayName.length>0 &&
  [transport isEqualToString:@"com.apple.UIKit.activity.AirDrop"];
}
