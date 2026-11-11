/*
 * hk_commcenter: a stand-in for CommCenter, so that iOS has somebody to ask about the cellular side.
 *
 * The stock CommCenter cannot run here: it dies on two IOKit drivers of Apple's cellular path that this kernel has
 * not got (mkjobs.py tells that story). The modem is the kernel's (tools/nf/cell/instantnoodle_cell.c, boot-arg
 * cell=1), which says what it knows through sysctls: kern.hackios.cellular_registered, _bars, _dbm, _data, _operator.
 * This daemon (job com.hackios.telephonyd) takes CommCenter's two Mach services and answers them from those:
 *
 *   com.apple.commcenter.coretelephony.xpc   NSXPC, the protocol CTXPCServiceInterface (CoreTelephonyClient's side
 *       of it: SpringBoard's status bar, Settings, imagent ...). The exported object has no methods of its own:
 *       every request arrives in forwardInvocation:, is counted and logged (the first few of each kind, with the
 *       types of what its reply block takes, read from the protocol), and its reply block is called with the
 *       values answers_for() has for that selector, or with nil and zero. Clients that register for notifications
 *       are told the delegate calls of CoreTelephonyClientDelegateProxyInterface, again when the numbers change.
 *   com.apple.commcenter.xpc                 the older API (CTServerConnection, CTTelephonyCenter): plain XPC
 *       dictionaries, "kRequest" names the call and every other key is a placeholder of the type wanted. The reply
 *       carries the same keys; the ones known here are answered, the rest go back as they came (zero, false).
 *
 * The design is the OnePlus 8 Pro port's (docs/research/modem-sdx55.md there, "a stand-in for CommCenter"); its
 * program is not public, so this one is written from that description. Nothing of the subscriber's identity is
 * logged: requests are logged by selector and client, never by argument values.
 */
