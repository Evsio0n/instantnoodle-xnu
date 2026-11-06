/* See modem_fat.h. The layouts are Microsoft's FAT specification's; no code of anybody's FAT driver is used. */
#include "modem_fat.h"

#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define FAT_EOC_MIN     0x0ffffff8u
#define ENTRY_SIZE      32
#define ATTR_LONG       0x0f
#define ATTR_DIRECTORY  0x10
#define ATTR_VOLUME     0x08

static uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
le32(const uint8_t *p)
{
	return le16(p) | le16(p + 2) << 16;
}

int
modem_fat_mount(struct modem_fat *fat, const struct modem_fat_ops *ops, void *context)
{
	uint8_t boot[512];
	uint32_t sectors_per_cluster, reserved, fats, root_entries, fat_sectors, total, root_sectors, data_sectors;
	int error;

	memset(fat, 0, sizeof(*fat));
	fat->ops = ops;
	fat->context = context;
	if ((error = ops->read(context, 0, boot, sizeof(boot))) != 0) {
		return error;
	}
	if (boot[510] != 0x55 || boot[511] != 0xaa) {
		return EINVAL;
	}
	fat->bytes_per_sector = le16(boot + 11);
	sectors_per_cluster = boot[13];
	reserved = le16(boot + 14);
	fats = boot[16];
	root_entries = le16(boot + 17);
	total = le16(boot + 19) != 0 ? le16(boot + 19) : le32(boot + 32);
	fat_sectors = le16(boot + 22) != 0 ? le16(boot + 22) : le32(boot + 36);
	if (fat->bytes_per_sector < 512 || fat->bytes_per_sector > 4096 || (fat->bytes_per_sector & (fat->bytes_per_sector - 1)) != 0 ||
	    sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 || fats == 0 || fat_sectors == 0 || total == 0) {
		return EINVAL;
	}
	root_sectors = (root_entries * ENTRY_SIZE + fat->bytes_per_sector - 1) / fat->bytes_per_sector;
	if (reserved + fats * fat_sectors + root_sectors >= total) {
		return EINVAL;
	}
	data_sectors = total - reserved - fats * fat_sectors - root_sectors;
	fat->bytes_per_cluster = fat->bytes_per_sector * sectors_per_cluster;
	fat->clusters = data_sectors / sectors_per_cluster;
	fat->fat_type = fat->clusters < 4085 ? 12 : fat->clusters < 65525 ? 16 : 32;
	fat->fat_offset = (uint64_t)reserved * fat->bytes_per_sector;
	fat->root_offset = fat->fat_offset + (uint64_t)fats * fat_sectors * fat->bytes_per_sector;
	fat->data_offset = fat->root_offset + (uint64_t)root_sectors * fat->bytes_per_sector;
	fat->root_entries = root_entries;
	fat->root_cluster = fat->fat_type == 32 ? le32(boot + 44) : 0;
	return 0;
}

/* The cluster after `cluster'; at or above FAT_EOC_MIN at the chain's end. */
static int
next_cluster(struct modem_fat *fat, uint32_t cluster, uint32_t *next)
{
	uint8_t bytes[4] = { 0, 0, 0, 0 };
	uint64_t at = fat->fat_offset + (fat->fat_type == 32 ? (uint64_t)cluster * 4 : fat->fat_type == 16 ? (uint64_t)cluster * 2 : (uint64_t)cluster * 3 / 2);
	int error = fat->ops->read(fat->context, at, bytes, fat->fat_type == 32 ? 4 : 2);

	if (error != 0) {
		return error;
	}
	if (fat->fat_type == 32) {
		*next = le32(bytes) & 0x0fffffffu;
	} else if (fat->fat_type == 16) {
		*next = le16(bytes);
		*next = *next >= 0xfff8 ? FAT_EOC_MIN : *next;
	} else {
		*next = cluster & 1 ? le16(bytes) >> 4 : le16(bytes) & 0xfff;
		*next = *next >= 0xff8 ? FAT_EOC_MIN : *next;
	}
	return 0;
}

static uint64_t
cluster_offset(const struct modem_fat *fat, uint32_t cluster)
{
	return fat->data_offset + (uint64_t)(cluster - 2) * fat->bytes_per_cluster;
}

