/*
 * cache_dump: what /var/tmp/hk-air holds, on the console, for tools/metal/loop/cache_take.py to take from a run's kept logs (the owner: 「开机前能不能build着色器」).
 *
 * A RAM boot starts with an empty /var, so every boot translates every shader again, seconds of the window server's time. The phone
 * has no other way to hand back a file in a run, and xl keeps the last megabyte of the log every second or so (XL_FOLLOW_KEEP), so each
 * file goes out deflated and in base64, a line at a time, slowly enough that nothing falls between two copies. A line carries its own CRC:
 * what is spliced by another writer is found out, and the next dump fills it in. Translated shaders only, no photo and no name of one.
 *
 *   usage: cache_dump [DIR]
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define CHUNK 300                /* bytes a line: 400 of base64 */
#define PAUSE_US 2500            /* about 150 KB/s of log */

static void
base64(const uint8_t *in, size_t size, char *out)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;

	for (size_t i = 0; i < size; i += 3) {
		const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < size ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < size ? in[i + 2] : 0);

		out[o++] = table[v >> 18 & 63];
		out[o++] = table[v >> 12 & 63];
		out[o++] = i + 1 < size ? table[v >> 6 & 63] : '=';
		out[o++] = i + 2 < size ? table[v & 63] : '=';
	}
	out[o] = 0;
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/var/tmp/hk-air";
	DIR *d = opendir(dir);
	const int console = open("/dev/console", O_WRONLY | O_NOCTTY);
	struct dirent *e;
	unsigned files = 0;
	unsigned long long raw = 0, sent = 0;

	if (d == NULL || console < 0) {
		printf("cache_dump: no %s, or no console\n", dir);
		return 1;
	}
	while ((e = readdir(d)) != NULL) {
		char path[512], line[600], text[CHUNK * 2];
		struct stat st;

		if (e->d_name[0] == '.' || strchr(e->d_name, '.') != strrchr(e->d_name, '.') || (size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path) ||
		    stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
			continue;
		}
		const int fd = open(path, O_RDONLY);
		uint8_t *bytes = malloc((size_t)st.st_size);
		uLongf packed = compressBound((uLong)st.st_size);
		uint8_t *out = malloc(packed);

		if (fd < 0 || bytes == NULL || out == NULL || read(fd, bytes, (size_t)st.st_size) != st.st_size || compress2(out, &packed, bytes, (uLong)st.st_size, 9) != Z_OK) {
			goto next;
		}
		for (size_t at = 0; at < packed; at += CHUNK) {
			const size_t n = packed - at < CHUNK ? packed - at : CHUNK;

			base64(out + at, n, text);
			const int length = snprintf(line, sizeof(line), "hkcache %s %llu %lu %zu %08lx %s\n", e->d_name, (unsigned long long)st.st_size, (unsigned long)packed, at, crc32(0, out + at, (uInt)n), text);

			(void)!write(console, line, (size_t)length);
			usleep(PAUSE_US);
		}
		files++;
		raw += (unsigned long long)st.st_size;
		sent += packed;
next:
		if (fd >= 0) {
			close(fd);
		}
		free(bytes);
		free(out);
	}
	closedir(d);
	dprintf(console, "hkcache-done %u files, %llu bytes, %llu deflated\n", files, raw, sent);
	return 0;
}