#import <Foundation/Foundation.h>
#import <CallKit/CallKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* XPC: not in this SDK's headers, in the libraries all the same. Its objects are Objective-C objects, so ARC keeps them. */
typedef NSObject *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern const struct _xpc_type_s _xpc_type_dictionary, _xpc_type_connection, _xpc_type_bool, _xpc_type_int64, _xpc_type_string, _xpc_type_array, _xpc_type_data;
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_BOOL (&_xpc_type_bool)
#define XPC_TYPE_INT64 (&_xpc_type_int64)
#define XPC_TYPE_STRING (&_xpc_type_string)
#define XPC_TYPE_ARRAY (&_xpc_type_array)
#define XPC_TYPE_DATA (&_xpc_type_data)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags) __attribute__((ns_returns_retained));
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern pid_t xpc_connection_get_pid(xpc_connection_t connection);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original) __attribute__((ns_returns_retained));
extern bool xpc_dictionary_apply(xpc_object_t dictionary, bool (^applier)(const char *key, xpc_object_t value));
extern void xpc_dictionary_set_value(xpc_object_t dictionary, const char *key, xpc_object_t value);
extern void xpc_dictionary_set_bool(xpc_object_t dictionary, const char *key, bool value);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
extern void xpc_dictionary_set_data(xpc_object_t dictionary, const char *key, const void *bytes, size_t length);
extern const void *xpc_dictionary_get_data(xpc_object_t dictionary, const char *key, size_t *length);
extern int64_t xpc_dictionary_get_int64(xpc_object_t dictionary, const char *key);
extern uint64_t xpc_dictionary_get_uint64(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_array_create(const xpc_object_t *objects, size_t count) __attribute__((ns_returns_retained));
extern void xpc_array_set_int64(xpc_object_t array, size_t index, int64_t value);
extern int64_t xpc_int64_get_value(xpc_object_t value);
extern uint64_t xpc_uint64_get_value(xpc_object_t value);
extern const struct _xpc_type_s _xpc_type_uint64;
#define XPC_TYPE_UINT64 (&_xpc_type_uint64)
#define XPC_ARRAY_APPEND ((size_t)-1)

extern const char *_protocol_getMethodTypeEncoding(Protocol *p, SEL sel, BOOL isRequiredMethod, BOOL isInstanceMethod);

static void
say(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	fputs("hk_commcenter: ", stdout);
	vfprintf(stdout, format, ap);
	va_end(ap);
	fflush(stdout);
}

/* ---- what the kernel says ------------------------------------------------------------------------------------------ */

static struct {
	int registered, bars, dbm, data;
	char oper[64];
} now;

static int
sys_int(const char *name)
{
	int v = 0;
	size_t n = sizeof(v);

	return sysctlbyname(name, &v, &n, NULL, 0) == 0 ? v : 0;
}

/* true if anything changed */
static bool
read_state(void)
{
	typeof(now) was = now;
	size_t n = sizeof(now.oper) - 1;

	now.registered = sys_int("kern.hackios.cellular_registered");
	now.bars = sys_int("kern.hackios.cellular_bars");
	now.dbm = sys_int("kern.hackios.cellular_dbm");
	now.data = sys_int("kern.hackios.cellular_data");
	memset(now.oper, 0, sizeof(now.oper));
	if (sysctlbyname("kern.hackios.cellular_operator", now.oper, &n, NULL, 0) != 0) {
		now.oper[0] = 0;
	}
	/* (the dBm moves all the time and is nobody's notification: a change of it alone tells no one) */
	was.dbm = now.dbm;
	return memcmp(&was, &now, sizeof(now)) != 0;
}

/* ---- QMI through the kernel's modem thread (sysctl kern.hackios.modem_qmi) ---------------------------------------- */

static NSLock *qmi_lock;

/*
 * One request to the modem: "SERVICE MSGID PORT TLVBYTES" written to the sysctl, the answer read back once the modem's thread has made the
 * call ("result R error E; [type/length:hex] ..."). The answer's TLVs by type; *result 0 when the modem said yes, its error code otherwise.
 * nil: no answer.
 */
static NSDictionary<NSNumber *, NSData *> *
qmi_from(unsigned port, unsigned seconds, unsigned service, unsigned message, const uint8_t *tlvs, size_t length, int *result)
{
	static char answer[4096];
	char request[420];
	size_t at = (size_t)snprintf(request, sizeof(request), "%u %x %u ", service, message, port), n;
	NSMutableDictionary *out = nil;
	unsigned r = 0xffff, e = 0;
	int sent = -1;

	for (size_t i = 0; i < length && at + 3 < sizeof(request); i++) {
		at += (size_t)snprintf(request + at, sizeof(request) - at, "%02x", tlvs[i]);
	}
	if (qmi_lock == nil) {
		qmi_lock = [NSLock new];
	}
	[qmi_lock lock];
	for (int tries = 0; tries < 50 && sent != 0; tries++) {
		sent = sysctlbyname("kern.hackios.modem_qmi", NULL, NULL, request, strlen(request));
		if (sent != 0) {
			if (errno != EBUSY) {
				break;
			}
			usleep(100000);
		}
	}
	answer[0] = 0;
	for (unsigned i = 0; sent == 0 && i < seconds * 10; i++) {
		usleep(100000);
		n = sizeof(answer) - 1;
		if (sysctlbyname("kern.hackios.modem_qmi", answer, &n, NULL, 0) != 0) {
			answer[0] = 0;
			break;
		}
		answer[n < sizeof(answer) ? n : sizeof(answer) - 1] = 0;
		if (strcmp(answer, "queued") != 0) {
			break;
		}
		answer[0] = 0;
	}
	if (answer[0] != 0 && sscanf(answer, "result %u error %u;", &r, &e) == 2) {
		out = [NSMutableDictionary dictionary];
		for (const char *q = strchr(answer, '['); q != NULL; q = strchr(q + 1, '[')) {
			unsigned type = 0, len = 0;
			const char *hex = strchr(q, ':');
			NSMutableData *value = [NSMutableData data];

			if (hex == NULL || sscanf(q, "[%x/%u:", &type, &len) != 2) {
				continue;
			}
			for (hex++; hex[0] != 0 && hex[1] != 0 && hex[0] != ']'; hex += 2) {
				unsigned byte = 0;

				if (sscanf(hex, "%2x", &byte) != 1) {
					break;
				}
				[value appendBytes:&(uint8_t){ (uint8_t)byte } length:1];
			}
			out[@(type)] = value;
		}
	}
	[qmi_lock unlock];
	if (result != NULL) {
		*result = out == nil ? -1 : r == 0 ? 0 : e != 0 ? (int)e : -1;
	}
	return out;
}

static NSDictionary<NSNumber *, NSData *> *
qmi(unsigned service, unsigned message, const uint8_t *tlvs, size_t length, int *result)
{
	return qmi_from(4, 15, service, message, tlvs, length, result);
}

/* What is learnt of the modem this way; -1: not known. Nothing of the subscriber: the serving network's code is the network's. */
static struct {
	int mode_pref;          /* NAS system selection preference, the mode mask: 0x04 GSM, 0x08 UMTS, 0x10 LTE, 0x40 NR */
	int mcc, mnc;           /* the serving network */
	int ims;                /* IMS registration: 0 not, 1 registering, 2 registered, 3 limited */
} facts = { -1, -1, -1, -1 };

static void
read_facts(bool say_them)
{
	int result = -1;
	NSDictionary<NSNumber *, NSData *> *a = qmi(3, 0x34, NULL, 0, &result);
	const uint8_t *b;

	if (a != nil && result == 0 && a[@0x11].length >= 2) {
		b = a[@0x11].bytes;
		facts.mode_pref = b[0] | b[1] << 8;
	}
	a = qmi(3, 0x24, NULL, 0, &result);
	if (a != nil && result == 0 && a[@0x12].length >= 4) {
		b = a[@0x12].bytes;
		facts.mcc = b[0] | b[1] << 8;
		facts.mnc = b[2] | b[3] << 8;
	}
	if (say_them) {
		say("the modem: mode preference %#x (LTE %d, NR %d), serving network %03d-%02d, IMS registration %d (2: registered)\n", facts.mode_pref,
		    facts.mode_pref >= 0 && (facts.mode_pref & 0x10) != 0, facts.mode_pref >= 0 && (facts.mode_pref & 0x40) != 0, facts.mcc, facts.mnc, facts.ims);
	}
}

/*
 * VoLTE. The modem's IMS brings its own data session up at the attach (seen in its log: the "ims" access point, 2026-11-10) and registers
 * by itself when it has the carrier's settings: those come with the carrier configuration (MCFG) the modem selects for the SIM, which only
 * takes effect at a start of the modem that still has what it saved (the kernel keeps its saved files in memory for that, cell=1). Here the
 * registration is only watched: IMSA bound to the subscription, its state read until it says registered or a minute and a half is over.
 */
static void
ims_watch(void)
{
	static const uint8_t bind[] = { 0x10, 0x04, 0x00, 0, 0, 0, 0 };
	static bool busy;
	int result = -1, was = -2;
	NSDictionary<NSNumber *, NSData *> *a;
	const uint8_t *b;

	if (busy || !now.registered) {
		return;
	}
	busy = true;
	qmi(33, 0x33, bind, sizeof(bind), &result);
	for (int i = 0; i < 30 && now.registered; i++) {
		a = qmi(33, 0x20, NULL, 0, &result);
		if (a != nil && result == 0 && a[@0x12].length >= 4) {
			b = a[@0x12].bytes;
			facts.ims = b[0];
		}
		if (facts.ims != was) {
			was = facts.ims;
			say("ims: registration %d (0 not registered, 1 registering, 2 REGISTERED, 3 limited)%s\n", facts.ims,
			    a[@0x11].length >= 2 ? [NSString stringWithFormat:@", error code %u", ((const uint8_t *)a[@0x11].bytes)[0] | ((const uint8_t *)a[@0x11].bytes)[1] << 8].UTF8String : "");
		}
		if (facts.ims == 2) {
			break;
		}
		sleep(3);
	}
	a = qmi(33, 0x21, NULL, 0, &result);
	if (a != nil && result == 0) {
		say("ims: voice service status %d (2: full), SMS %d\n", a[@0x11].length >= 4 ? ((const uint8_t *)a[@0x11].bytes)[0] : -1,
		    a[@0x10].length >= 4 ? ((const uint8_t *)a[@0x10].bytes)[0] : -1);
	}
	busy = false;
}

/* ---- CoreTelephony's values, built by name (the classes are private) --------------------------------------------- */

static id
make(const char *name)
{
	Class c = objc_getClass(name);

	return c != Nil ? [[c alloc] init] : nil;
}

static void
put(id object, NSString *key, id value)
{
	@try {
		[object setValue:value forKey:key];
	} @catch (NSException *e) {
		say("%s has no settable %s (%s)\n", class_getName([object class]), key.UTF8String, e.name.UTF8String);
	}
}

static id
context(void)
{
	static id ctx;

	if (ctx == nil) {
		Class c = objc_getClass("CTXPCServiceSubscriptionContext");

		/* (+contextWithSlot: is the way: the 8 Pro's notes say initWithUUID:andSlot: crashes) */
		ctx = c != Nil ? ((id (*)(Class, SEL, long long))objc_msgSend)(c, sel_registerName("contextWithSlot:"), 1) : nil;
		put(ctx, @"labelID", @"hk-sim-1");
		put(ctx, @"label", @"Primary");
		put(ctx, @"userDataPreferred", @YES);
		put(ctx, @"userDefaultVoice", @YES);
		put(ctx, @"isSimPresent", @YES);
		put(ctx, @"isSimGood", @YES);
	}
	return ctx;
}

static id
subscription_info(void)
{
	id info = make("CTXPCServiceSubscriptionInfo"), ctx = context();

	if (ctx != nil) {
		put(info, @"subscriptions", @[ctx]);
		put(info, @"subscriptionsInUse", @[ctx]);
		put(info, @"subscriptionsValid", @[ctx]);
	}
	return info;
}

static id
contexts(void)
{
	/*
	 * CTXPCContexts' subscriptions are CTXPCContextInfo, not the subscription context itself: with the context in the list SpringBoard
	 * could not decode the reply ("value for key 'NS.objects' was of unexpected class"), never had a SIM to ask about and showed SOS
	 * (2026-11-10; read from SpringBoard's own log, OS_ACTIVITY_DT_MODE=YES in its job).
	 */
	id all = make("CTXPCContexts"), ctx = context();
	Class c = objc_getClass("CTXPCContextInfo");
	NSUUID *uuid = [ctx valueForKey:@"uuid"] ?: [[NSUUID alloc] initWithUUIDString:@"00000000-0000-0000-0000-000000000001"];
	id info = c != Nil ? ((id (*)(Class, SEL, id, id, long long))objc_msgSend)(c, sel_registerName("contextWithUUID:andAccountID:andSlot:"), uuid, @"hk-sim-1", 1) : nil;

	put(info, @"labelID", @"hk-sim-1");
	put(info, @"label", @"Primary");
	if (info != nil) {
		put(all, @"subscriptions", @[info]);
	}
	put(all, @"dataPreferred", uuid);
	put(all, @"voicePreferred", uuid);
	return all;
}

static id
signal_info(void)
{
	id s = make("CTSignalStrengthInfo");
	int bars = now.registered ? now.bars : 0;

	put(s, @"bars", @(bars));
	put(s, @"displayBars", @(bars));
	put(s, @"maxDisplayBars", @4);
	return s;
}

static id
data_status(void)
{
	id d = make("CTDataStatus");

	/* (the numbers are the 8 Pro's, read there from the classes' own descriptions: radio technology LTE 7, indicator "4G" 7, bearer LTE 4) */
	put(d, @"attached", @(now.registered != 0));
	put(d, @"dataSim", @YES);
	put(d, @"indicator", @(now.registered ? 7 : 0));
	put(d, @"radioTechnology", @(now.registered ? 7 : 0));
	put(d, @"dataBearerTechnology", @(now.registered ? 4 : 0));
	put(d, @"cellularDataPossible", @(now.registered != 0));
	put(d, @"roamAllowed", @NO);
	put(d, @"inHomeCountry", @YES);
	put(d, @"activeContexts", @(now.data ? 1 : 0));
	put(d, @"totalActiveContexts", @(now.data ? 1 : 0));
	return d;
}

static NSString *
registration(void)
{
#ifdef ALWAYS_HOME
	return @"kCTRegistrationStatusRegisteredHome";         /* (an experiment: what the status bar does when it never hears anything else) */
#endif
	return now.registered ? @"kCTRegistrationStatusRegisteredHome" : @"kCTRegistrationStatusSearching";
}

static id
display_status(void)
{
	id s = make("CTRegistrationDisplayStatus");

	put(s, @"registrationDisplayStatus", registration());
	put(s, @"isInHomeCountry", @YES);
	return s;
}

static id
connection_status(void)
{
	id s = make("CTDataConnectionStatus");

	put(s, @"pdp", @0);
	put(s, @"interfaceName", @"en1");
	put(s, @"state", @(now.data ? 2 : 0));
	put(s, @"ipFamily", @1);
	return s;
}

static id
descriptor(void)
{
	Class c = objc_getClass("CTServiceDescriptor");
	id ctx = context();

	if (c == Nil || ctx == nil) {
		return nil;
	}
	return ((id (*)(Class, SEL, id))objc_msgSend)(c, sel_registerName("descriptorWithSubscriptionContext:"), ctx);
}

static id
rates(void)
{
	Class c = objc_getClass("CTSupportedMaxDataRates");

	return c != Nil ? ((id (*)(id, SEL, id))objc_msgSend)([c alloc], sel_registerName("initWithRates:"), @[@3, @4]) : (id)NSNull.null;
}

static id
nr_status(void)
{
	Class c = objc_getClass("CTNRStatus");

	return c != Nil ? ((id (*)(id, SEL, BOOL, unsigned long long, BOOL, unsigned long long))objc_msgSend)([c alloc],
	    sel_registerName("initWithSADisabled:saReasonMask:nsaDisabled:nsaReasonMask:"), NO, 0, NO, 0) : (id)NSNull.null;
}

static id
mcc(void)
{
	return facts.mcc >= 0 ? [NSString stringWithFormat:@"%03d", facts.mcc] : (id)NSNull.null;
}

static NSString *
oper(void)
{
	return now.oper[0] ? @(now.oper) : @"";
}

/*
 * What a selector's reply block is called with: objects (each goes to the first parameter of its class that is still
 * empty) and, under "n", numbers for the parameters that are not objects, in their order. Nothing here: nil and zero.
 */
static NSDictionary *
answers_for(NSString *name)
{
	static NSDictionary<NSString *, id (^)(void)> *table;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		table = @{
			@"getSubscriptionInfo:": ^{ return @{ @"o": @[subscription_info()] }; },
			@"getActiveContexts:": ^{ return @{ @"o": @[contexts()] }; },
			@"getCurrentDataSubscriptionContext:": ^{ return @{ @"o": @[context()] }; },
			@"getPreferredDataSubscriptionContext:": ^{ return @{ @"o": @[context()] }; },
			@"getUserDefaultVoiceSubscriptionContext:": ^{ return @{ @"o": @[context()] }; },
			@"getSIMStatus:completion:": ^{ return @{ @"o": @[@"kCTSIMSupportSIMStatusReady"] }; },
			@"getSIMTrayStatus:": ^{ return @{ @"o": @[@"kCTSIMSupportSIMTrayInsertedSIM"] }; },
			@"isAnySimReadyWithCompletion:": ^{ return @{ @"n": @[@1] }; },
			@"getSignalStrengthInfo:completion:": ^{ return @{ @"o": @[signal_info()] }; },
			@"getPublicSignalStrength:completion:": ^{ return @{ @"o": @[signal_info()] }; },
			@"copyRegistrationStatus:completion:": ^{ return @{ @"o": @[registration()] }; },
			@"copyRegistrationDisplayStatus:completion:": ^{ return @{ @"o": @[display_status()] }; },
			@"copyOperatorName:completion:": ^{ return @{ @"o": @[oper()] }; },
			@"copyLocalizedOperatorName:completion:": ^{ return @{ @"o": @[oper()] }; },
			@"copyAbbreviatedOperatorName:completion:": ^{ return @{ @"o": @[oper()] }; },
			@"copyIsInHomeCountry:completion:": ^{ return @{ @"n": @[@1], @"o": @[@YES] }; },
			@"copyIsDataAttached:completion:": ^{ return @{ @"n": @[@(now.registered)], @"o": @[@(now.registered != 0)] }; },
			@"copyRadioAccessTechnology:completion:": ^{ return @{ @"o": @[@"kCTRadioAccessTechnologyLTE"] }; },
			@"getDataStatus:completion:": ^{ return @{ @"o": @[data_status()] }; },
			@"getConnectionState:connectionType:completion:": ^{ return @{ @"o": @[connection_status()] }; },
			@"getInternationalDataAccess:completion:": ^{ return @{ @"n": @[@0] }; },
			@"getCurrentDataServiceDescriptor:": ^{ return @{ @"o": @[descriptor() ?: NSNull.null] }; },
			@"getPreferredDataServiceDescriptor:": ^{ return @{ @"o": @[descriptor() ?: NSNull.null] }; },
			@"getDualSimCapability:": ^{ return @{ @"n": @[@0] }; },
			/* the data rates of Settings' Voice & Data: CTDataRate 3 is 4G, 4 is 5G; the one chosen is the modem's mode preference (NR in it or not) */
			@"getSupportedDataRates:completion:": ^{ return @{ @"o": @[rates()] }; },
			@"getMaxDataRate:completion:": ^{ return @{ @"n": @[@(facts.mode_pref >= 0 && (facts.mode_pref & 0x40) == 0 ? 3 : 4)] }; },
			/* Settings greys 5G Auto / 5G On and the 5G Standalone switch out when these two are not answered (seen 2026-11-10): 5G is not disabled
			   (neither standalone nor non-standalone), and standalone is off for now (the switch is there; what it sets is not done yet) */
			@"getNRDisableStatus:completion:": ^{ return @{ @"o": @[nr_status()] }; },
			@"getSupports5GStandalone:completion:": ^{ return @{ @"o": @[@NO] }; },
			@"copyMobileCountryCode:completion:": ^{ return @{ @"o": @[mcc()] }; },
			@"copyLastKnownMobileCountryCode:completion:": ^{ return @{ @"o": @[mcc()] }; },
			@"copyMobileNetworkCode:completion:": ^{ return @{ @"o": @[facts.mnc >= 0 ? [NSString stringWithFormat:@"%02d", facts.mnc] : (id)NSNull.null] }; },
			@"smartDataMode:completion:": ^{ return @{ @"n": @[@0] }; },
			/* SMS can be sent and received (the kernel does both) */
			@"getSmsReadyState:completion:": ^{ return @{ @"n": @[@1], @"o": @[@YES] }; },
			@"getSupportDynamicDataSimSwitch:": ^{ return @{ @"n": @[@0] }; },
		};
	});
	id (^make_them)(void) = table[name];

	return make_them != nil ? make_them() : nil;
}

/* ---- the protocol: signatures, and what a reply block takes ---------------------------------------------------- */

static Protocol *service_protocol, *delegate_protocol;

static bool
protocol_method(Protocol *p, SEL sel, struct objc_method_description *out, const char **extended)
{
	unsigned count = 0;
	Protocol * __unsafe_unretained *parents;
	bool found = false;

	for (int required = 1; required >= 0 && !found; required--) {
		struct objc_method_description d = protocol_getMethodDescription(p, sel, required != 0, YES);

		if (d.name != NULL) {
			*out = d;
			if (extended != NULL) {
				*extended = _protocol_getMethodTypeEncoding(p, sel, required != 0, YES);
			}
			found = true;
		}
	}
	if (found) {
		return true;
	}
	parents = protocol_copyProtocolList(p, &count);
	for (unsigned i = 0; i < count && !found; i++) {
		found = protocol_method(parents[i], sel, out, extended);
	}
	free(parents);
	return found;
}