static char
fold(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool
same(const char *a, size_t length, const char *b)
{
	size_t i;

	for (i = 0; i < length && b[i] != 0; i++) {
		if (fold(a[i]) != fold(b[i])) {
			return false;
		}
	}
	return i == length && b[i] == 0;
}

/* "NAME    EXT" as "name.ext" */
static void
short_name(const uint8_t *entry, char *name)
{
	size_t n = 0;

	for (int i = 0; i < 8 && entry[i] != ' '; i++) {
		name[n++] = (char)entry[i];
	}
	if (entry[8] != ' ') {
		name[n++] = '.';
		for (int i = 8; i < 11 && entry[i] != ' '; i++) {
			name[n++] = (char)entry[i];
		}
	}
	name[n] = 0;
}

/* a long name's piece, into its place in `name' (ASCII only: the names here are) */
static void
long_piece(const uint8_t *entry, char *name, size_t room)
{
	static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	size_t base = ((size_t)(entry[0] & 0x1f) - 1) * 13;

	for (int i = 0; i < 13; i++) {
		uint32_t c = le16(entry + at[i]);

		if (base + (size_t)i + 1 >= room) {
			return;
		}
		if (c == 0xffff) {
			return;
		}
		name[base + (size_t)i] = c < 0x80 ? (char)c : '?';
		if (c == 0) {
			return;
		}
	}
	if ((entry[0] & 0x40) && base + 13 < room) {
		name[base + 13] = 0;
	}
}

/*
 * The entry called `part' (of `length' bytes) in a directory: its first cluster, size and whether it is a directory.
 * `dir_cluster' 0: FAT12/16's fixed root.
 */
static int
find_in_dir(struct modem_fat *fat, uint32_t dir_cluster, const char *part, size_t length, uint32_t *first, uint64_t *size, bool *directory)
{
	char long_name[256], name[16];
	bool have_long = false;
	uint8_t entry[ENTRY_SIZE];
	uint64_t at = dir_cluster == 0 ? fat->root_offset : cluster_offset(fat, dir_cluster);
	uint32_t in_cluster = 0, seen = 0, cluster = dir_cluster;
	int error;

	long_name[0] = 0;
	for (;;) {
		if (dir_cluster == 0 && seen >= fat->root_entries) {
			return ENOENT;
		}
		if ((error = fat->ops->read(fat->context, at + in_cluster, entry, sizeof(entry))) != 0) {
			return error;
		}
		seen++;
		in_cluster += ENTRY_SIZE;
		if (entry[0] == 0) {
			return ENOENT;
		}
		if (entry[0] != 0xe5) {
			if (entry[11] == ATTR_LONG) {
				long_piece(entry, long_name, sizeof(long_name));
				have_long = true;
			} else if (!(entry[11] & ATTR_VOLUME)) {
				short_name(entry, name);
				if ((have_long && same(part, length, long_name)) || same(part, length, name)) {
					*first = le16(entry + 26) | le16(entry + 20) << 16;
					*size = le32(entry + 28);
					*directory = (entry[11] & ATTR_DIRECTORY) != 0;
					return 0;
				}
				have_long = false;
				long_name[0] = 0;
			} else {
				have_long = false;
			}
		}
		if (dir_cluster != 0 && in_cluster >= fat->bytes_per_cluster) {
			uint32_t next;

			if ((error = next_cluster(fat, cluster, &next)) != 0) {
				return error;
			}
			if (next < 2 || next >= FAT_EOC_MIN) {
				return ENOENT;
			}
			cluster = next;
			at = cluster_offset(fat, cluster);
			in_cluster = 0;
		}
	}
}

int
modem_fat_open(struct modem_fat *fat, const char *path, struct modem_fat_file *file)
{
	uint32_t dir = fat->root_cluster, first = 0, cluster;
	uint64_t size = 0;
	bool directory = true;
	int error;

	memset(file, 0, sizeof(*file));
	while (*path != 0) {
		size_t length = 0;

		while (path[length] != 0 && path[length] != '/') {
			length++;
		}
		if (!directory) {
			return ENOTDIR;
		}
		if ((error = find_in_dir(fat, dir, path, length, &first, &size, &directory)) != 0) {
			return error;
		}
		dir = first;
		path += length;
		if (*path == '/') {
			path++;
		}
	}
	if (directory) {
		return EISDIR;
	}
	file->size = size;
	file->count = (uint32_t)((size + fat->bytes_per_cluster - 1) / fat->bytes_per_cluster);
	if (file->count == 0) {
		return 0;
	}
	if ((file->chain = fat->ops->alloc(fat->context, (size_t)file->count * sizeof(uint32_t))) == NULL) {
		return ENOMEM;
	}
	cluster = first;
	for (uint32_t i = 0; i < file->count; i++) {
		if (cluster < 2 || cluster >= FAT_EOC_MIN || cluster - 2 >= fat->clusters) {
			modem_fat_close(fat, file);
			return EIO;
		}
		file->chain[i] = cluster;
		if ((error = next_cluster(fat, cluster, &cluster)) != 0) {
			modem_fat_close(fat, file);
			return error;
		}
	}
	return 0;
}

int
modem_fat_read(struct modem_fat *fat, const struct modem_fat_file *file, uint64_t offset, void *buffer, size_t length)
{
	uint8_t *to = buffer;

	if (offset > file->size || length > file->size - offset) {
		return EINVAL;
	}
	while (length > 0) {
		uint32_t index = (uint32_t)(offset / fat->bytes_per_cluster), within = (uint32_t)(offset % fat->bytes_per_cluster);
		size_t take = fat->bytes_per_cluster - within;
		int error;

		/* (consecutive clusters in one read: firmware is mostly laid down in a line) */
		while (index + (uint32_t)(take / fat->bytes_per_cluster) + (take % fat->bytes_per_cluster != 0 ? 0 : 0) < file->count && take < length &&
		    file->chain[index + (take + within) / fat->bytes_per_cluster] == file->chain[index] + (take + within) / fat->bytes_per_cluster) {
			take += fat->bytes_per_cluster;
		}
		take = take < length ? take : length;
		if ((error = fat->ops->read(fat->context, cluster_offset(fat, file->chain[index]) + within, to, take)) != 0) {
			return error;
		}
		to += take;
		offset += take;
		length -= take;
	}
	return 0;
}

void
modem_fat_close(struct modem_fat *fat, struct modem_fat_file *file)
{
	if (file->chain != NULL) {
		fat->ops->free(fat->context, file->chain, (size_t)file->count * sizeof(uint32_t));
		file->chain = NULL;
	}
}
