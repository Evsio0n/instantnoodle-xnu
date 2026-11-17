#import <Foundation/Foundation.h>
#import "DiscoverCapability.h"
int main(void){@autoreleasepool {
 NSDictionary *native=@{@"ReceiverComputerName":@"Local device",@"ReceiverMediaCapabilities":[NSData dataWithBytes:"abc" length:3]};
 NSDictionary *updated=HKDiscoverCapability(native,200);
 NSCAssert([updated[@"IsAirDropable"] isEqual:@YES],@"publish native public eligibility");
 NSCAssert(updated.count==native.count+1 && native.count==2,@"only add capability; preserve input");
 for(id key in native)NSCAssert([native[key] isEqual:updated[key]],@"native fields immutable");
 NSCAssert(!HKDiscoverCapability(native,401),@"never turn rejection into eligibility");
 NSCAssert(!HKDiscoverCapability(@{@"ReceiverMediaCapabilities":[NSData data]},200),@"contacts-only unknown remains private");
 NSCAssert(!HKDiscoverCapability(@{@"ReceiverComputerName":@""},200),@"no name, no eligibility");
 NSCAssert(!HKDiscoverCapability(@{@"ReceiverComputerName":@3},200),@"invalid name");
 NSCAssert(!HKDiscoverCapability(@{@"ReceiverComputerName":@"Local",@"IsAirDropable":@NO},200),@"explicit denial immutable");
 for(NSNumber *format in @[@(NSPropertyListXMLFormat_v1_0),@(NSPropertyListBinaryFormat_v1_0)]) {
  NSData *data=[NSPropertyListSerialization dataWithPropertyList:updated format:format.unsignedIntegerValue options:0 error:NULL];
  NSDictionary *decoded=[NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
  NSCAssert([decoded isEqual:updated],@"original wire format roundtrip");
 }
 puts("PASS native capability extension: public 200 only, original fields/denial/privacy preserved, XML/binary roundtrip");
}return 0;}