/* One type of an encoding at s: its bare form into type, the class named after an object into cls, what is in <...> into inner; returns where the next begins */
static const char *
next_type(const char *s, char *type, size_t type_size, char *cls, size_t cls_size, char *inner, size_t inner_size)
{
	size_t n = 0;

	type[0] = cls[0] = inner[0] = 0;
	while (*s && strchr("rnNoORV", *s) != NULL) {
		s++;
	}
	if (*s == 0) {
		return s;
	}
	if (*s == '{' || *s == '(' || *s == '[') {
		char open = *s, close = open == '{' ? '}' : open == '(' ? ')' : ']';
		int depth = 0;

		do {
			depth += *s == open ? 1 : *s == close ? -1 : 0;
			if (n + 1 < type_size) {
				type[n++] = *s;
			}
			s++;
		} while (*s && depth > 0);
	} else if (*s == '^') {
		type[n++] = *s++;
		if (*s) {
			type[n++] = *s++;
		}
	} else {
		type[n++] = *s++;
		if (type[0] == '@' && *s == '?') {
			type[n++] = *s++;
			if (*s == '<') {
				int depth = 0;
				size_t k = 0;

				do {
					depth += *s == '<' ? 1 : *s == '>' ? -1 : 0;
					if (depth > 0 && !(depth == 1 && *s == '<') && k + 1 < inner_size) {
						inner[k++] = *s;
					}
					s++;
				} while (*s && depth > 0);
				inner[k] = 0;
			}
		} else if (type[0] == '@' && *s == '"') {
			size_t k = 0;

			for (s++; *s && *s != '"'; s++) {
				if (k + 1 < cls_size) {
					cls[k++] = *s;
				}
			}
			cls[k] = 0;
			s += *s == '"';
		}
	}
	type[n] = 0;
	while (*s >= '0' && *s <= '9') {
		s++;
	}
	return s;
}

/* the signature a block has in its own descriptor, if it has one */
static const char *
block_signature(id block)
{
	struct literal {
		void *isa;
		int flags, reserved;
		void *invoke;
		struct { unsigned long reserved, size; void *rest[3]; } *descriptor;
	} *b = (__bridge void *)block;

	if (b == NULL || !(b->flags & (1 << 30))) {
		return NULL;
	}
	return b->descriptor->rest[(b->flags & (1 << 25)) ? 2 : 0];
}

static NSMutableDictionary<NSString *, NSNumber *> *seen;       /* "client selector" -> how often */
static NSMutableSet<NSString *> *described;

/* Call the reply block that is argument `index` of the request with the answers for the selector */
static void
answer(NSInvocation *request, NSUInteger index, const char *extended, NSString *name, bool describe)
{
	__unsafe_unretained id block = nil;
	char type[128], cls[96], inner[512] = "", bare[96] = "v@?";
	const char *s = extended;
	NSDictionary *have = answers_for(name);
	NSMutableArray *objects = [(have[@"o"] ?: @[]) mutableCopy], *numbers = [(have[@"n"] ?: @[]) mutableCopy];
	NSMutableArray *keep = [NSMutableArray array];
	NSMutableString *shape = [NSMutableString string];

	[request getArgument:&block atIndex:(NSInteger)index];
	if (block == nil) {
		return;
	}
	/* the block's parameter types: from the protocol's extended encoding (it names the classes), else from the block itself */
	if (s != NULL) {
		s = next_type(s, type, sizeof(type), cls, sizeof(cls), inner, sizeof(inner));          /* the return type */
		for (NSUInteger i = 0; i <= index && *s; i++) {
			s = next_type(s, type, sizeof(type), cls, sizeof(cls), inner, sizeof(inner));
		}
	}
	if (inner[0] == 0) {
		const char *own = block_signature(block);

		if (own == NULL) {
			say("%s: its reply block says nothing of its types: not called\n", name.UTF8String);
			return;
		}
		snprintf(inner, sizeof(inner), "%s", own);
	}
	/* inner: return type, the block itself, then the parameters */
	struct { char type[64], cls[96]; } params[12];
	unsigned count = 0;
	char sub[8];

	s = next_type(inner, type, sizeof(type), cls, sizeof(cls), sub, sizeof(sub));
	s = next_type(s, type, sizeof(type), cls, sizeof(cls), sub, sizeof(sub));
	while (*s && count < 12) {
		s = next_type(s, params[count].type, sizeof(params[count].type), params[count].cls, sizeof(params[count].cls), sub, sizeof(sub));
		if (params[count].type[0] == 0) {
			break;
		}
		strlcat(bare, params[count].type, sizeof(bare));
		[shape appendFormat:@" %s%s", params[count].type, params[count].cls];
		count++;
	}
	if (describe) {
		say("  %s replies with:%s%s\n", name.UTF8String, shape.UTF8String, have != nil ? "  (answered)" : "");
	}
	NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:bare];
	NSInvocation *call = [NSInvocation invocationWithMethodSignature:signature];

	for (unsigned i = 0; i < count; i++) {
		const char *t = params[i].type;

		if (t[0] == '@' && t[1] != '?') {
			Class want = params[i].cls[0] ? objc_getClass(params[i].cls) : Nil;
			__unsafe_unretained id value = nil;

			for (NSUInteger k = 0; k < objects.count; k++) {
				if (objects[k] == NSNull.null) {
					continue;       /* (a value that could not be made) */
				}
				if (want == Nil ? ![objects[k] isKindOfClass:NSError.class] : [objects[k] isKindOfClass:want]) {
					value = objects[k];
					[keep addObject:objects[k]];       /* (NSInvocation does not retain what it is handed) */
					[objects removeObjectAtIndex:k];
					break;
				}
			}
			[call setArgument:&value atIndex:(NSInteger)i + 1];
		} else if (strchr("cCBsSiIlLqQ", t[0]) != NULL && numbers.count > 0) {
			long long v = [numbers[0] longLongValue];

			[numbers removeObjectAtIndex:0];
			[call setArgument:&v atIndex:(NSInteger)i + 1];
		}
		/* (everything else stays as the invocation made it: zero) */
	}
	if ([name hasPrefix:@"copyRegistration"] || [name hasPrefix:@"getSIMStatus"] || [name hasPrefix:@"getSignalStrength"]) {
		static NSMutableSet *said;
		NSString *what = [NSString stringWithFormat:@"%@ -> %@", name, [keep.firstObject description]];

		if (said == nil) {
			said = [NSMutableSet set];
		}
		@synchronized (said) {
			if (![said containsObject:what] && said.count < 60) {
				[said addObject:what];
				say("  answered %s\n", what.UTF8String);
			}
		}
	}
	@try {
		[call invokeWithTarget:block];
	} @catch (NSException *e) {
		say("%s: calling its reply block raised %s\n", name.UTF8String, e.name.UTF8String);
	}
	(void)keep.count;
}

/* ---- the NSXPC service ------------------------------------------------------------------------------------------------- */

@interface Responder : NSObject
@property (weak) NSXPCConnection *connection;
@property (copy) NSString *client;
@property BOOL registered;
@property (strong) NSMutableSet<NSString *> *wants;      /* the delegate selectors it registered for */
- (void)tell;
- (void)tellOne:(NSString *)selector arguments:(NSArray *)arguments;
@end

static NSMutableArray<Responder *> *responders;

static NSString *
process_name(pid_t pid)
{
	char name[64] = "";
	int mib[4] = { CTL_KERN, KERN_PROCARGS2, pid, 0 };
	static char buffer[4096];
	size_t size = sizeof(buffer);

	if (sysctl(mib, 3, buffer, &size, NULL, 0) == 0 && size > sizeof(int)) {
		const char *path = buffer + sizeof(int), *slash = strrchr(path, '/');

		snprintf(name, sizeof(name), "%s", slash != NULL ? slash + 1 : path);
	}
	return name[0] ? @(name) : [NSString stringWithFormat:@"pid%d", pid];
}

@implementation Responder

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	struct objc_method_description d;

	if (protocol_method(service_protocol, sel, &d, NULL)) {
		return [NSMethodSignature signatureWithObjCTypes:d.types];
	}
	return [super methodSignatureForSelector:sel];
}

- (BOOL)respondsToSelector:(SEL)sel
{
	struct objc_method_description d;

	return [super respondsToSelector:sel] || protocol_method(service_protocol, sel, &d, NULL);
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	NSString *name = NSStringFromSelector(invocation.selector), *key = [NSString stringWithFormat:@"%@ %@", self.client, name];
	NSMethodSignature *signature = invocation.methodSignature;
	struct objc_method_description d;
	const char *extended = NULL;
	unsigned count;
	bool describe;

	@synchronized (seen) {
		count = seen[key].unsignedIntValue + 1;
		seen[key] = @(count);
		describe = ![described containsObject:name];
		[described addObject:name];
	}
	if (count <= 2) {
		say("%s asks %s%s\n", self.client.UTF8String, name.UTF8String, count == 2 ? " (again; from now on only counted)" : "");
	}
	protocol_method(service_protocol, invocation.selector, &d, &extended);
	if ([name isEqualToString:@"registerForNotifications:completion:"]) {
		__unsafe_unretained id what = nil;

		[invocation getArgument:&what atIndex:2];
		if (count <= 2) {
			say("  %s registers with a %s%s\n", self.client.UTF8String, what != nil ? class_getName([what class]) : "nil",
			    [what respondsToSelector:@selector(count)] ? [NSString stringWithFormat:@" of %lu", (unsigned long)[what count]].UTF8String : "");
			if ([what isKindOfClass:NSArray.class] || [what isKindOfClass:NSSet.class]) {
				NSMutableString *names = [NSMutableString string];

				for (id one in what) {
					[names appendFormat:@" %@", [one isKindOfClass:NSString.class] ? one : NSStringFromClass([one class])];
				}
				say("  they are:%s\n", names.length < 1500 ? names.UTF8String : [names substringToIndex:1500].UTF8String);
			}
		}
		if (self.wants == nil) {
			self.wants = [NSMutableSet set];
		}
		if ([what isKindOfClass:NSArray.class] || [what isKindOfClass:NSSet.class]) {
			for (id one in what) {
				if ([one isKindOfClass:NSString.class]) {
					[self.wants addObject:one];
				}
			}
		}
		self.registered = YES;
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [self tell]; });
	}
	if ([name isEqualToString:@"setMaxDataRate:rate:completion:"] && signature.numberOfArguments >= 5) {
		long long rate = 0;
		int result = -1, mode;
		uint8_t tlv[5] = { 0x11, 0x02, 0x00, 0, 0 };

		[invocation getArgument:&rate atIndex:3];
		read_facts(false);
		mode = ((facts.mode_pref >= 0 ? facts.mode_pref : 0x1c) & ~0x40) | 0x10 | (rate >= 4 ? 0x40 : 0);
		tlv[3] = (uint8_t)mode;
		tlv[4] = (uint8_t)(mode >> 8);
		qmi(3, 0x33, tlv, sizeof(tlv), &result);
		read_facts(false);
		say("%s chose data rate %lld: the modem's mode preference asked to be %#x: %s (%d); it reads %#x now\n", self.client.UTF8String, rate, mode,
		    result == 0 ? "done" : "NOT done", result, facts.mode_pref);
	}
	for (NSUInteger i = 2; i < signature.numberOfArguments; i++) {
		if (strncmp([signature getArgumentTypeAtIndex:i], "@?", 2) == 0) {
			answer(invocation, i, extended, name, describe);
		}
	}
}

