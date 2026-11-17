#import <Foundation/Foundation.h>
/* Retained #575 experiment, NOT a fix to promote: native macOS27 already
 * maps the absent optional field to true. The RAM extension was observed
 * on the wire and did not restore Finder visibility. Default builds omit it. */
/* Modern Discover clients decode IsAirDropable as an optional Boolean.
 * Publish the already-authorized native receiver capability, never an
 * account/contact identity. A contacts-only unknown sender gets no public
 * ReceiverComputerName and is deliberately ineligible for this extension.
 * Existing explicit capability values and non-200 responses are immutable. */
static NSDictionary *HKDiscoverCapability(NSDictionary *native,NSInteger status) {
    if(status!=200 || ![native isKindOfClass:NSDictionary.class] || native[@"IsAirDropable"]!=nil)return nil;
    id name=native[@"ReceiverComputerName"];
    if(![name isKindOfClass:NSString.class] || ![name length])return nil;
    NSMutableDictionary *updated=[native mutableCopy];updated[@"IsAirDropable"]=@YES;
    return updated;
}
