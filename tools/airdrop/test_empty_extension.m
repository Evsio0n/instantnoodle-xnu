#import <Foundation/Foundation.h>
#import "EmptyExtensionPolicy.h"
int main(void){@autoreleasepool{
 NSData *token=[@"native-token\0" dataUsingEncoding:NSUTF8StringEncoding];NSData *empty=[NSData data];
 NSDictionary *source=@{@"valid":token,@"empty":empty,@"other":@42};NSUInteger n=0;
 NSDictionary *filtered=HKWithoutEmptyExtensionData(source,&n);
 NSCAssert(n==1 && filtered.count==2,@"only empty data removed");
 NSCAssert(filtered[@"valid"]==token && [filtered[@"valid"] isEqualToData:token],@"native token bytes/object preserved");
 NSCAssert(filtered[@"other"]==source[@"other"] && source.count==3,@"other entries and source immutable");
 NSCAssert(HKWithoutEmptyExtensionData(@{@"valid":token},&n)==nil && n==0,@"valid dictionary needs no replacement");
 NSCAssert(HKWithoutEmptyExtensionData(nil,&n)==nil && n==0,@"missing dictionary untouched");
 NSDictionary *all=HKWithoutEmptyExtensionData(@{@"a":empty},&n);NSCAssert(n==1 && all.count==0,@"no fabricated token or grant");
 puts("PASS empty-token normalization: only empty NSData excluded; native tokens and source preserved; no fabricated grants");
}return 0;}
