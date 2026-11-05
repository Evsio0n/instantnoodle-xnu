/*
 * partdump: one of the phone's boot-chain images read out through the console, read-only (the owner, 2026-11-05: "dump
 * hyp放在我的桌面上"). The kernel's driver gives every GPT partition a raw device (/dev/rufs_NAME, docs/subsystems/
 * storage.md) and nothing else on the phone can reach the Mac without writing somewhere, so the bytes go the one way
 * that writes nothing: base64 lines in the run's log.
 *
 * It opens O_RDONLY, and takes only the names in `allowed`: nothing here can read any other partition, or write any.
 *
 *   partdump NAME --stats            sizes and hashes only
 *   partdump NAME                    those, then every byte up to the end of what the partition holds
 *   partdump NAME OFFSET LENGTH      those, then that range (bytes again, for lines a run lost)
 *
 *     partdump: NAME size N sha256 H
 *     partdump: NAME trimmed L sha256 H fill 00|ff; elf end E
 *     partdump: NAME D OFFSET CRC32 BASE64        (OFFSET and CRC32 in hex; 192 bytes a line)
 *     partdump: NAME end L bytes in K lines
 *
 * "Trimmed" is the partition without the run of zeros or of 0xFF that ends it, which is what an image written into a
 * larger partition is followed by. "elf end" is what the ELF's own program headers say its last byte is (0 if it is
 * not an ELF): the two agreeing, or the trimmed one being a little longer (a signature block), says nothing was cut.
 *
 * WHY IT IS PACED. The loader keeps the first 256 KB and the last 1 MB of a log, so a 3.6 MB image would lose its
 * middle. run_cam_probe.py's XL_FOLLOW_KEEP fetches the whole log every second or two and keeps each copy, so what
 * comes out at well under a megabyte per fetch is all in some copy: PACE_US per line is about 80 KB/s of log. Each line
 * is one write(), so another process's console output falls between lines and not inside one; a line that was still
 * damaged fails its CRC on the Mac, and a second run asks for that range. Tools: scratchpad partdump_join.py.
 *
 * Vendor firmware: what this prints belongs on the owner's desktop and in untracked out/, never in git or docs.
 */
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* <sys/disk.h>'s, which the iPhoneOS SDK leaves out */
#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)

#define BLOCK      4096u
#define CHUNK      (64u * 1024u)
#define LINE_BYTES 192u
#define PACE_US    3600u

static const char *const allowed[] = { "hyp_a", "hyp_b" };

static uint32_t
crc32_of(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xffffffffu;

	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

static size_t
base64_of(const uint8_t *p, size_t n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;

	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);

		out[o++] = t[v >> 18 & 63];
		out[o++] = t[v >> 12 & 63];
		out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? t[v & 63] : '=';
	}
	return o;
}

static void
hex_of(const uint8_t *p, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", p[i]);
	}
}

/* The last byte the ELF's program headers name, or 0 for anything that is not a little-endian ELF64 or ELF32. */
static uint64_t
elf_end(const uint8_t *m, uint64_t size)
{
	uint64_t end = 0;

	if (size < 64 || memcmp(m, "\177ELF", 4) != 0 || m[5] != 1) {
		return 0;
	}
	if (m[4] == 2) {
		uint64_t off = *(const uint64_t *)(const void *)(m + 32);
		uint16_t each = *(const uint16_t *)(const void *)(m + 54), n = *(const uint16_t *)(const void *)(m + 56);

		for (uint16_t i = 0; i < n && off + (uint64_t)(i + 1) * each <= size; i++) {
			const uint8_t *ph = m + off + (uint64_t)i * each;
			uint64_t e = *(const uint64_t *)(const void *)(ph + 8) + *(const uint64_t *)(const void *)(ph + 32);

			end = e > end ? e : end;
		}
	} else if (m[4] == 1) {
		uint32_t off = *(const uint32_t *)(const void *)(m + 28);
		uint16_t each = *(const uint16_t *)(const void *)(m + 42), n = *(const uint16_t *)(const void *)(m + 44);

		for (uint16_t i = 0; i < n && (uint64_t)off + (uint64_t)(i + 1) * each <= size; i++) {
			const uint8_t *ph = m + off + (uint64_t)i * each;
			uint64_t e = (uint64_t)*(const uint32_t *)(const void *)(ph + 4) + *(const uint32_t *)(const void *)(ph + 16);

			end = e > end ? e : end;
		}
	}
	return end;
}

