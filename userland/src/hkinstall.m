/*
 * hkinstall: an .app onto the phone with no installd (T38, item 13; the owner: 「可不可以幫我裝ipa，比如telegram」). installd checks a signature and a profile itself, in user space, where the
 * kernel's AMFI stand-in does not reach. LaunchServices takes a description of an app from a program entitled to give one, which is what TrollStore's RootHelper/uicache.m does, and the
 * description below follows it: its keys are what FrontBoard and LaunchServices read, nothing here changes a byte of the app, and no Apple server sees any of it.
 *
 * TrollStore's also tells LaunchServices that the app has Apple's own application signature, and is ad-hoc signed. Neither is what a build signed "Apple Distribution: its maker" carries, so they
 * are left out unless "claim" comes first: to be used only if a launch is seen refused for want of them, and said so (the supervisor, 2026-11-06).
 *
 * usage: hkinstall STEP...
 *   alarm SECONDS             the program's end, from now on, instead of 300 s
 *   claim                     what follows registers with TrollStore's signer fields (above)
 *   wait-net SECONDS          until en0 has an address, for at most SECONDS (the address is not printed)
 *   fetch HOST PORT PATH SHA256 FILE   HTTP/1.0 GET into FILE, and its hash checked: tools/ipa/serve.py is the other end. Says how far every 32 MB; a stall starts it again, up to three tries
 *   unpack FILE DIR           a ustar archive under DIR (tools/ipa/pack.sh made it): Payload/NAME.app and ents/BUNDLE.plist, entitlements as the host read them from the signatures
 *   install DIR               the .app in DIR/Payload into a bundle container, then the containers it needs, then registered
 *   state BUNDLE              what LaunchServices says of it
 *   open BUNDLE               ask for it to be launched
 *   remove BUNDLE             unregister it and delete its bundle
 *
 * Says only steps, counts, sizes, paths under /var/containers, and results. It reads no file in the app's data container, so nothing of an account can reach a log from here.
 */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define INSTALLD_UID 33         /* _installd, who owns /var/containers/Bundle on a real phone */

static bool claim_signer;

@interface NSObject (HKPrivate)
+ (id)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary;
- (BOOL)unregisterApplication:(NSURL *)url;
- (BOOL)openApplicationWithBundleID:(NSString *)bundle;
+ (id)applicationProxyForIdentifier:(NSString *)bundle;
- (NSURL *)bundleURL;
- (NSURL *)dataContainerURL;
- (NSString *)applicationType;
- (NSArray *)plugInKitPlugins;
- (BOOL)isInstalled;
+ (id)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
+ (id)serviceWithDefaultShellEndpoint;
- (void)openApplication:(NSString *)bundle withOptions:(id)options completion:(void (^)(id process, NSError *error))completion;
- (NSString *)signerIdentity;
- (BOOL)profileValidated;
- (BOOL)isRestricted;
@end

static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	va_list list;

	printf("hkinstall: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
	fflush(stdout);
}

static bool
network_is_up(void)
{
	struct ifaddrs *all, *one;
	bool up = false;

	if (getifaddrs(&all) != 0) {
		return false;
	}
	for (one = all; one != NULL; one = one->ifa_next) {
		if (strcmp(one->ifa_name, "en0") == 0 && one->ifa_addr != NULL && one->ifa_addr->sa_family == AF_INET &&
		    (ntohl(((struct sockaddr_in *)one->ifa_addr)->sin_addr.s_addr) >> 16) != 0xa9fe) {
			up = true;
		}
	}
	freeifaddrs(all);
	return up;
}

static int
wait_net(double most)
{
	double spent = 0;

	for (; spent < most && !network_is_up(); spent += 1) {
		sleep(1);
	}
	say("the network is %s after %.0f s", network_is_up() ? "up" : "NOT up", spent);
	return network_is_up() ? 0 : 1;
}

/* ---- fetch ---- */

#define FETCH_STALL_S 30        /* nothing read for this long is a stall: run 125618 stood still until the run's end with the network joined, and nothing said how far it had got */
#define FETCH_TRIES 3
#define FETCH_STALLED 2

