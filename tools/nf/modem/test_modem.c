/*
 * tools/nf/modem/test_modem.c: the FAT reader against a real FAT image, and the Sahara server against a device simulated here: an image's pieces at odd offsets, the 32-bit and the
 * 64-bit ask, big transfers in chained pieces, a second image, the end, and the ways it can go wrong. run.sh builds the image and runs it. No phone.
 */
#include "modem_fat.h"
#include "modem_sahara.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* ---- FAT ---- */
static FILE *volume;
static int read_calls;

static int
vol_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	(void)context;
	read_calls++;
	return fseeko(volume, (off_t)offset, SEEK_SET) == 0 && fread(buffer, 1, length, volume) == length ? 0 : EIO;
}
static void *vol_alloc(void *c, size_t s) { (void)c; return malloc(s); }
static void vol_free(void *c, void *m, size_t s) { (void)c; (void)s; free(m); }
static const struct modem_fat_ops vol_ops = { vol_read, vol_alloc, vol_free };

static uint8_t
pattern(uint32_t seed, uint64_t at)
{
	return (uint8_t)((at * 2654435761u >> 13) ^ seed);
}

static void
check_file(struct modem_fat *fat, const char *path, uint64_t size, uint32_t seed)
{
	struct modem_fat_file f;
	static uint8_t buffer[1 << 20];
	int error = modem_fat_open(fat, path, &f);

	if (error != 0) {
		printf("FAILED %s: open %d\n", path, error);
		failures++;
		return;
	}
	CHECK(f.size == size);
	/* whole, in odd pieces */
	for (uint64_t at = 0; at < size;) {
		size_t take = (size_t)(size - at < 70001 ? size - at : 70001);

		CHECK(modem_fat_read(fat, &f, at, buffer, take) == 0);
		for (size_t i = 0; i < take; i++) {
			if (buffer[i] != pattern(seed, at + i)) {
				printf("FAILED %s: byte at %llu\n", path, (unsigned long long)(at + i));
				failures++;
				goto next;
			}
		}
		at += take;
	}
next:
	if (size > 8192) {
		CHECK(modem_fat_read(fat, &f, size - 100, buffer, 100) == 0);
		CHECK(buffer[99] == pattern(seed, size - 1));
		CHECK(modem_fat_read(fat, &f, 4095, buffer, 4099) == 0 && buffer[0] == pattern(seed, 4095) && buffer[4098] == pattern(seed, 8193));
	}
	CHECK(modem_fat_read(fat, &f, size, buffer, 1) == EINVAL);
	modem_fat_close(fat, &f);
}

static void
test_fat(const char *image)
{
	struct modem_fat fat;
	struct modem_fat_file f;

	volume = fopen(image, "rb");
	CHECK(volume != NULL);
	CHECK(modem_fat_mount(&fat, &vol_ops, NULL) == 0);
	printf("fat: FAT%u, %u bytes a cluster, %u clusters\n", fat.fat_type, fat.bytes_per_cluster, fat.clusters);
	check_file(&fat, "image/sdx55m/sbl1.mbn", 547904, 1);
	check_file(&fat, "IMAGE/SDX55M/SBL1.MBN", 547904, 1);
	check_file(&fat, "image/sdx55m/multi_image_qti.mbn", 12616, 2);
	check_file(&fat, "image/sdx55m/big.mbn", 3333333, 3);
	check_file(&fat, "image/sdx55m/mdmddr.mbn", 40, 4);
	check_file(&fat, "image/sdx55m/frag.mbn", 200000, 5);
	CHECK(modem_fat_open(&fat, "image/sdx55m/none.mbn", &f) == ENOENT);
	CHECK(modem_fat_open(&fat, "image/sdx55m", &f) == EISDIR);
	CHECK(modem_fat_open(&fat, "image/sdx55m/sbl1.mbn/x", &f) == ENOTDIR);
	fclose(volume);
}

/* ---- Sahara ---- */
#define IMAGE_A 21
#define IMAGE_B 12
#define IMAGE_A_SIZE 300000u
#define IMAGE_B_SIZE 2000000u

static uint8_t received[1 << 22];
static size_t received_length;
static int transfers_ended, sends;
static uint32_t last_cmd;
static int fail_send_at = -1;

static int
sim_send(void *context, const void *data, size_t length, bool more)
{
	(void)context;
	if (sends++ == fail_send_at) {
		return EIO;
	}
	CHECK(length <= MODEM_SAHARA_CHUNK);
	memcpy(received + received_length, data, length);
	received_length += length;
	if (!more) {
		transfers_ended++;
	}
	last_cmd = *(const uint32_t *)data;
	return 0;
}

static int
sim_open(void *context, uint32_t id, uint64_t *size, void **image)
{
	(void)context;
	if (id == IMAGE_A) {
		*size = IMAGE_A_SIZE;
	} else if (id == IMAGE_B) {
		*size = IMAGE_B_SIZE;
	} else {
		return ENOENT;
	}
	*image = (void *)(uintptr_t)id;
	return 0;
}

static int
sim_read(void *context, void *image, uint64_t offset, void *buffer, size_t length)
{
	uint8_t *to = buffer;

	(void)context;
	for (size_t i = 0; i < length; i++) {
		to[i] = pattern((uint32_t)(uintptr_t)image, offset + i);
	}
	return 0;
}

static void
sim_say(void *context, const char *format, ...)
{
	va_list list;

	(void)context;
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
}

static const struct modem_sahara_ops sim_ops = { sim_send, sim_open, sim_read, sim_say };