int
main(int argc, char **argv)
{
	char path[64], digest[CC_SHA256_DIGEST_LENGTH * 2 + 1], line[64 + LINE_BYTES * 2];
	uint8_t hash[CC_SHA256_DIGEST_LENGTH];
	const char *name = argc > 1 ? argv[1] : "";
	uint32_t block = 0;
	uint64_t blocks = 0, size, done = 0, trimmed, from = 0, length = 0, lines = 0;
	bool known = false, stats = argc > 2 && strcmp(argv[2], "--stats") == 0;
	uint8_t *m;
	int fd;

	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		known = known || strcmp(name, allowed[i]) == 0;
	}
	if (!known) {
		printf("partdump: only hyp_a and hyp_b, not '%s'\n", name);
		return 2;
	}
	snprintf(path, sizeof(path), "/dev/rufs_%s", name);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	if (ioctl(fd, DKIOCGETBLOCKSIZE, &block) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) != 0 || block == 0) {
		block = BLOCK;
		blocks = 8192u * 1024u / BLOCK;  /* (the kernel's own line: 8192 KB) */
		printf("partdump: %s: the device does not say its size; taking %llu blocks of %u\n", name,
		    (unsigned long long)blocks, block);
	}
	size = blocks * block;
	if (posix_memalign((void **)&m, BLOCK, size) != 0) {
		printf("partdump: no memory for %llu bytes\n", (unsigned long long)size);
		return 1;
	}
	while (done < size) {
		size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
		ssize_t got = pread(fd, m + done, want, (off_t)done);

		if (got <= 0) {
			printf("partdump: %s: read at %llu gave %zd\n", name, (unsigned long long)done, got);
			break;
		}
		done += (uint64_t)got;
	}
	close(fd);
	size = done;

	CC_SHA256(m, (CC_LONG)size, hash);
	hex_of(hash, sizeof(hash), digest);
	printf("partdump: %s size %llu sha256 %s\n", name, (unsigned long long)size, digest);

	uint8_t fill = size ? m[size - 1] : 0;

	trimmed = size;
	if (fill == 0x00 || fill == 0xff) {
		while (trimmed > 0 && m[trimmed - 1] == fill) {
			trimmed--;
		}
	}
	CC_SHA256(m, (CC_LONG)trimmed, hash);
	hex_of(hash, sizeof(hash), digest);
	printf("partdump: %s trimmed %llu sha256 %s fill %02x; elf end %llu\n", name, (unsigned long long)trimmed, digest,
	    fill, (unsigned long long)elf_end(m, size));
	fflush(stdout);
	if (stats) {
		return 0;
	}

	length = trimmed;
	if (argc > 3) {
		from = strtoull(argv[2], NULL, 0);
		length = strtoull(argv[3], NULL, 0);
		from -= from % LINE_BYTES;
		if (from > size) {
			from = size;
		}
		if (length > size - from) {
			length = size - from;
		}
	}
	for (uint64_t at = from; at < from + length; at += LINE_BYTES) {
		size_t n = from + length - at < LINE_BYTES ? (size_t)(from + length - at) : LINE_BYTES;
		int head = snprintf(line, sizeof(line), "partdump: %s D %llx %08x ", name, (unsigned long long)at, crc32_of(m + at, n));
		size_t all = (size_t)head + base64_of(m + at, n, line + head);

		line[all++] = '\n';
		if (write(STDOUT_FILENO, line, all) != (ssize_t)all) {
			perror("write");
		}
		lines++;
		usleep(PACE_US);
	}
	printf("partdump: %s end %llu bytes in %llu lines\n", name, (unsigned long long)length, (unsigned long long)lines);
	return 0;
}