/* The delegate calls a client waits for before it believes anything: everything, with the numbers as they are now */
/* one delegate call to this client, if it registered for it: objects as they are, numbers as 64-bit values */
- (void)tellOne:(NSString *)selector arguments:(NSArray *)arguments
{
	NSXPCConnection *c = self.connection;
	SEL sel = NSSelectorFromString(selector);
	struct objc_method_description d;

	if (c == nil || !self.registered || ![self.wants containsObject:selector] || !protocol_method(delegate_protocol, sel, &d, NULL)) {
		return;
	}
	id proxy = [c remoteObjectProxyWithErrorHandler:^(NSError *error) {
		say("%s to %s failed: %s %ld\n", selector.UTF8String, self.client.UTF8String, error.domain.UTF8String, (long)error.code);
	}];
	NSInvocation *call = [NSInvocation invocationWithMethodSignature:[NSMethodSignature signatureWithObjCTypes:d.types]];

	call.selector = sel;
	for (NSUInteger i = 0; i < arguments.count && i + 2 < call.methodSignature.numberOfArguments; i++) {
		const char *t = [call.methodSignature getArgumentTypeAtIndex:i + 2];

		if (t[0] == '@') {
			__unsafe_unretained id value = arguments[i] == NSNull.null ? nil : arguments[i];

			[call setArgument:&value atIndex:(NSInteger)i + 2];
		} else {
			long long v = [arguments[i] longLongValue];

			[call setArgument:&v atIndex:(NSInteger)i + 2];
		}
	}
	say("%s is told %s\n", self.client.UTF8String, selector.UTF8String);
	@try {
		[call invokeWithTarget:proxy];
	} @catch (NSException *e) {
		say("telling %s %s raised %s\n", self.client.UTF8String, selector.UTF8String, e.name.UTF8String);
	}
}

- (void)tell
{
	NSXPCConnection *c = self.connection;

	if (c == nil || !self.registered) {
		return;
	}
	id proxy = [c remoteObjectProxyWithErrorHandler:^(NSError *error) {
		static unsigned said;

		if (said++ < 6) {
			say("a notification to %s failed: %s %ld\n", self.client.UTF8String, error.domain.UTF8String, (long)error.code);
		}
	}];
	id ctx = context();
	void (^one)(NSString *, NSArray *) = ^(NSString *selector, NSArray *arguments) {
		SEL sel = NSSelectorFromString(selector);
		struct objc_method_description d;

		if (![self.wants containsObject:selector] || !protocol_method(delegate_protocol, sel, &d, NULL)) {
			return;
		}
		{
			NSString *key = [NSString stringWithFormat:@"%@ told %@", self.client, selector];
			unsigned told;

			@synchronized (seen) {
				told = seen[key].unsignedIntValue + 1;
				seen[key] = @(told);
			}
			if (told == 1) {
				say("%s is told %s\n", self.client.UTF8String, selector.UTF8String);
			}
		}
		NSInvocation *call = [NSInvocation invocationWithMethodSignature:[NSMethodSignature signatureWithObjCTypes:d.types]];

		call.selector = sel;
		for (NSUInteger i = 0; i < arguments.count && i + 2 < call.methodSignature.numberOfArguments; i++) {
			const char *t = [call.methodSignature getArgumentTypeAtIndex:i + 2];

			if (t[0] == '@') {
				__unsafe_unretained id value = arguments[i] == NSNull.null ? nil : arguments[i];

				[call setArgument:&value atIndex:(NSInteger)i + 2];
			} else {
				long long v = [arguments[i] longLongValue];

				[call setArgument:&v atIndex:(NSInteger)i + 2];
			}
		}
		@try {
			[call invokeWithTarget:proxy];
		} @catch (NSException *e) {
			say("telling %s %s raised %s\n", self.client.UTF8String, selector.UTF8String, e.name.UTF8String);
		}
	};

	if (ctx == nil) {
		return;
	}
	one(@"subscriptionInfoDidChange", @[]);
	one(@"activeSubscriptionsDidChange", @[]);
	one(@"simStatusDidChange:status:", @[ctx, @"kCTSIMSupportSIMStatusReady"]);
	one(@"displayStatusChanged:status:", @[ctx, display_status()]);
	one(@"operatorNameChanged:name:", @[ctx, oper()]);
	one(@"signalStrengthChanged:info:", @[ctx, signal_info()]);
	one(@"currentDataSimChanged:", @[ctx]);
	one(@"preferredDataSimChanged:", @[ctx]);
	one(@"dataStatus:dataStatusInfo:", @[ctx, data_status()]);
	one(@"connectionStateChanged:connection:dataConnectionStatusInfo:", @[ctx, @0, connection_status()]);
}

@end

@interface Listener : NSObject <NSXPCListenerDelegate>
@end

@implementation Listener

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	Responder *r = [Responder new];
	static unsigned connections;

	r.connection = connection;
	r.client = process_name(connection.processIdentifier);
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:service_protocol];
	connection.exportedObject = r;
	connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:delegate_protocol];
	__weak Responder *weak = r;
	connection.invalidationHandler = ^{
		Responder *gone = weak;

		if (gone != nil) {
			@synchronized (responders) {
				[responders removeObject:gone];
			}
		}
	};
	@synchronized (responders) {
		[responders addObject:r];
	}
	connections++;
	if (connections <= 40 || connections % 500 == 0) {
		say("connection %u: %s\n", connections, r.client.UTF8String);
	}
	[connection resume];
	return YES;
}

@end

/* ---- the older API: plain XPC dictionaries ------------------------------------------------------------------------- */

/* ---- SMS: the kernel's messages for CoreTelephony's clients (imagent's SMS service is the one that matters) ---------- */

/*
 * How iOS 16.5 moves a message (read from CoreTelephony, 2026-11-11; nothing here is modified in iOS):
 *  - a message arrives: CommCenter calls the delegates' smsMessageReceived:msgID: (context, a number). The client then asks for it over the older
 *    interface, kSmsRetrieveMessage {kSmsMessageID}, and the reply carries kSmsMessageBody (data), kSmsMessageType (1 SMS, 2 MMS), kSmsSubInstance
 *    (the slot), kSmsMobileCountryCode, kSmsReplyEnabled; later kSmsAcknowledgeMessage.
 *  - the body is what +[CTMMSEncoder decodeSmsFromData:data:] reads: the date as a WSP integer (one byte with bit 7 set up to 127, else a length byte
 *    and big-endian bytes), then four strings each ended by a zero: the sender, the text, the service centre, an identifier (a UUID as text); a last
 *    integer ("replace") may follow.
 *  - a send: kSmsSendMessage {kSmsMessageID, kSmsMessageType, kSmsMessageBody, kSmsMoreMessages, kSmsSubInstance, kSmsType}; the body is
 *    +[CTMMSEncoder encodeSms:]'s, the same layout with the recipient where the sender is. The reply's kSmsMessageResult (a boolean: accepted) is read; the outcome comes as
 *    the delegates' postSMSMessageSent:success:messageID:err1:err2:.
 * The radio's side is the kernel's: kern.hackios.modem_sms_send ("DIGITS TEXT"), modem_sms_status, modem_sms_ring (index, a tab, "NUMBER: TEXT").
 */
static NSMutableDictionary<NSNumber *, NSDictionary *> *sms_in;      /* our message number -> { from, text, date } until it is acknowledged */
static unsigned sms_taken;                                            /* how many of the kernel's messages have been handed on */
static long long sms_next = 1;

static void
tell_all(NSString *selector, NSArray *arguments)
{
	NSArray *all;

	@synchronized (responders) {
		all = [responders copy];
	}
	for (Responder *r in all) {
		[r tellOne:selector arguments:arguments];
	}
}

static void
wsp_integer(NSMutableData *d, uint32_t v)
{
	uint8_t b[5];
	int n = v > 0xffffff ? 4 : v > 0xffff ? 3 : v > 0xff ? 2 : 1;

	if (v <= 0x7f) {
		b[0] = (uint8_t)(v | 0x80);
		[d appendBytes:b length:1];
		return;
	}
	b[0] = (uint8_t)n;
	for (int i = 0; i < n; i++) {
		b[1 + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
	}
	[d appendBytes:b length:1 + (NSUInteger)n];
}

static void
wsp_string(NSMutableData *d, NSString *s)
{
	[d appendData:[s dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data]];
	[d appendBytes:"" length:1];
}

static NSData *
sms_body(NSDictionary *m)
{
	NSMutableData *d = [NSMutableData data];

	wsp_integer(d, (uint32_t)[m[@"date"] timeIntervalSince1970]);
	wsp_string(d, m[@"from"]);
	wsp_string(d, m[@"text"]);
	wsp_string(d, @"");
	wsp_string(d, [NSUUID UUID].UUIDString);
	return d;
}

/* the recipient and the text of a client's send */
static bool
sms_parse(const uint8_t *b, size_t n, NSString **to, NSString **text)
{
	size_t at = 0;
	NSMutableArray<NSString *> *strings = [NSMutableArray array];

	if (n == 0) {
		return false;
	}
	at = (b[0] & 0x80) ? 1 : 1 + (size_t)b[0];
	while (at < n && strings.count < 4) {
		size_t end = at;

		while (end < n && b[end] != 0) {
			end++;
		}
		[strings addObject:[[NSString alloc] initWithBytes:b + at length:end - at encoding:NSUTF8StringEncoding] ?: @""];
		at = end + 1;
	}
	if (strings.count < 2) {
		return false;
	}
	*to = strings[0];
	*text = strings[1];
	return true;
}

/* the kernel's new messages, each given a number and announced */
static void
sms_poll(void)
{
	static char ring[8 * 2780];     /* (the kernel's eight entries of up to 2760 bytes, a long message's parts joined; each with its number and a tab) */
	size_t n = sizeof(ring) - 1;
	id ctx = context();

	if (ctx == nil || sysctlbyname("kern.hackios.modem_sms_ring", ring, &n, NULL, 0) != 0) {
		return;
	}
	ring[n < sizeof(ring) ? n : sizeof(ring) - 1] = 0;
	for (char *line = ring, *next; line != NULL && *line != 0; line = next) {
		char *end = NULL, *colon;
		unsigned long index;

		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = 0;
		}
		index = strtoul(line, &end, 10);
		if (end == NULL || *end != '\t' || index < sms_taken || (colon = strstr(end + 1, ": ")) == NULL) {
			continue;
		}
		sms_taken = (unsigned)index + 1;
		*colon = 0;
		for (char *c = colon + 2; *c != 0; c++) {
			if (*c == 0x1f) {
				*c = '\n';
			}
		}
		NSString *from = @(end + 1) ?: @"", *text = [NSString stringWithUTF8String:colon + 2] ?: @"";
		long long number = sms_next++;

		if (sms_in == nil) {
			sms_in = [NSMutableDictionary dictionary];
		}
		sms_in[@(number)] = @{ @"from": from, @"text": text, @"date": [NSDate date] };
		say("sms: the kernel's message %lu (a sender of %lu characters, %lu of text) is message %lld for iOS\n", index, (unsigned long)from.length, (unsigned long)text.length, number);
		tell_all(@"smsMessageReceived:msgID:", @[ctx, @(number)]);
	}
}

static long long
legacy_number(xpc_object_t message, const char *key)
{
	xpc_object_t v = xpc_dictionary_get_value(message, key);

	return v == nil ? -1 : xpc_get_type(v) == XPC_TYPE_UINT64 ? (long long)xpc_uint64_get_value(v) : xpc_get_type(v) == XPC_TYPE_INT64 ? xpc_int64_get_value(v) : -1;
}

