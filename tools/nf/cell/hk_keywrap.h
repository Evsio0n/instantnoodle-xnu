/* AES key wrap (RFC 3394) under a 256-bit key, for the key store's stand-in: hk_keywrap.c. */
#ifndef HK_KEYWRAP_H
#define HK_KEYWRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The key that wraps the keychain's keys, made of the root key: two blocks of AES under the root, of a fixed text. */
void hk_keywrap_kek(const uint8_t root[32], uint8_t kek[32]);
/* length: 16 to 64 bytes, a multiple of 8. wrapped gets length + 8 bytes. False: a length that cannot be wrapped. */
bool hk_keywrap(const uint8_t kek[32], const uint8_t *key, size_t length, uint8_t *wrapped);
/* length: that of the wrapped key, 24 to 72. key gets length - 8 bytes. False: not wrapped under this key (or damaged). */
bool hk_keyunwrap(const uint8_t kek[32], const uint8_t *wrapped, size_t length, uint8_t *key);

/* The root key the key master keeps for us (hk_tz.c, boot-arg tz_keys=). False: there is none in this boot. */
bool hk_tz_root_key(uint8_t key[32]);
/* For kern.hackios.tz_keys: 0 a key wrapped under the root key, 1 unwrapped, 2 wrapped with the old pattern, 3 unwrapped
 * of it, 4 an unwrap refused. */
void hk_tz_count(unsigned which);

#ifdef __cplusplus
}
#endif

#endif