static int
fetch_once(const char *host, const char *port, const char *path, const char *expected, const char *file)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found = NULL;
	CC_SHA256_CTX hash;
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	char request[512], text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
	static char buffer[1 << 20];
	unsigned long long length = 0, got = 0;
	size_t have = 0;
	char *body = NULL;
	NSDate *began = [NSDate date];
	int sock, out;

	if (getaddrinfo(host, port, &hints, &found) != 0) {
		say("fetch: no such host");
		return 1;
	}
	sock = socket(found->ai_family, found->ai_socktype, 0);
	if (sock < 0 || connect(sock, found->ai_addr, found->ai_addrlen) != 0) {
		say("fetch: cannot connect");
		freeaddrinfo(found);
		return 1;
	}
	freeaddrinfo(found);
	{
		struct timeval stall = { .tv_sec = FETCH_STALL_S };

		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &stall, sizeof(stall));
	}
	snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
	if (write(sock, request, strlen(request)) < 0) {
		say("fetch: cannot send");
		close(sock);
		return 1;
	}
	while (body == NULL && have < sizeof(buffer) - 1) {
		ssize_t n = read(sock, buffer + have, sizeof(buffer) - 1 - have);

		if (n <= 0) {
			say("fetch: the connection ended in the headers");
			close(sock);
			return n < 0 && errno == EAGAIN ? FETCH_STALLED : 1;
		}
		have += (size_t)n;
		buffer[have] = 0;
		body = strstr(buffer, "\r\n\r\n");
	}
	if (body == NULL || strncmp(buffer, "HTTP/1.", 7) != 0 || strncmp(buffer + 9, "200", 3) != 0) {
		say("fetch: the server does not say 200");
		return 1;
	}
	{
		const char *field = strcasestr(buffer, "content-length:");

		if (field != NULL) {
			length = strtoull(field + 15, NULL, 10);
		}
	}
	body += 4;
	out = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		say("fetch: cannot create the file");
		return 1;
	}
	CC_SHA256_Init(&hash);
	{
		size_t first = have - (size_t)(body - buffer);

		if (first != 0) {
			CC_SHA256_Update(&hash, body, (CC_LONG)first);
			if (write(out, body, first) != (ssize_t)first) {
				say("fetch: cannot write");
				return 1;
			}
			got = first;
		}
	}
	for (;;) {
		ssize_t n = read(sock, buffer, sizeof(buffer));
		time_t before;

		if (n < 0) {
			int why = errno;

			say(why == EAGAIN ? "fetch: nothing for %d s after %llu bytes" : "fetch: read failed (%d) after %llu bytes", why == EAGAIN ? FETCH_STALL_S : why, got);
			close(sock);
			close(out);
			return FETCH_STALLED;
		}
		if (n == 0) {
			break;
		}
		CC_SHA256_Update(&hash, buffer, (CC_LONG)n);
		before = time(NULL);
		if (write(out, buffer, (size_t)n) != n) {
			say("fetch: cannot write after %llu bytes (is /var big enough?)", got);
			close(sock);
			close(out);
			return 1;
		}
		if (time(NULL) - before >= 5) {
			say("fetch: one write took %ld s, after %llu bytes", (long)(time(NULL) - before), got);
		}
		if ((got + (unsigned long long)n) >> 25 != got >> 25) {
			say("fetch: %llu MB in %.0f s", (got + (unsigned long long)n) >> 20, -began.timeIntervalSinceNow);
		}
		got += (unsigned long long)n;
	}
	close(sock);
	close(out);
	CC_SHA256_Final(digest, &hash);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
		snprintf(text + 2 * i, 3, "%02x", digest[i]);
	}
	say("fetch: %llu bytes of %llu in %.0f s", got, length, -began.timeIntervalSinceNow);
	if ((length != 0 && got != length) || strcasecmp(text, expected) != 0) {
		say("fetch: THE HASH OR THE SIZE IS WRONG: not used");
		unlink(file);
		return 1;
	}
	say("fetch: the hash is right");
	return 0;
}

static int
fetch(const char *host, const char *port, const char *path, const char *expected, const char *file)
{
	int result = FETCH_STALLED;

	for (int try = 1; try <= FETCH_TRIES && result == FETCH_STALLED; try++) {
		if (try > 1) {
			say("fetch: from the start again, try %d of %d", try, FETCH_TRIES);
		}
		result = fetch_once(host, port, path, expected, file);
	}
	return result == 0 ? 0 : 1;
}

/* ---- unpack: ustar, files, directories and links; nothing outside DIR ---- */

static unsigned long long
octal(const char *field, size_t length)
{
	unsigned long long value = 0;

	for (size_t i = 0; i < length && field[i] != 0; i++) {
		if (field[i] >= '0' && field[i] <= '7') {
			value = value * 8 + (unsigned long long)(field[i] - '0');
		}
	}
	return value;
}

static bool
safe_name(const char *name)
{
	return name[0] != '/' && strstr(name, "../") == NULL && strcmp(name, "..") != 0;
}

static void
make_parents(char *path)
{
	for (char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		*slash = 0;
		mkdir(path, 0755);
		*slash = '/';
	}
}

