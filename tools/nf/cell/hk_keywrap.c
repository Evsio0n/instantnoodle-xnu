/*
 * AES KEY WRAP (RFC 3394) UNDER A 256-BIT KEY, for the key store's stand-in (iokit/Kernel/MiniAppleKeyStore.cpp).
 *
 * Why here and written out: the keychain hands the key store each item's key to be wrapped, and sizes its buffers for AES
 * key wrap (eight bytes more than the key). The stand-in used to answer with a pattern that anybody can undo. With a root
 * key that TrustZone's key master keeps (hk_tz.c, boot-arg tz_keys=) there is something to wrap with, and this is the
 * wrapping. This kernel has no corecrypto behind libkern's aes.h (nothing registers g_crypto_funcs), so the cipher is
 * here: AES-256 as FIPS 197 writes it, a block at a time, by table look-up. It is not constant-time and need not be fast:
 * a keychain item's key is 32 or 64 bytes, six rounds of five or nine blocks.
 *
 * Checked on the computer against FIPS 197's AES-256 example and RFC 3394's 256-bit vectors: the test is at the end of
 * this file (cc -DHK_KEYWRAP_TEST hk_keywrap.c).
 */
#ifdef KERNEL
#include <libkern/libkern.h>
#include <string.h>
#else
#include <stdio.h>
#include <string.h>
#endif
#include "hk_keywrap.h"

static const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

struct hk_aes {
	uint8_t round[15][16];          /* the round keys of AES-256 */
	uint8_t back[256];              /* the S-box read the other way */
};

static uint8_t
twice(uint8_t x)
{
	return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0));
}

static uint8_t
times(uint8_t x, uint8_t by)
{
	uint8_t product = 0;

	for (; by != 0; by >>= 1, x = twice(x)) {
		if (by & 1) {
			product ^= x;
		}
	}
	return product;
}

static void
aes_start(struct hk_aes *aes, const uint8_t key[32])
{
	uint8_t words[60][4];
	uint8_t constant = 1;

	for (unsigned i = 0; i < 256; i++) {
		aes->back[sbox[i]] = (uint8_t)i;
	}
	memcpy(words, key, 32);
	for (unsigned i = 8; i < 60; i++) {
		uint8_t next[4];

		memcpy(next, words[i - 1], 4);
		if (i % 8 == 0) {
			const uint8_t first = next[0];

			next[0] = sbox[next[1]] ^ constant;
			next[1] = sbox[next[2]];
			next[2] = sbox[next[3]];
			next[3] = sbox[first];
			constant = twice(constant);
		} else if (i % 8 == 4) {
			for (unsigned j = 0; j < 4; j++) {
				next[j] = sbox[next[j]];
			}
		}
		for (unsigned j = 0; j < 4; j++) {
			words[i][j] = words[i - 8][j] ^ next[j];
		}
	}
	memcpy(aes->round, words, sizeof(aes->round));
}

static void
add_round(uint8_t block[16], const uint8_t key[16])
{
	for (unsigned i = 0; i < 16; i++) {
		block[i] ^= key[i];
	}
}

static void
aes_encrypt(const struct hk_aes *aes, uint8_t block[16])
{
	add_round(block, aes->round[0]);
	for (unsigned round = 1; round <= 14; round++) {
		uint8_t moved[16];

		/* (bytes replaced, and each row turned left by its number: the block is four columns of four) */
		for (unsigned column = 0; column < 4; column++) {
			for (unsigned row = 0; row < 4; row++) {
				moved[4 * column + row] = sbox[block[4 * ((column + row) % 4) + row]];
			}
		}
		if (round != 14) {
			for (unsigned column = 0; column < 4; column++) {
				const uint8_t *in = moved + 4 * column;
				uint8_t *out = block + 4 * column;

				out[0] = twice(in[0]) ^ twice(in[1]) ^ in[1] ^ in[2] ^ in[3];
				out[1] = in[0] ^ twice(in[1]) ^ twice(in[2]) ^ in[2] ^ in[3];
				out[2] = in[0] ^ in[1] ^ twice(in[2]) ^ twice(in[3]) ^ in[3];
				out[3] = twice(in[0]) ^ in[0] ^ in[1] ^ in[2] ^ twice(in[3]);
			}
		} else {
			memcpy(block, moved, 16);
		}
		add_round(block, aes->round[round]);
	}
}

