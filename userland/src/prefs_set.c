/*
 * prefs_set: a preference set through cfprefsd, the way an application's own setting is, so that the daemon's
 * cache and its file agree and every program that asks afterwards gets it (a file written past cfprefsd is not
 * seen by a program cfprefsd has already answered).
 *
 * usage: prefs_set DOMAIN USER KEY TYPE VALUE
 *   DOMAIN   an application's domain (com.apple.mobilesafari), or -g for every application's (NSGlobalDomain)
 *   USER     mobile or root
 *   TYPE     bool (true/false/1/0), int, or string
 * It prints what the domain answers for KEY afterwards. Run as root (rc.jobs), it may write mobile's.
 *
 * First use (backlog item 11, 2026-11-04): WebKit's PreferPageRenderingUpdatesNear60FPSEnabled off for a RAM test
 * -- WebKit reads its WebKitExperimental%@ keys from the defaults -- to see whether Safari's pages then go past 45
 * frames a second on the 90 Hz panel.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	CFStringRef domain, user, key;
	CFPropertyListRef value = NULL, back;
	char text[256] = "";

	if (argc != 6) {
		fprintf(stderr, "usage: prefs_set DOMAIN|-g mobile|root KEY bool|int|string VALUE\n");
		return 2;
	}
	domain = strcmp(argv[1], "-g") == 0 ? kCFPreferencesAnyApplication :
	    CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
	user = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
	key = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
	if (strcmp(argv[4], "bool") == 0) {
		value = strcmp(argv[5], "true") == 0 || strcmp(argv[5], "1") == 0 ? kCFBooleanTrue : kCFBooleanFalse;
	} else if (strcmp(argv[4], "int") == 0) {
		long long n = strtoll(argv[5], NULL, 0);

		value = CFNumberCreate(NULL, kCFNumberLongLongType, &n);
	} else if (strcmp(argv[4], "string") == 0) {
		value = CFStringCreateWithCString(NULL, argv[5], kCFStringEncodingUTF8);
	} else {
		fprintf(stderr, "prefs_set: the type is bool, int or string\n");
		return 2;
	}
	CFPreferencesSetValue(key, value, domain, user, kCFPreferencesAnyHost);
	if (!CFPreferencesSynchronize(domain, user, kCFPreferencesAnyHost)) {
		printf("prefs_set: %s %s %s: cfprefsd did not take it\n", argv[1], argv[2], argv[3]);
		return 1;
	}
	back = CFPreferencesCopyValue(key, domain, user, kCFPreferencesAnyHost);
	if (back == NULL) {
		strlcpy(text, "nothing", sizeof(text));
	} else if (CFGetTypeID(back) == CFBooleanGetTypeID()) {
		strlcpy(text, CFBooleanGetValue(back) ? "true" : "false", sizeof(text));
	} else if (CFGetTypeID(back) == CFStringGetTypeID()) {
		CFStringGetCString(back, text, sizeof(text), kCFStringEncodingUTF8);
	} else if (CFGetTypeID(back) == CFNumberGetTypeID()) {
		long long n = 0;

		CFNumberGetValue(back, kCFNumberLongLongType, &n);
		snprintf(text, sizeof(text), "%lld", n);
	} else {
		strlcpy(text, "(another type)", sizeof(text));
	}
	printf("prefs_set: %s %s %s is now %s\n", argv[1], argv[2], argv[3], text);
	return 0;
}