static int
unpack(const char *file, const char *dir)
{
	int in = open(file, O_RDONLY);
	char block[512];
	static char buffer[1 << 20];
	unsigned files = 0, dirs = 0, links = 0;
	unsigned long long bytes = 0;

	if (in < 0) {
		say("unpack: cannot open the archive");
		return 1;
	}
	mkdir(dir, 0755);
	while (read(in, block, sizeof(block)) == sizeof(block) && block[0] != 0) {
		char name[256], path[1024];
		unsigned long long size = octal(block + 124, 12);
		unsigned mode = (unsigned)octal(block + 100, 8) & 0777;
		char type = block[156];

		if (block[345] != 0) {
			snprintf(name, sizeof(name), "%.155s/%.100s", block + 345, block);
		} else {
			snprintf(name, sizeof(name), "%.100s", block);
		}
		if (!safe_name(name)) {
			say("unpack: refused a name outside its directory");
			return 1;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		make_parents(path);
		if (type == '5') {
			mkdir(path, mode | 0700);
			dirs++;
		} else if (type == '2') {
			char target[101] = { 0 };

			memcpy(target, block + 157, 100);
			if (!safe_name(target)) {
				say("unpack: refused a link outside its directory");
				return 1;
			}
			(void)symlink(target, path);
			links++;
		} else if (type == '0' || type == 0) {
			int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
			unsigned long long left = size;

			if (out < 0) {
				say("unpack: cannot create a file");
				return 1;
			}
			while (left != 0) {
				size_t take = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
				ssize_t n = read(in, buffer, take);

				if (n <= 0 || write(out, buffer, (size_t)n) != n) {
					say("unpack: a short read or write (is /var big enough?)");
					return 1;
				}
				left -= (unsigned long long)n;
			}
			close(out);
			if (size % 512 != 0) {
				lseek(in, (off_t)(512 - size % 512), SEEK_CUR);
			}
			files++;
			bytes += size;
		} else {
			say("unpack: refused an entry of type %c", type);
			return 1;
		}
	}
	close(in);
	unlink(file);   /* (twice the app's size in a /var in RAM otherwise) */
	say("unpack: %u files, %llu bytes, %u directories, %u links", files, bytes, dirs, links);
	return 0;
}

/* ---- containers and the description ---- */

static void
load_frameworks(void)
{
	dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
	dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
	dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
}

static NSString *
container(NSString *kind, NSString *identifier)
{
	Class class = NSClassFromString(kind);
	NSError *error = nil;
	id made;

	if (class == Nil) {
		say("no class %s", kind.UTF8String);
		return nil;
	}
	made = [class containerWithIdentifier:identifier createIfNecessary:YES existed:NULL error:&error];
	if (made == nil) {
		say("%s for %s: %s %ld", kind.UTF8String, identifier.UTF8String, error.domain.UTF8String, (long)error.code);
		return nil;
	}
	return [made url].path;
}

static NSDictionary *
environment(NSString *home)
{
	return @{ @"CFFIXED_USER_HOME": home, @"HOME": home, @"TMPDIR": [home stringByAppendingPathComponent:@"tmp"] };
}

/* What a bundle's description has in common, the app's and an extension's. */
static NSMutableDictionary *
describe(NSString *path, NSDictionary *info, NSDictionary *entitlements, NSString *kind, NSString *type)
{
	NSString *bundle = info[@"CFBundleIdentifier"];
	NSString *home = container(kind, bundle);
	NSMutableDictionary *description = [NSMutableDictionary dictionary];
	NSMutableDictionary *groups = [NSMutableDictionary dictionary];

	if (entitlements != nil) {
		description[@"Entitlements"] = entitlements;
	}
	description[@"ApplicationType"] = type;
	description[@"CFBundleIdentifier"] = bundle;
	description[@"CodeInfoIdentifier"] = bundle;
	description[@"CompatibilityState"] = @0;
	description[@"IsContainerized"] = @YES;
	description[@"Path"] = path;
	if (claim_signer) {
		description[@"SignerOrganization"] = @"Apple Inc.";
		description[@"SignatureVersion"] = @132352;
		description[@"SignerIdentity"] = @"Apple iPhone OS Application Signing";
	}
	if (home != nil) {
		description[@"Container"] = home;
		description[@"EnvironmentVariables"] = environment(home);
	}
	if (entitlements[@"com.apple.developer.team-identifier"] != nil) {
		description[@"TeamIdentifier"] = entitlements[@"com.apple.developer.team-identifier"];
	}
	for (NSString *group in entitlements[@"com.apple.security.application-groups"]) {
		NSString *shared = container(@"MCMSharedDataContainer", group);

		if (shared != nil) {
			groups[group] = shared;
		}
	}
	if (groups.count != 0) {
		description[@"HasAppGroupContainers"] = @YES;
		description[@"GroupContainers"] = groups;
	}
	return description;
}

static void
own_the_tree(NSString *path)
{
	NSDirectoryEnumerator *walk = [NSFileManager.defaultManager enumeratorAtPath:path];

	chown(path.fileSystemRepresentation, INSTALLD_UID, INSTALLD_UID);
	for (NSString *name in walk) {
		(void)lchown([path stringByAppendingPathComponent:name].fileSystemRepresentation, INSTALLD_UID, INSTALLD_UID);
	}
}

static int
install(const char *dir)
{
	NSString *top = @(dir), *payload = [top stringByAppendingPathComponent:@"Payload"], *name = nil, *from, *to, *bundle, *home;
	NSFileManager *files = NSFileManager.defaultManager;
	NSError *error = nil;
	NSDictionary *info, *entitlements;
	NSMutableDictionary *description, *plugins = [NSMutableDictionary dictionary];
	NSString *pluginRoot;

	for (NSString *one in [files contentsOfDirectoryAtPath:payload error:NULL]) {
		if ([one.pathExtension isEqualToString:@"app"]) {
			name = one;
		}
	}
	if (name == nil) {
		say("install: no .app in the payload");
		return 1;
	}
	from = [payload stringByAppendingPathComponent:name];
	info = [NSDictionary dictionaryWithContentsOfFile:[from stringByAppendingPathComponent:@"Info.plist"]];
	bundle = info[@"CFBundleIdentifier"];
	if (bundle == nil) {
		say("install: its Info.plist has no bundle identifier");
		return 1;
	}
	say("install: %s", bundle.UTF8String);
	home = container(@"MCMAppContainer", bundle);
	if (home == nil) {
		home = [@"/var/containers/Bundle/Application" stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
		[files createDirectoryAtPath:home withIntermediateDirectories:YES attributes:nil error:NULL];
		say("install: no bundle container from containermanagerd: a directory of its own");
	}
	to = [home stringByAppendingPathComponent:name];
	[files removeItemAtPath:to error:NULL];
	if (![files moveItemAtPath:from toPath:to error:&error]) {
		say("install: cannot move it into place: %s %ld", error.domain.UTF8String, (long)error.code);
		return 1;
	}
	say("install: at %s", to.UTF8String);
	own_the_tree(home);

	entitlements = [NSDictionary dictionaryWithContentsOfFile:[top stringByAppendingFormat:@"/ents/%@.plist", bundle]];
	description = describe(to, info, entitlements, @"MCMAppDataContainer", @"User");
	description[@"IsDeletable"] = @YES;
	if (claim_signer) {
		description[@"IsAdHocSigned"] = @YES;
	}
	description[@"LSInstallType"] = @1;
	description[@"HasMIDBasedSINF"] = @0;
	description[@"MissingSINF"] = @0;
	description[@"FamilyID"] = @0;
	description[@"IsOnDemandInstallCapable"] = @0;

	pluginRoot = [to stringByAppendingPathComponent:@"PlugIns"];
	for (NSString *one in [files contentsOfDirectoryAtPath:pluginRoot error:NULL]) {
		NSString *path = [pluginRoot stringByAppendingPathComponent:one];
		NSDictionary *pluginInfo = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
		NSString *pluginBundle = pluginInfo[@"CFBundleIdentifier"];
		NSMutableDictionary *plugin;

		if (pluginBundle == nil) {
			continue;
		}
		plugin = describe(path, pluginInfo, [NSDictionary dictionaryWithContentsOfFile:[top stringByAppendingFormat:@"/ents/%@.plist", pluginBundle]],
		    @"MCMPluginKitPluginDataContainer", @"PluginKitPlugin");
		plugin[@"PluginOwnerBundleID"] = bundle;
		plugins[pluginBundle] = plugin;
	}
	description[@"_LSBundlePlugins"] = plugins;
	say("install: %lu extensions, %lu app groups, signer fields %s", (unsigned long)plugins.count, (unsigned long)[description[@"GroupContainers"] count], claim_signer ? "CLAIMED" : "left out");
	if (![[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] registerApplicationDictionary:description]) {
		say("install: LaunchServices REFUSED it");
		return 1;
	}
	say("install: REGISTERED");
	return 0;
}

static int
state(NSString *bundle)
{
	id proxy = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:bundle];

	if (proxy == nil || ![proxy bundleURL]) {
		say("state: %s is not known", bundle.UTF8String);
		return 1;
	}
	say("state: %s: type %s, at %s, installed %d, %lu extensions, a data container: %s", bundle.UTF8String, [proxy applicationType].UTF8String, [proxy bundleURL].path.UTF8String,
	    (int)[proxy isInstalled], (unsigned long)[proxy plugInKitPlugins].count, [proxy dataContainerURL] != nil ? "yes" : "no");
	if ([proxy respondsToSelector:@selector(signerIdentity)]) {
		say("state: signer identity %s", [proxy signerIdentity].UTF8String ?: "none");
	}
	if ([proxy respondsToSelector:@selector(profileValidated)]) {
		say("state: profile validated %d", (int)[proxy profileValidated]);
	}
	if ([proxy respondsToSelector:@selector(isRestricted)]) {
		say("state: restricted %d", (int)[proxy isRestricted]);
	}
	return 0;
}

/*
 * Through FrontBoard's own service, which answers with a reason, where LaunchServices' open answers only yes or no and the system's logs cost a boot: run 123316, lsd's and SpringBoard's on, took
 * 388 s to the home screen and over 82 C. An error's text here is FrontBoard's, about the app.
 */
static int
open_app(NSString *bundle)
{
	Class class = NSClassFromString(@"FBSOpenApplicationService");
	id service = class != Nil ? [class serviceWithDefaultShellEndpoint] : nil;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block bool started = false;

	if (service == nil) {
		BOOL opened = [[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openApplicationWithBundleID:bundle];

		say("open: no FrontBoard service; LaunchServices says %d", (int)opened);
		return opened ? 0 : 1;
	}
	[service openApplication:bundle withOptions:nil completion:^(id process, NSError *error) {
		if (error == nil) {
			started = true;
			say("open: LAUNCHED");
		} else {
			NSError *under = error.userInfo[NSUnderlyingErrorKey];

			say("open: REFUSED: %s %ld: %s", error.domain.UTF8String, (long)error.code, error.localizedDescription.UTF8String);
			if (error.localizedFailureReason != nil) {
				say("open:   because: %s", error.localizedFailureReason.UTF8String);
			}
			if (under != nil) {
				say("open:   under it: %s %ld: %s", under.domain.UTF8String, (long)under.code, under.localizedDescription.UTF8String);
			}
		}
		dispatch_semaphore_signal(done);
	}];
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) != 0) {
		say("open: no answer in 20 s");
		return 1;
	}
	return started ? 0 : 1;
}