/* the SMS requests of the older interface: true when this one was answered here */
static bool
legacy_sms(xpc_connection_t peer, xpc_object_t message, xpc_object_t reply, const char *request, NSString *client)
{
	if (strcmp(request, "kSmsRetrieveMessage") == 0) {
		long long number = legacy_number(message, "kSmsMessageID");
		NSDictionary *m = sms_in[@(number)];

		say("sms: %s takes message %lld: %s\n", client.UTF8String, number, m != nil ? "given" : "not here");
		if (m != nil) {
			NSData *body = sms_body(m);

			xpc_dictionary_set_data(reply, "kSmsMessageBody", body.bytes, body.length);
			xpc_dictionary_set_int64(reply, "kSmsMessageType", 1);
			xpc_dictionary_set_int64(reply, "kSmsSubInstance", 1);
			xpc_dictionary_set_string(reply, "kSmsMobileCountryCode", "hk");
			xpc_dictionary_set_bool(reply, "kSmsReplyEnabled", true);
		}
		xpc_dictionary_set_int64(reply, "kSmsMessageID", number);
		return true;
	}
	if (strcmp(request, "kSmsAcknowledgeMessage") == 0) {
		long long number = legacy_number(message, "kSmsMessageID");

		say("sms: %s acknowledges message %lld\n", client.UTF8String, number);
		[sms_in removeObjectForKey:@(number)];
		return true;
	}
	if (strcmp(request, "kSmsGetIncomingMessageCount") == 0) {
		xpc_dictionary_set_int64(reply, "kSmsIncomingMessageCount", (int64_t)sms_in.count);
		return true;
	}
	if (strcmp(request, "kSmsGetRecievedMessageIds") == 0) {
		xpc_object_t ids = xpc_array_create(NULL, 0), none = xpc_array_create(NULL, 0);

		for (NSNumber *k in [sms_in.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
			xpc_array_set_int64(ids, XPC_ARRAY_APPEND, k.longLongValue);
		}
		xpc_dictionary_set_value(reply, "kSmsRecievedSmsMessageIds", ids);
		xpc_dictionary_set_value(reply, "kSmsRecievedMmsMessageIds", none);
		return true;
	}
	if (strcmp(request, "kSmsIsMmsConfigured") == 0) {
		xpc_dictionary_set_bool(reply, "kSmsMmsIsConfigured", false);
		return true;
	}
	if (strcmp(request, "kSmsGetCharacterCountAndThresholdForMessage") == 0) {
		const char *text = xpc_dictionary_get_string(message, "kSmsMessageBody");
		NSString *t = text != NULL ? @(text) : nil;
		bool plain = t == nil || [t canBeConvertedToEncoding:NSASCIIStringEncoding];

		xpc_dictionary_set_bool(reply, "kDidSucceed", true);
		xpc_dictionary_set_int64(reply, "kSmsCharacterCount", (int64_t)t.length);
		xpc_dictionary_set_int64(reply, "kSmsThreshold", plain ? 160 : 70);
		return true;
	}
	if (strcmp(request, "kSmsSendMessage") == 0) {
		size_t length = 0;
		const uint8_t *body = xpc_dictionary_get_data(message, "kSmsMessageBody", &length);
		long long number = legacy_number(message, "kSmsMessageID"), type = legacy_number(message, "kSmsMessageType");
		NSString *to = nil, *text = nil;
		NSMutableString *digits = [NSMutableString string];
		id ctx = context();
		int e = -1;

		if (body != NULL && type != 2 && sms_parse(body, length, &to, &text)) {
			for (NSUInteger i = 0; i < to.length; i++) {
				unichar c = [to characterAtIndex:i];

				if (c >= '0' && c <= '9') {
					[digits appendFormat:@"%C", c];
				}
			}
			/* (the kernel sends to an international number: a local one of eight digits gets this network's country, Hong Kong's) */
			if (![to hasPrefix:@"+"] && digits.length == 8) {
				[digits insertString:@"852" atIndex:0];
			}
			if (digits.length >= 3 && text.length > 0) {
				const char *line = [NSString stringWithFormat:@"%@ %@", digits, text].UTF8String;

				e = sysctlbyname("kern.hackios.modem_sms_send", NULL, NULL, (void *)line, strlen(line));
			}
		}
		say("sms: %s sends message %lld (type %lld, %lu bytes: a recipient of %lu digits, %lu characters of text): %s\n", client.UTF8String, number, type,
		    (unsigned long)length, (unsigned long)digits.length, (unsigned long)text.length, e == 0 ? "handed to the kernel" : "NOT sent");
		/* (the client reads this as a boolean: anything else is "No message result for message ID" and the message is shown Not Delivered at once,
		   although it went out: measured 2026-11-11) */
		xpc_dictionary_set_bool(reply, "kSmsMessageResult", e == 0);
		xpc_dictionary_set_int64(reply, "kSmsMessageID", number);
		/* the outcome, when the kernel knows it: its state goes from "queued" to "sent ..." or "failed ..." */
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
			bool sent = false;

			for (int i = 0; e == 0 && i < 80; i++) {
				char status[128] = "";
				size_t n = sizeof(status) - 1;

				usleep(500000);
				if (sysctlbyname("kern.hackios.modem_sms_status", status, &n, NULL, 0) == 0 && strncmp(status, "queued", 6) != 0) {
					sent = strncmp(status, "sent", 4) == 0;
					break;
				}
			}
			dispatch_async(dispatch_get_main_queue(), ^{
				say("sms: message %lld: %s\n", number, sent ? "sent" : "not sent");
				if (ctx != nil) {
					tell_all(@"postSMSMessageSent:success:messageID:err1:err2:", @[ctx, @(sent), @(number), @0, @0]);
				}
			});
		});
		return true;
	}
	return false;
}

static void
legacy_message(xpc_connection_t peer, xpc_object_t message, NSString *client)
{
	const char *request = xpc_dictionary_get_string(message, "kRequest");
	xpc_object_t reply = xpc_dictionary_create_reply(message);
	NSString *key = [NSString stringWithFormat:@"%@ legacy %s", client, request ?: "(no kRequest)"];
	unsigned count;
	NSMutableString *keys = [NSMutableString string];

	@synchronized (seen) {
		count = seen[key].unsignedIntValue + 1;
		seen[key] = @(count);
	}
	if (reply == NULL) {
		if (count <= 2) {
			say("%s sends %s and wants no reply\n", client.UTF8String, request ?: "(no kRequest)");
		}
		if (request != NULL && strcmp(request, "kRegisterNotification") == 0 && count <= 40) {
			/* (which notifications of the older interface a client asks for: named here once, to know what they wait for) */
			xpc_dictionary_apply(message, ^bool (const char *name, xpc_object_t value) {
				if (strcmp(name, "kRequest") != 0) {
					say("  %s registers %s = %s\n", client.UTF8String, name, xpc_get_type(value) == XPC_TYPE_STRING ? xpc_dictionary_get_string(message, name) : "(not a string)");
				}
				return true;
			});
		}
		return;
	}
	if (request != NULL && strncmp(request, "kSms", 4) == 0 && legacy_sms(peer, message, reply, request, client)) {
		if (count <= 4) {
			say("%s legacy %s: answered\n", client.UTF8String, request);
		}
		xpc_connection_send_message(peer, reply);
		return;
	}
	xpc_dictionary_apply(message, ^bool (const char *name, xpc_object_t value) {
		xpc_type_t type = xpc_get_type(value);

		[keys appendFormat:@" %s(%s)", name, type == XPC_TYPE_BOOL ? "bool" : type == XPC_TYPE_INT64 ? "int" : type == XPC_TYPE_STRING ? "string" :
		    type == XPC_TYPE_DICTIONARY ? "dictionary" : type == XPC_TYPE_ARRAY ? "array" : type == XPC_TYPE_DATA ? "data" : "other"];
		if (strcmp(name, "kRequest") == 0) {
			return true;
		}
		/* the answers known here; everything else goes back as it came */
		if (strcmp(name, "kRadioModuleIsDead") == 0) {
			type == XPC_TYPE_BOOL ? xpc_dictionary_set_bool(reply, name, false) : xpc_dictionary_set_int64(reply, name, 0);
		} else if (strcmp(name, "kCellularDataHideIndicator") == 0 || strcmp(name, "kCellularDataHideRatIndicator") == 0) {
			/*
			 * Said outright: the status bar's side starts from "hidden" (SystemStatusServer, -_internalQueue_isCellularRadioCapabilityActive:
			 * its out value begins as 1) and a reply that only hands the placeholder back leaves it there. A hidden indicator makes the
			 * subscription a "bootstrap" one, and a registered bootstrap subscription is shown as SOS (2026-11-10).
			 */
			xpc_dictionary_set_bool(reply, name, false);
		} else if (strcmp(name, "kCellularDataEnabled") == 0 || strcmp(name, "kEnableCellularData") == 0) {
			type == XPC_TYPE_BOOL ? xpc_dictionary_set_bool(reply, name, true) : xpc_dictionary_set_int64(reply, name, 1);
		} else {
			xpc_dictionary_set_value(reply, name, value);
		}
		return true;
	});
	if (request != NULL && strcmp(request, "kSubscriberGetSimState") == 0) {
		xpc_dictionary_set_string(reply, "kSubscriberSimState", "kCTSIMSupportSIMStatusReady");
	}
	if (request != NULL && strcmp(request, "kRadioModuleGetProperty") == 0) {
		/* (CoreTelephony's GetRadioModuleProperty reads the value only if the reply says it is valid; without this SpringBoard asked
		 * kRadioModuleIsDead every second and showed SOS, 2026-11-10) */
		xpc_dictionary_set_bool(reply, "kRadioModuleValid", true);
	}
	if (count <= 2) {
		say("%s legacy %s:%s\n", client.UTF8String, request ?: "(no kRequest)", keys.length < 600 ? keys.UTF8String : [keys substringToIndex:600].UTF8String);
	}
	xpc_connection_send_message(peer, reply);
}

/*
 * (The listener is kept for good: under ARC a local would be released when this returns, and then the name is still launchd's to hand out
 * but nobody reads what arrives: SpringBoard's first synchronous request waited for ever and the phone stayed at its boot logo, 2026-11-10.)
 */
static NSMutableArray *legacy_listeners;

static void
legacy_listen(const char *service)
{
	xpc_connection_t listener = xpc_connection_create_mach_service(service, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == nil) {
		say("no listener for %s\n", service);
		return;
	}
	if (legacy_listeners == nil) {
		legacy_listeners = [NSMutableArray array];
	}
	[legacy_listeners addObject:listener];

	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		NSString *client = process_name(xpc_connection_get_pid(peer));

		{
			static unsigned peers;

			if (++peers <= 30) {
				say("legacy connection %u: %s\n", peers, client.UTF8String);
			}
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) == XPC_TYPE_DICTIONARY) {
				legacy_message(peer, message, client);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
}

/* ---- */

/*
 * A newer build of this program put on a running phone (`iphont put FILE hk_commcenter.N` lands in /private/var/root/iphont; the root is read
 * only) takes over at the next start: the highest N. launchd starts the job again when the old process is ended and somebody asks.
 */
/*
 * ---- Calls ---------------------------------------------------------------------------------------------------------
 *
 * callservicesd does not ask CoreTelephony about calls: on an iPhone CommCenter is a CallKit provider, the one whose
 * identifier is com.apple.coretelephony ("Telephony"), and so is this daemon (as the OnePlus 8 Pro port's notes describe
 * theirs, docs/research/modem-sdx55.md there, "Calls through CallKit": written from that description). What makes
 * callservicesd take it for that provider is in the build: CFBundleIdentifier com.apple.coretelephony in the program's
 * __info_plist section (src/hk_commcenter.info.plist) and the entitlements application-identifier (the same name) and
 * com.apple.callkit = private-provider-api, private-controller-api (src/hk_commcenter.entitlements, ldid).
 *
 * The modem is the kernel's: kern.hackios.modem_call takes "dial NUMBER", "answer ID", "end ID", "dtmf ID DIGITS" and says
 * how that went; kern.hackios.modem_call_events lists every change of a call's state (N ID STATE DIRECTION TYPE MODE REASON
 * NUMBER; states: 1 origination, 2 incoming, 3 conversation, 4 call control in progress, 5 alerting, 6 hold, 7 waiting,
 * 8 disconnecting, 9 end, 10 setup; direction 1 from here, 2 to here). This side reads the events twice a second and tells
 * CallKit; CallKit's actions (the Phone application's button, Accept, the red button) become requests.
 *
 * A call that is not on the air, for looking at iOS's side alone: "hk_commcenter call fake NUMBER" leaves a note that the
 * daemon takes at its next look: an incoming call that Accept "connects" and that either side's end ends (id 100).
 *
 * The other party's number is never logged: its length is.
 */
