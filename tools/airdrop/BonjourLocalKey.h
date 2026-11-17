#import <Foundation/Foundation.h>
/* A local grouping key for an existing anonymous native Bonjour node.
 * It is never an Apple ID, contact identifier, validation record or BLE
 * authentication tag. The native node retains all original trust/kind flags.
 * A real peer's Bonjour service instance provides the opaque local name. */
static NSString *HKBonjourLocalKey(BOOL bonjour,NSString *service,NSString *computer,
                                   NSString *user,NSString *contact)
{
 if(!bonjour || user || contact || !service.length || service.length>255 || !computer.length)return nil;
 return [@"bonjour:" stringByAppendingString:service];
}