static void
aes_decrypt(const struct hk_aes *aes, uint8_t block[16])
{
	add_round(block, aes->round[14]);
	for (unsigned round = 13;; round--) {
		uint8_t moved[16];

		for (unsigned column = 0; column < 4; column++) {
			for (unsigned row = 0; row < 4; row++) {
				moved[4 * ((column + row) % 4) + row] = aes->back[block[4 * column + row]];
			}
		}
		add_round(moved, aes->round[round]);
		if (round == 0) {
			memcpy(block, moved, 16);
			return;
		}
		for (unsigned column = 0; column < 4; column++) {
			const uint8_t *in = moved + 4 * column;
			uint8_t *out = block + 4 * column;

			out[0] = times(in[0], 14) ^ times(in[1], 11) ^ times(in[2], 13) ^ times(in[3], 9);
			out[1] = times(in[0], 9) ^ times(in[1], 14) ^ times(in[2], 11) ^ times(in[3], 13);
			out[2] = times(in[0], 13) ^ times(in[1], 9) ^ times(in[2], 14) ^ times(in[3], 11);
			out[3] = times(in[0], 11) ^ times(in[1], 13) ^ times(in[2], 9) ^ times(in[3], 14);
		}
	}
}

void
hk_keywrap_kek(const uint8_t root[32], uint8_t kek[32])
{
	static const char text[] = "HackiOS keys v1";    /* (15 letters, and the half's number makes the block) */
	struct hk_aes aes;

	aes_start(&aes, root);
	for (unsigned half = 0; half < 2; half++) {
		memcpy(kek + 16 * half, text, 15);
		kek[16 * half + 15] = (uint8_t)(half + 1);
		aes_encrypt(&aes, kek + 16 * half);
	}
	memset(&aes, 0, sizeof(aes));
}

/* RFC 3394, section 2.2: six rounds over the key's 8-byte pieces, a 64-bit register in front that the last step checks. */
bool
hk_keywrap(const uint8_t kek[32], const uint8_t *key, size_t length, uint8_t *wrapped)
{
	const size_t pieces = length / 8;
	struct hk_aes aes;
	uint8_t block[16];

	if (length < 16 || length > 64 || length % 8 != 0) {
		return false;
	}
	aes_start(&aes, kek);
	memset(wrapped, 0xa6, 8);
	memcpy(wrapped + 8, key, length);
	for (unsigned round = 0; round < 6; round++) {
		for (size_t i = 1; i <= pieces; i++) {
			const uint64_t count = pieces * round + i;

			memcpy(block, wrapped, 8);
			memcpy(block + 8, wrapped + 8 * i, 8);
			aes_encrypt(&aes, block);
			for (unsigned j = 0; j < 8; j++) {
				block[7 - j] ^= (uint8_t)(count >> 8 * j);
			}
			memcpy(wrapped, block, 8);
			memcpy(wrapped + 8 * i, block + 8, 8);
		}
	}
	memset(&aes, 0, sizeof(aes));
	memset(block, 0, sizeof(block));
	return true;
}

bool
hk_keyunwrap(const uint8_t kek[32], const uint8_t *wrapped, size_t length, uint8_t *key)
{
	const size_t pieces = length / 8 - 1;
	struct hk_aes aes;
	uint8_t block[16], front[8], work[64];
	uint8_t wrong = 0;

	if (length < 24 || length > 72 || length % 8 != 0) {
		return false;
	}
	aes_start(&aes, kek);
	memcpy(front, wrapped, 8);
	memcpy(work, wrapped + 8, length - 8);
	for (unsigned round = 6; round-- > 0;) {
		for (size_t i = pieces; i >= 1; i--) {
			const uint64_t count = pieces * round + i;

			memcpy(block, front, 8);
			for (unsigned j = 0; j < 8; j++) {
				block[7 - j] ^= (uint8_t)(count >> 8 * j);
			}
			memcpy(block + 8, work + 8 * (i - 1), 8);
			aes_decrypt(&aes, block);
			memcpy(front, block, 8);
			memcpy(work + 8 * (i - 1), block + 8, 8);
		}
	}
	for (unsigned j = 0; j < 8; j++) {
		wrong |= front[j] ^ 0xa6;
	}
	if (wrong == 0) {
		memcpy(key, work, length - 8);
	}
	memset(&aes, 0, sizeof(aes));
	memset(block, 0, sizeof(block));
	memset(work, 0, sizeof(work));
	return wrong == 0;
}

