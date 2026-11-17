#import <Foundation/Foundation.h>
#import "BonjourLocalKey.h"
int main(void){@autoreleasepool{
 NSString *service=@"observed-service-123",*computer=@"Owned test computer";
 NSCAssert([HKBonjourLocalKey(YES,service,computer,nil,nil) isEqualToString:@"bonjour:observed-service-123"],@"real service instance key");
 NSCAssert(HKBonjourLocalKey(YES,service,computer,@"",nil)==nil,@"existing empty object preserved");
 NSCAssert(HKBonjourLocalKey(NO,service,computer,nil,nil)==nil,@"non-Bonjour preserved");
 NSCAssert(HKBonjourLocalKey(YES,service,computer,@"existing-user",nil)==nil,@"existing user preserved");
 NSCAssert(HKBonjourLocalKey(YES,service,computer,nil,@"existing-contact")==nil,@"existing contact preserved");
 NSCAssert(HKBonjourLocalKey(YES,nil,computer,nil,nil)==nil,@"missing real service rejected");
 NSCAssert(HKBonjourLocalKey(YES,service,nil,nil,nil)==nil,@"missing real computer rejected");
 NSString *large=[@"x" stringByPaddingToLength:256 withString:@"x" startingAtIndex:0];
 NSCAssert(HKBonjourLocalKey(YES,large,computer,nil,nil)==nil,@"invalid service size rejected");
 NSCAssert([service isEqualToString:@"observed-service-123"] && [computer isEqualToString:@"Owned test computer"],@"inputs immutable");
 puts("PASS Bonjour local grouping policy: derived key, native-node scope, existing identity preservation, real-field requirements");
}return 0;}