static void
packet(uint32_t *p, uint32_t cmd, uint32_t length, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	memset(p, 0, 0x30);
	p[0] = cmd; p[1] = length; p[2] = a; p[3] = b; p[4] = c; p[5] = d;
}

static void
check_received(uint32_t image, uint64_t offset, size_t length)
{
	CHECK(received_length == length);
	for (size_t i = 0; i < length && failures < 10; i++) {
		if (received[i] != pattern(image, offset + i)) {
			printf("FAILED sahara: byte %zu of image %u\n", i, image);
			failures++;
		}
	}
	received_length = 0;
}

static void
test_sahara(void)
{
	static uint8_t chunk[MODEM_SAHARA_CHUNK];
	struct modem_sahara s;
	uint32_t p[12];

	modem_sahara_init(&s, &sim_ops, NULL, chunk);
	/* hello */
	packet(p, 1, 0x30, 2, 1, 1024, 0);
	CHECK(modem_sahara_input(&s, p, 0x30) == MODEM_SAHARA_MORE);
	CHECK(received_length == 0x30 && *(uint32_t *)received == 2 && ((uint32_t *)received)[2] == 2 && ((uint32_t *)received)[5] == 0);
	received_length = 0;
	/* the 32-bit ask, a header, then a big one in chained pieces: one transfer each */
	transfers_ended = 0;
	packet(p, 3, 0x14, IMAGE_A, 0, 4096, 0);
	CHECK(modem_sahara_input(&s, p, 0x14) == MODEM_SAHARA_MORE);
	check_received(IMAGE_A, 0, 4096);
	CHECK(transfers_ended == 1);
	packet(p, 3, 0x14, IMAGE_A, 1000, 250000, 0);
	CHECK(modem_sahara_input(&s, p, 0x14) == MODEM_SAHARA_MORE);
	check_received(IMAGE_A, 1000, 250000);
	CHECK(transfers_ended == 2);
	/* past its end */
	packet(p, 3, 0x14, IMAGE_A, IMAGE_A_SIZE - 10, 11, 0);
	CHECK(modem_sahara_input(&s, p, 0x14) == MODEM_SAHARA_FAILED);
	/* the 64-bit ask, another image */
	packet(p, 0x12, 0x20, IMAGE_B, 0, 900001, 0);
	p[6] = 1000000; p[7] = 0;
	CHECK(modem_sahara_input(&s, p, 0x20) == MODEM_SAHARA_MORE);
	check_received(IMAGE_B, 900001, 1000000);
	/* an image that is not there */
	packet(p, 3, 0x14, 99, 0, 100, 0);
	CHECK(modem_sahara_input(&s, p, 0x14) == MODEM_SAHARA_FAILED);
	/* the end of an image, DONE, more to come, a new hello, then all taken */
	packet(p, 4, 0x10, IMAGE_B, 0, 0, 0);
	CHECK(modem_sahara_input(&s, p, 0x10) == MODEM_SAHARA_MORE);
	CHECK(last_cmd == 5 && received_length == 8);
	received_length = 0;
	packet(p, 6, 0xc, 0, 0, 0, 0);
	CHECK(modem_sahara_input(&s, p, 0xc) == MODEM_SAHARA_MORE);
	packet(p, 1, 0x30, 2, 1, 1024, 0);
	CHECK(modem_sahara_input(&s, p, 0x30) == MODEM_SAHARA_MORE);
	received_length = 0;
	/* command mode, after its DDR's training: back to the images */
	packet(p, 1, 0x30, 2, 1, 1024, 3);
	CHECK(modem_sahara_input(&s, p, 0x30) == MODEM_SAHARA_MORE);
	CHECK(received_length == 0x30 && ((uint32_t *)received)[5] == 3);
	received_length = 0;
	packet(p, 0xb, 8, 0, 0, 0, 0);
	CHECK(modem_sahara_input(&s, p, 8) == MODEM_SAHARA_MORE);
	CHECK(received_length == 12 && last_cmd == 0xc && ((uint32_t *)received)[2] == 0);
	received_length = 0;
	packet(p, 6, 0xc, 1, 0, 0, 0);
	CHECK(modem_sahara_input(&s, p, 0xc) == MODEM_SAHARA_DONE);
	/* an image that the device says failed, a crash's hello, a short or lying packet, a failing send */
	packet(p, 4, 0x10, IMAGE_B, 7, 0, 0);
	CHECK(modem_sahara_input(&s, p, 0x10) == MODEM_SAHARA_FAILED);
	packet(p, 1, 0x30, 2, 1, 1024, 2);
	CHECK(modem_sahara_input(&s, p, 0x30) == MODEM_SAHARA_FAILED);
	CHECK(modem_sahara_input(&s, p, 4) == MODEM_SAHARA_FAILED);
	packet(p, 3, 0x14, IMAGE_A, 0, 16, 0);
	CHECK(modem_sahara_input(&s, p, 0x10) == MODEM_SAHARA_FAILED);
	packet(p, 0xee, 8, 0, 0, 0, 0);
	CHECK(modem_sahara_input(&s, p, 8) == MODEM_SAHARA_FAILED);
	fail_send_at = sends;
	packet(p, 3, 0x14, IMAGE_A, 0, 16, 0);
	CHECK(modem_sahara_input(&s, p, 0x14) == MODEM_SAHARA_FAILED);
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		test_fat(argv[1]);
	}
	test_sahara();
	printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	return failures != 0;
}
