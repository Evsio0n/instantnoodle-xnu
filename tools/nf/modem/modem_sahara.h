/*
 * A Sahara server: how the SDX55's secondary boot loader asks the host for the rest of its firmware, over MHI's SAHARA channels 2 and 3 once it runs. The packets and their
 * order are as Linux's drivers/accel/qaic/sahara.c has them (read as data), and Android's closed `ks` does this on this phone. It knows nothing of any kernel or of MHI:
 * struct modem_sahara_ops sends and finds images, so that it runs the same in tools/nf/modem/test_modem.c against a device simulated in software.
 */
#ifndef _MODEM_SAHARA_H_
#define _MODEM_SAHARA_H_

#ifdef KERNEL
#include <sys/types.h>
#include <stdbool.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

struct modem_sahara_ops {
	/* one packet, or a piece of a transfer with `more' to come in the same one (MHI's chain); 0, or an error */
	int (*send)(void *context, const void *data, size_t length, bool more);
	/* image `id': its size and a handle for reads; 0, or an error when there is no such image */
	int (*image_open)(void *context, uint32_t id, uint64_t *size, void **image);
	int (*image_read)(void *context, void *image, uint64_t offset, void *buffer, size_t length);
	void (*say)(void *context, const char *format, ...) __attribute__((format(printf, 2, 3)));
};

enum modem_sahara_result { MODEM_SAHARA_MORE, MODEM_SAHARA_DONE, MODEM_SAHARA_FAILED };

#define MODEM_SAHARA_CHUNK      0xfff0u         /* a piece of data, as Linux's SAHARA_READ_MAX_SIZE */
#define MODEM_SAHARA_IMAGES     32

struct modem_sahara {
	const struct modem_sahara_ops *ops;
	void *context;
	uint8_t *chunk;                 /* MODEM_SAHARA_CHUNK bytes, the caller's */
	uint32_t current_id;
	void *current;
	uint64_t current_size;
	bool have_current;
	bool memory_debug_offered;      /* SBL crash offer, never read; distinguish from a wire failure */
	uint32_t ids[MODEM_SAHARA_IMAGES];
	uint64_t sent[MODEM_SAHARA_IMAGES];
	unsigned images;
	uint64_t total;
};

void modem_sahara_init(struct modem_sahara *s, const struct modem_sahara_ops *ops, void *context, uint8_t *chunk);
/* One packet from the device. */
enum modem_sahara_result modem_sahara_input(struct modem_sahara *s, const void *packet, size_t length);

#endif
