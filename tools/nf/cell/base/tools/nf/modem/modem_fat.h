/*
 * A FAT reader, for what the modem is given: modem_a is a FAT volume and image/sdx55m/ in it holds the SDX55's firmware (84 MB of it in one file, too much for the
 * boot bundle's 96 MB slots, so it is read from the phone's storage as it is asked for). Read only. It knows nothing of any kernel: its reads and its memory come
 * through struct modem_fat_ops, so that it runs the same on the phone and in tools/nf/modem/test_modem.c on the computer that builds it.
 */
#ifndef _MODEM_FAT_H_
#define _MODEM_FAT_H_

#ifdef KERNEL
#include <sys/types.h>
#include <stdbool.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

struct modem_fat_ops {
	int (*read)(void *context, uint64_t offset, void *buffer, size_t length);       /* of the volume; 0, or an error */
	void *(*alloc)(void *context, size_t size);
	void (*free)(void *context, void *memory, size_t size);
};

struct modem_fat {
	const struct modem_fat_ops *ops;
	void *context;
	uint32_t bytes_per_sector, bytes_per_cluster, fat_type;
	uint64_t fat_offset, root_offset, data_offset;  /* in the volume */
	uint32_t root_entries, root_cluster, clusters;
};

struct modem_fat_file {
	uint64_t size;
	uint32_t *chain;                /* the file's clusters, in order */
	uint32_t count;
};

int modem_fat_mount(struct modem_fat *fat, const struct modem_fat_ops *ops, void *context);
/* `path' as "image/sdx55m/sbl1.mbn", case not minded. 0, or an error (ENOENT). */
int modem_fat_open(struct modem_fat *fat, const char *path, struct modem_fat_file *file);
int modem_fat_read(struct modem_fat *fat, const struct modem_fat_file *file, uint64_t offset, void *buffer, size_t length);
void modem_fat_close(struct modem_fat *fat, struct modem_fat_file *file);

#endif
