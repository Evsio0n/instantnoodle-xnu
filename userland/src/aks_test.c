/*
 * aks_test: asks the kernel's key store (AppleKeyStore; here the stand-in, iokit/Kernel/MiniAppleKeyStore.cpp) to wrap a
 * key and to unwrap one, as securityd does for every keychain item (selectors 0x0a and 0x0b), and prints what comes back.
 *
 * For the question the keychain cannot answer in a test boot, whose data volume is in memory and gone at its end: is a key
 * wrapped in one boot unwrapped in the next? With boot-arg tz_keys= the stand-in wraps under a root key that TrustZone's
 * key master keeps (tools/nf/cell/hk_tz.c), and the wrapped key printed here, given to the next boot's aks_test, says.
 *
 * usage: aks_test                wraps two known keys (32 bytes, 64 bytes), prints them wrapped ("aks_test: wrapped 32: hex"),
 *                                unwraps them and compares
 *        aks_test unwrap HEX     unwraps HEX and says whether it is the known key of its length
 * The known keys are 0xb0, 0xb1, ... : they protect nothing.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRAP    0x0a
#define UNWRAP  0x0b
#define CLASS   6               /* "after first unlock", what most keychain items are */

static io_connect_t store;

static void
known(uint8_t *key, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		key[i] = (uint8_t)(0xb0 + i);
	}
}

static void
print_hex(const char *what, const uint8_t *bytes, size_t length)
{
	printf("aks_test: %s %zu: ", what, length);
	for (size_t i = 0; i < length; i++) {
		printf("%02x", bytes[i]);
	}
	printf("\n");
}

/* Returns the kernel's answer; *out_length the bytes that came back. */
static kern_return_t
ask(uint32_t selector, const uint8_t *in, size_t in_length, uint8_t *out, size_t *out_length)
{
	const uint64_t scalars[2] = { 0, CLASS };
	uint64_t back[1] = { 0 };
	uint32_t backs = 1;

	return IOConnectCallMethod(store, selector, scalars, 2, in, in_length, back, &backs, out, out_length);
}

static int
unwrap_and_say(const uint8_t *wrapped, size_t length)
{
	uint8_t key[80], want[80];
	size_t got = sizeof(key);
	const kern_return_t result = ask(UNWRAP, wrapped, length, key, &got);

	if (result != KERN_SUCCESS) {
		printf("aks_test: unwrapping %zu bytes: REFUSED (%#x)\n", length, result);
		return 1;
	}
	known(want, length - 8);
	if (got == length - 8 && memcmp(key, want, got) == 0) {
		printf("aks_test: unwrapping %zu bytes: the known key of %zu bytes\n", length, got);
		return 0;
	}
	printf("aks_test: unwrapping %zu bytes: %zu bytes that are NOT the known key\n", length, got);
	return 1;
}

int
main(int argc, char **argv)
{
	static const size_t sizes[2] = { 32, 64 };
	const io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("AppleKeyStore"));
	int failed = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (service == MACH_PORT_NULL || IOServiceOpen(service, mach_task_self(), 0, &store) != KERN_SUCCESS) {
		printf("aks_test: no AppleKeyStore to open\n");
		return 1;
	}
	if (argc == 3 && strcmp(argv[1], "unwrap") == 0) {
		uint8_t wrapped[80];
		const size_t length = strlen(argv[2]) / 2;

		if (length < 9 || length > sizeof(wrapped) || strlen(argv[2]) % 2 != 0) {
			printf("aks_test: that is not a wrapped key\n");
			return 1;
		}
		for (size_t i = 0; i < length; i++) {
			unsigned byte = 0;

			if (sscanf(argv[2] + 2 * i, "%2x", &byte) != 1) {
				printf("aks_test: that is not hexadecimal\n");
				return 1;
			}
			wrapped[i] = (uint8_t)byte;
		}
		return unwrap_and_say(wrapped, length);
	}
	for (unsigned i = 0; i < 2; i++) {
		uint8_t key[64], wrapped[80];
		size_t got = sizeof(wrapped);
		kern_return_t result;

		known(key, sizes[i]);
		result = ask(WRAP, key, sizes[i], wrapped, &got);
		if (result != KERN_SUCCESS) {
			printf("aks_test: wrapping %zu bytes: REFUSED (%#x)\n", sizes[i], result);
			failed = 1;
			continue;
		}
		print_hex("wrapped", wrapped, got);
		printf("aks_test: the wrapped key %s\n", got >= 4 && memcmp(wrapped, "HKwr", 4) == 0 ?
		    "is of the old pattern, which protects nothing" : "is not of the old pattern");
		failed |= unwrap_and_say(wrapped, got);
	}
	return failed;
}