static int
remove_app(NSString *bundle)
{
	id proxy = [NSClassFromString(@"LSApplicationProxy") applicationProxyForIdentifier:bundle];
	NSURL *where = [proxy bundleURL];
	BOOL done;

	if (where == nil) {
		say("remove: %s is not known", bundle.UTF8String);
		return 1;
	}
	done = [[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] unregisterApplication:where];
	say("remove: unregistered: %d", (int)done);
	[NSFileManager.defaultManager removeItemAtPath:where.path.stringByDeletingLastPathComponent error:NULL];
	return done ? 0 : 1;
}

int
main(int argc, char **argv)
{
	int error = 0;

	alarm(300);
	@autoreleasepool {
		load_frameworks();
		for (int i = 1; i < argc && error == 0; i++) {
			say("step %s", argv[i]);
			if (strcmp(argv[i], "alarm") == 0 && i + 1 < argc) {
				alarm((unsigned)atoi(argv[++i]));
			} else if (strcmp(argv[i], "claim") == 0) {
				claim_signer = true;
			} else if (strcmp(argv[i], "wait-net") == 0 && i + 1 < argc) {
				error = wait_net(atof(argv[++i]));
			} else if (strcmp(argv[i], "fetch") == 0 && i + 5 < argc) {
				error = fetch(argv[i + 1], argv[i + 2], argv[i + 3], argv[i + 4], argv[i + 5]);
				i += 5;
			} else if (strcmp(argv[i], "unpack") == 0 && i + 2 < argc) {
				error = unpack(argv[i + 1], argv[i + 2]);
				i += 2;
			} else if (strcmp(argv[i], "install") == 0 && i + 1 < argc) {
				error = install(argv[++i]);
			} else if (strcmp(argv[i], "state") == 0 && i + 1 < argc) {
				error = state(@(argv[++i]));
			} else if (strcmp(argv[i], "open") == 0 && i + 1 < argc) {
				error = open_app(@(argv[++i]));
			} else if (strcmp(argv[i], "remove") == 0 && i + 1 < argc) {
				error = remove_app(@(argv[++i]));
			} else {
				say("what is %s?", argv[i]);
				error = 2;
			}
		}
	}
	say("done: %d", error);
	return error;
}