#define CALL_FAKE 100
#define CALL_FAKE_NOTE "/private/var/root/hk_call_fake"

static CXProvider *call_provider;
static NSMutableDictionary<NSNumber *, NSUUID *> *call_uuid;     /* the modem's id of a call -> CallKit's */
static NSMutableDictionary<NSUUID *, NSNumber *> *call_number;   /* and back */
static NSMutableDictionary<NSUUID *, NSNumber *> *call_state;    /* the modem's last state of it */
static NSMutableSet<NSUUID *> *call_ending;                      /* ended from this side: its end is not news for CallKit */
static NSMutableSet<NSUUID *> *call_was_connected;
static NSMutableSet<NSUUID *> *call_starting;                    /* iOS's start action is not answered yet */
static NSMutableSet<NSUUID *> *call_given_up;                    /* ... and iOS has ended the call meanwhile */
static NSUUID *call_dialling;                                    /* dialled, and the modem has not said its id yet */
static bool call_abandoned;                                      /* iOS ended a call before the modem numbered it: ended when its number is known */
static long call_taken = -1;                                     /* events read so far (-1: none looked at yet) */
static dispatch_queue_t call_queue;

/* one request to the modem and how it went (up to 12 s: the kernel's thread carries it out between its other work) */
static NSString *
call_request(NSString *text)
{
	const char *c = text.UTF8String;
	char status[120];
	int e = -1;

	for (int i = 0; i < 20 && (e = sysctlbyname("kern.hackios.modem_call", NULL, NULL, (void *)c, strlen(c))) != 0 && errno == EBUSY; i++) {
		usleep(100000);
	}
	if (e != 0) {
		return [NSString stringWithFormat:@"failed (the kernel does not take it: %s)", strerror(errno)];
	}
	for (int i = 0; i < 120; i++) {
		size_t n = sizeof(status) - 1;

		usleep(100000);
		if (sysctlbyname("kern.hackios.modem_call", status, &n, NULL, 0) == 0) {
			status[n < sizeof(status) ? n : sizeof(status) - 1] = 0;
			if (strcmp(status, "queued") != 0) {
				return @(status);
			}
		}
	}
	return @"failed (no outcome in 12 s)";
}

/*
 * The sound of a call is the kernel's (bsd/dev/hk_mic.c, boot-arg voice=3: the audio DSP and the modem exchange it between themselves once this side has
 * set their sessions up): kern.hackios.voice_call 1 while a call is ringing at the other end or connected, 0 when none is. Without voice=3 the kernel
 * refuses the write, and a call is silent.
 */
static NSMutableSet<NSNumber *> *call_sounding;

static void
call_sound(unsigned number, bool on)
{
	static int was = -1;
	int value;

	if (call_sounding == nil) {
		call_sounding = [NSMutableSet set];
	}
	if (on) {
		[call_sounding addObject:@(number)];
	} else {
		[call_sounding removeObject:@(number)];
	}
	value = call_sounding.count != 0 ? 1 : 0;
	if (value != was) {
		int e = sysctlbyname("kern.hackios.voice_call", NULL, NULL, &value, sizeof(value));

		was = value;
		say("call: the sound's path asked %s: %s\n", value ? "up" : "down", e == 0 ? "taken" : strerror(errno));
	}
}

static void
call_forget(NSUUID *u)
{
	NSNumber *n = call_number[u];

	if (n != nil) {
		[call_uuid removeObjectForKey:n];
	}
	[call_number removeObjectForKey:u];
	[call_state removeObjectForKey:u];
	[call_ending removeObject:u];
	[call_was_connected removeObject:u];
}

static void
call_bind(unsigned number, NSUUID *u)
{
	call_uuid[@(number)] = u;
	call_number[u] = @(number);
}

static void
call_incoming(unsigned number, NSString *from)
{
	NSUUID *u = [NSUUID UUID];
	CXCallUpdate *update = [CXCallUpdate new];

	call_bind(number, u);
	call_state[u] = @2;
	if (from.length > 0 && ![from isEqualToString:@"-"]) {
		update.remoteHandle = [[CXHandle alloc] initWithType:CXHandleTypePhoneNumber value:from];
	}
	update.hasVideo = NO;
	update.supportsHolding = NO;
	update.supportsGrouping = NO;
	update.supportsUngrouping = NO;
	update.supportsDTMF = YES;
	say("call: %u comes in (a number of %lu characters): CallKit is told\n", number, (unsigned long)from.length);
	[call_provider reportNewIncomingCallWithUUID:u update:update completion:^(NSError *error) {
		dispatch_async(dispatch_get_main_queue(), ^{
			if (error != nil) {
				/* (CallKit's reasons: 1 unentitled, 2 the UUID exists, 3 Do Not Disturb, 4 blocked: the call goes on in the modem and nobody sees it) */
				say("call: %u: iOS does not take it: %s code %ld\n", number, error.domain.UTF8String, (long)error.code);
				call_forget(u);
			} else {
				say("call: %u: iOS took it (the banner, the ringtone)\n", number);
			}
		});
	}];
}

/* one line of the kernel's events */
static void
call_event(unsigned number, unsigned state, unsigned direction, unsigned type, unsigned mode, unsigned reason, NSString *party)
{
	NSUUID *u = call_uuid[@(number)];

	say("call: the modem: call %u state %u, direction %u, type %u, mode %u, end reason %u%s\n", number, state, direction, type, mode, reason, u != nil ? "" : " (not a call iOS knows yet)");
	/* (the sound: from the other end's ringing on for a call from here, from the answer for one to here; whoever made the call) */
	if (state == 3 || (state == 5 && direction == 1)) {
		call_sound(number, true);
	} else if (state == 9) {
		call_sound(number, false);
	}
	if (call_provider == nil) {
		return;
	}
	if (u == nil && direction == 1 && call_dialling != nil && state != 9) {
		u = call_dialling;
		call_dialling = nil;
		call_bind(number, u);
	}
	if (u == nil && direction == 1 && call_abandoned && state != 9) {
		NSString *request = [NSString stringWithFormat:@"end %u", number];

		call_abandoned = false;
		say("call: %u is the call iOS gave up before it had a number: ended\n", number);
		dispatch_async(call_queue, ^{
			(void)call_request(request);
		});
		return;
	}
	if (u == nil && direction == 2 && (state == 2 || state == 7 || state == 10)) {
		call_incoming(number, party);
		return;
	}
	if (u == nil) {
		return;         /* (a call made behind iOS's back: "hk_commcenter call dial") */
	}
	call_state[u] = @(state);
	switch (state) {
	case 1: case 4: case 5:
		if (![call_starting containsObject:u]) {
			[call_provider reportOutgoingCallWithUUID:u startedConnectingAtDate:nil];
		}
		break;
	case 3:
		if ([call_starting containsObject:u]) {
			break;          /* (said when the start action is answered: its block looks at call_state) */
		}
		if (![call_was_connected containsObject:u]) {
			[call_was_connected addObject:u];
			if (direction == 1) {
				[call_provider reportOutgoingCallWithUUID:u connectedAtDate:nil];
			}
			say("call: %u is connected\n", number);
		}
		break;
	case 9:
		if ([call_starting containsObject:u]) {
			break;          /* (over before the dial was answered: the start action's block ends it for iOS) */
		}
		if (![call_ending containsObject:u]) {
			CXCallEndedReason why = [call_was_connected containsObject:u] ? CXCallEndedReasonRemoteEnded : direction == 2 ? CXCallEndedReasonUnanswered : CXCallEndedReasonFailed;

			say("call: %u ended from the other side or by the network (CallKit's reason %ld)\n", number, (long)why);
			[call_provider reportCallWithUUID:u endedAtDate:nil reason:why];
		}
		call_forget(u);
		break;
	default:
		break;
	}
}

static void
call_poll(void)
{
	static char text[32 + 16 * 97 + 8];
	size_t n = sizeof(text) - 1;
	NSString *fake;

	if (sysctlbyname("kern.hackios.modem_call_events", text, &n, NULL, 0) == 0) {
		text[n < sizeof(text) ? n : sizeof(text) - 1] = 0;
		if (call_taken < 0) {
			call_taken = strtol(text, NULL, 10);     /* (what happened before this daemon started is not replayed) */
		}
		for (char *line = strchr(text, '\n'), *next; line != NULL && *++line != 0; line = next) {
			unsigned seq, number, state, direction, type, mode, reason;
			char party[40] = "";

			next = strchr(line, '\n');
			if (next != NULL) {
				*next = 0;
			}
			if (sscanf(line, "%u %u %u %u %u %u %u %39s", &seq, &number, &state, &direction, &type, &mode, &reason, party) >= 7 && (long)seq >= call_taken) {
				call_taken = (long)seq + 1;
				call_event(number, state, direction, type, mode, reason, @(party));
			}
		}
	}
	if (call_provider != nil && (fake = [NSString stringWithContentsOfFile:@CALL_FAKE_NOTE encoding:NSUTF8StringEncoding error:NULL]) != nil) {
		unlink(CALL_FAKE_NOTE);
		if (call_uuid[@CALL_FAKE] == nil) {
			say("call: a call that is not on the air, asked for by hand\n");
			call_incoming(CALL_FAKE, [fake stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]);
		}
	}
}

@interface CallDelegate : NSObject <CXProviderDelegate>
@end

@implementation CallDelegate

- (void)providerDidReset:(CXProvider *)provider {
	say("call: CallKit resets its provider: every call it knew is ended\n");
	for (NSNumber *n in call_uuid.allKeys) {
		if (n.unsignedIntValue != CALL_FAKE) {
			NSString *request = [NSString stringWithFormat:@"end %u", n.unsignedIntValue];

			dispatch_async(call_queue, ^{
				(void)call_request(request);
			});
		}
	}
	[call_uuid removeAllObjects];
	[call_number removeAllObjects];
	[call_state removeAllObjects];
	[call_ending removeAllObjects];
	[call_was_connected removeAllObjects];
	[call_starting removeAllObjects];
	[call_given_up removeAllObjects];
	call_dialling = nil;
}

- (void)providerDidBegin:(CXProvider *)provider {
	say("call: CallKit's provider has begun\n");
}

- (void)provider:(CXProvider *)provider performStartCallAction:(CXStartCallAction *)action {
	NSMutableString *digits = [NSMutableString string];
	NSString *value = action.handle.value ?: @"";
	NSUUID *u = action.callUUID;

	for (NSUInteger i = 0; i < value.length; i++) {
		unichar c = [value characterAtIndex:i];

		if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#') {
			[digits appendFormat:@"%C", c];
		}
	}
	say("call: iOS starts a call to a number of %lu characters (handle type %ld)\n", (unsigned long)digits.length, (long)action.handle.type);
	if (digits.length == 0 || call_dialling != nil) {
		[action fail];
		return;
	}
	call_dialling = u;
	call_abandoned = false;
	call_state[u] = @1;
	[call_starting addObject:u];
	dispatch_async(call_queue, ^{
		NSString *status = call_request([@"dial " stringByAppendingString:digits]);

		dispatch_async(dispatch_get_main_queue(), ^{
			unsigned number = 0;
			bool given_up = [call_given_up containsObject:u];

			say("call: the modem on the dial: %s%s\n", status.UTF8String, given_up ? " (iOS has given the call up meanwhile)" : "");
			[call_starting removeObject:u];
			[call_given_up removeObject:u];
			if (sscanf(status.UTF8String, "dial: accepted, call %u", &number) == 1) {
				if (given_up) {
					if (number != 0 && call_abandoned) {
						call_abandoned = false;
						dispatch_async(call_queue, ^{
							(void)call_request([NSString stringWithFormat:@"end %u", number]);
						});
					}
					return;
				}
				if (call_dialling == u) {
					call_dialling = nil;
					if (number != 0 && call_uuid[@(number)] == nil) {
						call_bind(number, u);
					}
				}
				[action fulfill];
				[call_provider reportOutgoingCallWithUUID:u startedConnectingAtDate:nil];
				if (call_state[u].unsignedIntValue == 3 && ![call_was_connected containsObject:u]) {
					[call_was_connected addObject:u];
					[call_provider reportOutgoingCallWithUUID:u connectedAtDate:nil];
				} else if (call_state[u].unsignedIntValue == 9) {
					say("call: it was over before the dial was answered\n");
					[call_provider reportCallWithUUID:u endedAtDate:nil reason:CXCallEndedReasonFailed];
					call_forget(u);
				}
			} else if (!given_up) {
				if (call_dialling == u) {
					call_dialling = nil;
				}
				call_forget(u);
				[action fail];
			}
		});
	});
}

