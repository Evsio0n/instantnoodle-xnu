#import <Foundation/Foundation.h>
/* Empty NSData has no C token string. Preserve every nonempty native token
 * and every other entry; never create a token or turn a failure into a grant. */
static NSDictionary *HKWithoutEmptyExtensionData(NSDictionary *tokens,NSUInteger *removed){
 if(removed)*removed=0;
 if(![tokens isKindOfClass:NSDictionary.class])return nil;
 NSMutableDictionary *copy=[tokens mutableCopy];NSUInteger count=0;
 for(id key in tokens){id value=tokens[key];if([value isKindOfClass:NSData.class] && [value length]==0){[copy removeObjectForKey:key];count++;}}
 if(removed)*removed=count;return count?copy:nil;
}