#ifdef HK_KEYWRAP_TEST
/* cc -DHK_KEYWRAP_TEST -fsanitize=address,undefined -o /tmp/keywrap hk_keywrap.c && /tmp/keywrap */
static int
same(const char *what, const uint8_t *got, const char *hex, size_t length)
{
	char text[2 * 80 + 1];

	for (size_t i = 0; i < length; i++) {
		snprintf(text + 2 * i, 3, "%02x", got[i]);
	}
	printf("%s: %s\n", strcmp(text, hex) == 0 ? "ok  " : "FAIL", what);
	return strcmp(text, hex) != 0;
}

int
main(void)
{
	uint8_t kek[32], data[64], wrapped[72], back[64], block[16];
	struct hk_aes aes;
	int failed = 0;

	for (unsigned i = 0; i < 32; i++) {
		kek[i] = (uint8_t)i;
	}
	/* FIPS 197, appendix C.3 */
	for (unsigned i = 0; i < 16; i++) {
		block[i] = (uint8_t)(i * 0x11);
	}
	aes_start(&aes, kek);
	aes_encrypt(&aes, block);
	failed += same("FIPS 197 C.3, AES-256 of a block", block, "8ea2b7ca516745bfeafc49904b496089", 16);
	aes_decrypt(&aes, block);
	failed += same("and back", block, "00112233445566778899aabbccddeeff", 16);
	/* RFC 3394, 4.3, 4.5 and 4.6: 128, 192 and 256 bits of key data under a 256-bit key */
	for (unsigned i = 0; i < 16; i++) {
		data[i] = (uint8_t)(i * 0x11);
	}
	for (unsigned i = 16; i < 32; i++) {
		data[i] = (uint8_t)(i - 16);
	}
	failed += !hk_keywrap(kek, data, 16, wrapped);
	failed += same("RFC 3394 4.3, 128 bits wrapped", wrapped, "64e8c3f9ce0f5ba263e9777905818a2a93c8191e7d6e8ae7", 24);
	failed += !hk_keyunwrap(kek, wrapped, 24, back) || memcmp(back, data, 16) != 0;
	failed += !hk_keywrap(kek, data, 24, wrapped);
	failed += same("RFC 3394 4.5, 192 bits wrapped", wrapped, "a8f9bc1612c68b3ff6e6f4fbe30e71e4769c8b80a32cb8958cd5d17d6b254da1", 32);
	failed += !hk_keyunwrap(kek, wrapped, 32, back) || memcmp(back, data, 24) != 0;
	failed += !hk_keywrap(kek, data, 32, wrapped);
	failed += same("RFC 3394 4.6, 256 bits wrapped", wrapped,
	    "28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43bfb988b9b7a02dd21", 40);
	failed += !hk_keyunwrap(kek, wrapped, 40, back) || memcmp(back, data, 32) != 0;
	printf("%s: all three unwrap to what went in\n", failed == 0 ? "ok  " : "FAIL");
	/* what must be refused: a changed byte, another key, the lengths the keychain does not use */
	wrapped[17] ^= 1;
	failed += hk_keyunwrap(kek, wrapped, 40, back);
	wrapped[17] ^= 1;
	kek[5] ^= 0x40;
	failed += hk_keyunwrap(kek, wrapped, 40, back);
	kek[5] ^= 0x40;
	failed += hk_keywrap(kek, data, 8, wrapped) || hk_keywrap(kek, data, 20, wrapped) || hk_keywrap(kek, data, 72, wrapped);
	failed += hk_keyunwrap(kek, (const uint8_t *)"HKwr\6\0\0\0abcdefghijklmnopqrstuvwxyz012345", 40, back);
	/* 64 bytes, the keychain's other size, there and back */
	for (unsigned i = 0; i < 64; i++) {
		data[i] = (uint8_t)(200 - i);
	}
	failed += !hk_keywrap(kek, data, 64, wrapped) || !hk_keyunwrap(kek, wrapped, 72, back) || memcmp(back, data, 64) != 0;
	hk_keywrap_kek(kek, back);
	failed += memcmp(back, kek, 32) == 0;
	printf("%s: a changed byte, another key, odd lengths and the old pattern are refused; 64 bytes go there and back\n",
	    failed == 0 ? "ok  " : "FAIL");
	return failed != 0;
}
#endif