- (void)provider:(CXProvider *)provider performAnswerCallAction:(CXAnswerCallAction *)action {
	NSUUID *u = action.callUUID;
	NSNumber *n = call_number[u];

	say("call: iOS answers call %d\n", n != nil ? n.intValue : -1);
	if (n == nil) {
		[action fail];
		return;
	}
	if (n.unsignedIntValue == CALL_FAKE) {
		[call_was_connected addObject:u];
		[action fulfill];
		return;
	}
	dispatch_async(call_queue, ^{
		NSString *status = call_request([NSString stringWithFormat:@"answer %u", n.unsignedIntValue]);

		dispatch_async(dispatch_get_main_queue(), ^{
			say("call: the modem on the answer: %s\n", status.UTF8String);
			if ([status hasPrefix:@"answer: accepted"]) {
				[action fulfill];
			} else {
				[action fail];
			}
		});
	});
}

- (void)provider:(CXProvider *)provider performEndCallAction:(CXEndCallAction *)action {
	NSUUID *u = action.callUUID;
	NSNumber *n = call_number[u];

	say("call: iOS ends call %d\n", n != nil ? n.intValue : -1);
	if (n == nil && call_dialling == u) {
		/* (ended before the modem numbered it: nothing to end by id yet. The call is ended when its number is known: by the dial's answer or its first event) */
		call_dialling = nil;
		call_abandoned = true;
		[call_given_up addObject:u];
		call_forget(u);
		[action fulfill];
		return;
	}
	if (n == nil || n.unsignedIntValue == CALL_FAKE) {
		call_forget(u);
		[action fulfill];
		return;
	}
	[call_ending addObject:u];
	dispatch_async(call_queue, ^{
		NSString *status = call_request([NSString stringWithFormat:@"end %u", n.unsignedIntValue]);

		dispatch_async(dispatch_get_main_queue(), ^{
			say("call: the modem on the end: %s\n", status.UTF8String);
			[action fulfill];        /* (whatever the modem said: for iOS the call is over, and a call the modem still has ends with its own event) */
		});
	});
}

- (void)provider:(CXProvider *)provider performPlayDTMFCallAction:(CXPlayDTMFCallAction *)action {
	NSNumber *n = call_number[action.callUUID];
	NSString *digits = action.digits ?: @"";

	say("call: iOS plays %lu tone(s) into call %d\n", (unsigned long)digits.length, n != nil ? n.intValue : -1);
	if (n != nil && n.unsignedIntValue != CALL_FAKE && digits.length > 0 && digits.length <= 32) {
		NSString *request = [NSString stringWithFormat:@"dtmf %u %@", n.unsignedIntValue, digits];

		dispatch_async(call_queue, ^{
			NSString *status = call_request(request);

			say("call: the modem on the tones: %s\n", status.UTF8String);
		});
	}
	[action fulfill];
}

- (void)provider:(CXProvider *)provider performSetHeldCallAction:(CXSetHeldCallAction *)action {
	say("call: iOS wants a call %s: not done here yet\n", action.onHold ? "held" : "taken back");
	[action fail];
}

- (void)provider:(CXProvider *)provider performSetMutedCallAction:(CXSetMutedCallAction *)action {
	int value = action.muted ? 1 : 0, e = sysctlbyname("kern.hackios.voice_mute", NULL, NULL, &value, sizeof(value));

	say("call: iOS wants the microphone %s: %s\n", action.muted ? "muted" : "open", e == 0 ? "the kernel took it" : strerror(errno));
	[action fulfill];
}

- (void)provider:(CXProvider *)provider timedOutPerformingAction:(CXAction *)action {
	say("call: CallKit waited too long for %s\n", NSStringFromClass(action.class).UTF8String);
}

- (void)provider:(CXProvider *)provider didActivateAudioSession:(id)session {
	say("call: iOS has the call's audio session active\n");
}

- (void)provider:(CXProvider *)provider didDeactivateAudioSession:(id)session {
	say("call: iOS has the call's audio session no longer active\n");
}

@end

static void
call_start(void)
{
	static CallDelegate *delegate;
	static dispatch_source_t timer;
	CXProviderConfiguration *c = [[CXProviderConfiguration alloc] initWithLocalizedName:@"Telephony"];

	call_uuid = [NSMutableDictionary dictionary];
	call_number = [NSMutableDictionary dictionary];
	call_state = [NSMutableDictionary dictionary];
	call_ending = [NSMutableSet set];
	call_was_connected = [NSMutableSet set];
	call_starting = [NSMutableSet set];
	call_given_up = [NSMutableSet set];
	call_queue = dispatch_queue_create("com.hackios.telephonyd.calls", DISPATCH_QUEUE_SERIAL);
	c.supportsVideo = NO;
	c.maximumCallGroups = 1;
	c.maximumCallsPerCallGroup = 1;
	c.supportedHandleTypes = [NSSet setWithObject:@(CXHandleTypePhoneNumber)];
	c.includesCallsInRecents = YES;
	delegate = [CallDelegate new];
	call_provider = [[CXProvider alloc] initWithConfiguration:c];
	[call_provider setDelegate:delegate queue:dispatch_get_main_queue()];
	say("call: a CallKit provider %s (this program's identifier: %s)\n", call_provider != nil ? "made" : "NOT made", NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "none");
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC / 2, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{
		call_poll();
	});
	dispatch_resume(timer);
}

/* hk_commcenter call ...: by hand, beside the daemon */
static int
call_command(int argc, char **argv)
{
	static char text[32 + 16 * 97 + 8];
	size_t n = sizeof(text) - 1;

	if (argc >= 4 && (strcmp(argv[2], "speaker") == 0 || strcmp(argv[2], "mute") == 0 || strcmp(argv[2], "sound") == 0)) {
		/* hk_commcenter call speaker|mute|sound 0|1: the kernel's switches of a call's sound, by hand (kern.hackios.voice_speaker, voice_mute, voice_call) */
		const char *name = argv[2][0] == 's' && argv[2][1] == 'p' ? "kern.hackios.voice_speaker" : argv[2][0] == 'm' ? "kern.hackios.voice_mute" : "kern.hackios.voice_call";
		int value = atoi(argv[3]), now = -1, e = sysctlbyname(name, NULL, NULL, &value, sizeof(value));
		size_t n = sizeof(now);

		(void)sysctlbyname(name, &now, &n, NULL, 0);
		printf("hk_commcenter: %s = %d: %s; it reads %d\n", name, value, e == 0 ? "taken" : strerror(errno), now);
		return e == 0 ? 0 : 1;
	}
	if (argc >= 4 && strcmp(argv[2], "fake") == 0) {
		bool ok = [@(argv[3]) writeToFile:@CALL_FAKE_NOTE atomically:YES encoding:NSUTF8StringEncoding error:NULL];

		printf("hk_commcenter: a call that is not on the air, from a number of %zu characters: the note for the daemon is %s\n", strlen(argv[3]), ok ? "left" : "NOT left");
		return ok ? 0 : 1;
	}
	if (argc >= 4 && (strcmp(argv[2], "dial") == 0 || strcmp(argv[2], "answer") == 0 || strcmp(argv[2], "end") == 0 || strcmp(argv[2], "dtmf") == 0)) {
		NSMutableString *request = [NSMutableString stringWithUTF8String:argv[2]];

		for (int i = 3; i < argc; i++) {
			[request appendFormat:@" %@", [NSString stringWithUTF8String:argv[i]] ?: @"?"];
		}
		printf("hk_commcenter: call %s: %s\n", argv[2], call_request(request).UTF8String);
	} else if (argc < 3 || strcmp(argv[2], "events") != 0) {
		printf("usage: hk_commcenter call dial NUMBER | answer ID | end ID | dtmf ID DIGITS | events | fake NUMBER | speaker 0|1 | mute 0|1 | sound 0|1\n");
		return 2;
	}
	/* the events, the other party's number as its length */
	if (sysctlbyname("kern.hackios.modem_call_events", text, &n, NULL, 0) != 0) {
		printf("hk_commcenter: the kernel has no call events (%s)\n", strerror(errno));
		return 1;
	}
	text[n < sizeof(text) ? n : sizeof(text) - 1] = 0;
	printf("hk_commcenter: %ld call event(s) so far\n", strtol(text, NULL, 10));
	for (char *line = strchr(text, '\n'), *next; line != NULL && *++line != 0; line = next) {
		unsigned seq, number, state, direction, type, mode, reason;
		char party[40] = "";

		next = strchr(line, '\n');
		if (next != NULL) {
			*next = 0;
		}
		if (sscanf(line, "%u %u %u %u %u %u %u %39s", &seq, &number, &state, &direction, &type, &mode, &reason, party) >= 7) {
			printf("hk_commcenter:   event %u: call %u state %u direction %u type %u mode %u end reason %u, a number of %zu characters\n", seq, number, state, direction, type, mode, reason,
			    strcmp(party, "-") == 0 ? (size_t)0 : strlen(party));
		}
	}
	return 0;
}

/*
 * A received message makes no banner and no lock-screen entry here until this is done (2026-11-11, read from IMDPersistenceAgent's own log with oslog: the
 * notification's request is made, then "We don't post notifications during system migration"). -[IMDNotificationsController _haveMigrated] is
 * IMDSystemMigrationOccurred(): the preference kIMDPersistenceLastBuild of com.apple.imdsmsrecordstore (user mobile) has to be this system's build. On an
 * iPhone the Messages data migrator writes it after an update, with IMDSystemMigrationSetHasMigrated(true); this system has never run a migration (its
 * Messages database is made by this build: there is nothing to migrate), so the same function is called here, once it says "not yet". The same state
 * also lets Spotlight index the messages ("Needs migration, not allowing indexing").
 */
static const char *
imd_migrated(void)
{
	void *lib = dlopen("/System/Library/PrivateFrameworks/IMDPersistence.framework/IMDPersistence", RTLD_NOW);
	bool (*occurred)(void) = lib != NULL ? dlsym(lib, "IMDSystemMigrationOccurred") : NULL;
	void (*set)(bool) = lib != NULL ? dlsym(lib, "IMDSystemMigrationSetHasMigrated") : NULL;

	if (occurred == NULL || set == NULL) {
		return "IMDPersistence has no such functions: nothing done";
	}
	if (occurred()) {
		return "already recorded";
	}
	set(true);
	return occurred() ? "recorded now" : "written, and it still says not migrated";
}

/*
 * MobileGestalt's kept answers on an installed phone. Whether the phone has a baseband (HasBaseband, cellular-data, telephony) MobileGestalt reads from
 * the device tree once and keeps in com.apple.MobileGestalt.plist in its system group container, for as long as the build is the same. A data volume that
 * was first started by a kernel without the cellular path keeps "no baseband", and iOS then shows nothing of the cellular side however well the modem
 * works (the OnePlus 8 Pro port's notes: "it was removed once and iOS made a new one").
 *
 * Putting the file aside from here is too late: MobileGestalt's helper is running by then and writes the same answers out again from memory (tried
 * 2026-11-11: the preview's boot put it aside, and the next boot had "no baseband" again). init does it at the right time for the camera
 * (userland/src/init.c, camera_answers(): when what this boot says about the camera is not what the stamp /private/var/.hackios-camera says, the kept
 * file is renamed before iOS starts, and the stamp written anew). So: when this kernel has the cellular path and MobileGestalt still says no baseband,
 * the stamp is overwritten with a line init cannot agree with, and the NEXT start renames the kept file before iOS reads it. One restart; on a memory
 * filesystem the answer is right and nothing is done.
 */
#define GESTALT_STAMP "/private/var/.hackios-camera"

static void
gestalt_check(void)
{
	void *lib = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	CFTypeRef (*copy)(CFStringRef) = lib != NULL ? dlsym(lib, "MGCopyAnswer") : NULL;
	int registered = 0;
	size_t n = sizeof(registered);
	id has;

	if (copy == NULL || sysctlbyname("kern.hackios.cellular_registered", &registered, &n, NULL, 0) != 0) {
		return;         /* (no MobileGestalt, or a kernel without the cellular path: nothing to say) */
	}
	has = CFBridgingRelease(copy(CFSTR("HasBaseband")));
	if ([has respondsToSelector:@selector(boolValue)] && [has boolValue]) {
		say("MobileGestalt says the phone has a baseband\n");
		return;
	}
	if (access(GESTALT_STAMP, F_OK) != 0) {
		say("MobileGestalt says NO baseband, and there is no stamp of init's on this volume: nothing done\n");
		return;
	}
	say("MobileGestalt's kept answers say NO baseband (they are from before this kernel's cellular path): init's stamp is marked, so that the next start makes MobileGestalt ask the device tree again: %s. iOS shows the cellular side after ONE RESTART\n",
	    [@"asked again by hk_commcenter: the kept answers had no baseband\n" writeToFile:@GESTALT_STAMP atomically:YES encoding:NSUTF8StringEncoding error:NULL] ? "done" : "NOT written");
}

static void
hop(char **argv)
{
	NSString *dir = @"/private/var/root/iphont", *best = nil;
	long newest = -1;

	for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:NULL] ?: @[]) {
		if ([name hasPrefix:@"hk_commcenter."] && [name substringFromIndex:14].integerValue > newest) {
			newest = [name substringFromIndex:14].integerValue;
			best = [dir stringByAppendingPathComponent:name];
		}
	}
	if (best != nil) {
		char *const next[] = { (char *)best.fileSystemRepresentation, "here", NULL };

		chmod(next[0], 0755);
		say("a newer build is on the phone: %s\n", next[0]);
		execv(next[0], next);
		say("it does not start (errno %d): going on with the one in the root\n", errno);
	}
	(void)argv;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc >= 3 && strcmp(argv[1], "diag") == 0) {
			/* hk_commcenter diag HEX: a DIAG request to the modem as the kernel frames it (kern.hackios.modem_diag); its terse messages are printed from then on */
			int e = sysctlbyname("kern.hackios.modem_diag", NULL, NULL, argv[2], strlen(argv[2]));

			printf("hk_commcenter: diag %s: %s\n", argv[2], e == 0 ? "queued" : strerror(errno));
			return 0;
		}
		if (argc >= 4 && strcmp(argv[1], "sms") == 0) {
			/* hk_commcenter sms DIGITS TEXT...: one SMS through the kernel (kern.hackios.modem_sms_send: an international number without the plus, a space, the text). The number is not printed. */
			NSMutableString *t = [NSMutableString stringWithUTF8String:argv[2]];

			for (int i = 3; i < argc; i++) {
				/* (not %s: NSString's %s reads the bytes in the system encoding, and Chinese went out as accented Latin letters, 2026-11-11) */
				[t appendFormat:@" %@", [NSString stringWithUTF8String:argv[i]] ?: @"?"];
			}
			const char *c = t.UTF8String;
			int e = sysctlbyname("kern.hackios.modem_sms_send", NULL, NULL, (void *)c, strlen(c));

			printf("hk_commcenter: sms to a number of %lu digits, %lu bytes of text: %s\n", strlen(argv[2]), strlen(c) - strlen(argv[2]) - 1, e == 0 ? "queued" : strerror(errno));
			return 0;
		}
		if (argc >= 2 && strcmp(argv[1], "sms-status") == 0) {
			/* hk_commcenter sms-status [TEXT...]: the last send's state and the last message received (kern.hackios.modem_sms_status, modem_sms_last: "COUNT NUMBER: TEXT"),
			   as lengths; with TEXT, whether the last message's text is that one. Neither number nor text is printed. */
			char status[256] = "", last[400] = "";
			size_t n = sizeof(status) - 1, m = sizeof(last) - 1;
			NSMutableString *want = [NSMutableString string];

			sysctlbyname("kern.hackios.modem_sms_status", status, &n, NULL, 0);
			sysctlbyname("kern.hackios.modem_sms_last", last, &m, NULL, 0);
			for (int i = 2; i < argc; i++) {
				[want appendFormat:@"%s%s", i > 2 ? " " : "", argv[i]];
			}
			char *end = NULL;
			unsigned long count = strtoul(last, &end, 10);
			const char *from = end != NULL && *end == ' ' ? end + 1 : "", *colon = strstr(from, ": "), *text = colon != NULL ? colon + 2 : "";

			printf("hk_commcenter: sms: last send \"%.40s\"; %lu received; the last from a number of %ld characters with %lu bytes of text%s\n", status, count,
			    colon != NULL ? (long)(colon - from) : 0L, strlen(text), want.length == 0 ? "" : strcmp(text, want.UTF8String) == 0 ? ": the text expected" : ": NOT the text expected");
			return 0;
		}
		if (argc >= 3 && strcmp(argv[1], "dcm") == 0) {
			/* hk_commcenter dcm HEX|again: a QMI message by hand from the kernel's IMS data service (0x302) to the modem's IMS, or its activation indication once more (kern.hackios.modem_dcm) */
			int e = sysctlbyname("kern.hackios.modem_dcm", NULL, NULL, argv[2], strlen(argv[2]));

			printf("hk_commcenter: dcm (%lu letters): %s\n", strlen(argv[2]), e == 0 ? "queued" : strerror(errno));
			return 0;
		}
		if (argc >= 2 && strcmp(argv[1], "imd-migrated") == 0) {
			/* hk_commcenter imd-migrated: Messages' "the system's migration is done" recorded, by hand (the daemon does it at its start) */
			printf("hk_commcenter: Messages' migration state: %s\n", imd_migrated());
			return 0;
		}
		if (argc >= 2 && strcmp(argv[1], "call") == 0) {
			/* hk_commcenter call dial NUMBER | answer ID | end ID | dtmf ID DIGITS | events | fake NUMBER */
			return call_command(argc, argv);
		}
		if (argc >= 2 && strcmp(argv[1], "restart-modem") == 0) {
			/* hk_commcenter restart-modem: the kernel starts the modem again from scratch (kern.hackios.modem_restart) */
			int one = 1;

			printf("hk_commcenter: the modem's restart asked of the kernel: %s\n", sysctlbyname("kern.hackios.modem_restart", NULL, NULL, &one, sizeof(one)) == 0 ? "done" : strerror(errno));
			return 0;
		}
		if (argc >= 4 && strcmp(argv[1], "qmi") == 0) {
			/* hk_commcenter qmi SERVICE MSGID [TLVHEX [PORT]]: one request by hand (PORT 4 or 6: which of the kernel's two clients) (service decimal, the rest hex), its answer's TLVs */
			uint8_t tlv[200];
			size_t n = 0;
			int result = -1;
			NSDictionary<NSNumber *, NSData *> *a;

			for (const char *h = argc >= 5 ? argv[4] : ""; h[0] != 0 && h[1] != 0 && n < sizeof(tlv); h += 2) {
				unsigned byte = 0;

				sscanf(h, "%2x", &byte);
				tlv[n++] = (uint8_t)byte;
			}
			a = qmi_from(argc >= 6 ? (unsigned)strtoul(argv[5], NULL, 10) : 4, 60, (unsigned)strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 16), tlv, n, &result);
			printf("hk_commcenter: qmi %s %s: %s, result %d\n", argv[2], argv[3], a == nil ? "no answer" : "answered", result);
			for (NSNumber *type in [a.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
				printf("hk_commcenter:   TLV %#04x, %lu bytes: %s\n", type.unsignedIntValue, (unsigned long)a[type].length, a[type].description.UTF8String);
			}
			return 0;
		}
		if (argc < 2) {
			hop(argv);
		}
		static Listener *delegate;
		static NSXPCListener *listener;
		static dispatch_source_t timer;

		setvbuf(stdout, NULL, _IOLBF, 0);
		if (dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony", RTLD_NOW) == NULL) {
			say("CoreTelephony does not load: %s\n", dlerror());
			return 1;
		}
		service_protocol = objc_getProtocol("CTXPCServiceInterface");
		delegate_protocol = objc_getProtocol("CoreTelephonyClientDelegateProxyInterface");
		if (service_protocol == nil || delegate_protocol == nil) {
			say("CoreTelephony has no CTXPCServiceInterface or no CoreTelephonyClientDelegateProxyInterface\n");
			return 1;
		}
		seen = [NSMutableDictionary dictionary];
		described = [NSMutableSet set];
		responders = [NSMutableArray array];
		read_state();
		say("started: registered %d, bars %d, data %d, an operator name of %zu letters; a context %s\n", now.registered, now.bars, now.data, strlen(now.oper),
		    context() != nil ? "made" : "NOT made");
		say("the context: %s; the contexts: %s\n", [[context() description] UTF8String], [[contexts() description] UTF8String]);
		delegate = [Listener new];
		listener = ((id (*)(id, SEL, id))objc_msgSend)([NSXPCListener alloc], sel_registerName("initWithMachServiceName:"), @"com.apple.commcenter.coretelephony.xpc");
		listener.delegate = delegate;
		[listener resume];
		legacy_listen("com.apple.commcenter.xpc");
		call_start();
		gestalt_check();
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			say("Messages' migration state (a message's notification needs it): %s\n", imd_migrated());
		});
		/* every 2 s: the kernel's numbers; when they change, everybody registered is told. Every 30 s: who asked what, how often */
		timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
		dispatch_source_set_event_handler(timer, ^{
			static unsigned ticks;

			static int was_registered = -1;

			static int was_data = -1;

			if (now.registered != was_registered || now.data != was_data || ticks % 30 == 29) {
				bool first = now.registered != was_registered || now.data != was_data;

				was_registered = now.registered;
				was_data = now.data;
				dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
					read_facts(first);
					if (now.data) {
						ims_watch();            /* (after the internet session: the kernel's thread is busy until that is up) */
					}
				});
			}
			sms_poll();
			if (read_state()) {
				NSArray *all;

				say("now: registered %d, bars %d, data %d\n", now.registered, now.bars, now.data);
				@synchronized (responders) {
					all = [responders copy];
				}
				for (Responder *r in all) {
					[r tell];
				}
			}
			if (++ticks % 15 == 0) {
				NSDictionary *counts;
				NSMutableString *line = [NSMutableString string];

				@synchronized (seen) {
					counts = [seen copy];
				}
				for (NSString *key in [counts keysSortedByValueUsingComparator:^(id a, id b) { return [b compare:a]; }]) {
					if ([counts[key] unsignedIntValue] < 3 || line.length > 900) {
						break;
					}
					[line appendFormat:@" [%@ x%@]", key, counts[key]];
				}
				if (line.length > 0) {
					say("often:%s\n", line.UTF8String);
				}
			}
		});
		dispatch_resume(timer);
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
