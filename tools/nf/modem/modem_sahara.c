/* See modem_sahara.h. */
#include "modem_sahara.h"

#ifdef KERNEL
#include <sys/systm.h>
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define CMD_HELLO               0x1
#define CMD_HELLO_RESP          0x2
#define CMD_READ_DATA           0x3
#define CMD_END_OF_IMAGE        0x4
#define CMD_DONE                0x5
#define CMD_DONE_RESP           0x6
#define CMD_RESET               0x7
#define CMD_RESET_RESP          0x8
#define CMD_MEM_DEBUG           0x9
#define CMD_READY               0xb
#define CMD_SWITCH_MODE         0xc
#define CMD_MEM_DEBUG64         0x10
#define CMD_READ_DATA64         0x12
#define VERSION                 2
#define MODE_IMAGE_TX_PENDING   0
#define MODE_IMAGE_TX_COMPLETE  1
#define MODE_MEMORY_DEBUG       2
#define HELLO_LENGTH            0x30

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void
modem_sahara_init(struct modem_sahara *s, const struct modem_sahara_ops *ops, void *context, uint8_t *chunk)
{
	memset(s, 0, sizeof(*s));
	s->ops = ops;
	s->context = context;
	s->chunk = chunk;
}

static void
count(struct modem_sahara *s, uint32_t id, uint64_t bytes)
{
	for (unsigned i = 0; i < s->images; i++) {
		if (s->ids[i] == id) {
			s->sent[i] += bytes;
			return;
		}
	}
	if (s->images < MODEM_SAHARA_IMAGES) {
		s->ids[s->images] = id;
		s->sent[s->images++] = bytes;
	}
}

static enum modem_sahara_result
read_data(struct modem_sahara *s, uint32_t id, uint64_t offset, uint64_t length)
{
	int error;

	if (!s->have_current || s->current_id != id) {
		if ((error = s->ops->image_open(s->context, id, &s->current_size, &s->current)) != 0) {
			/* (the one thing the first boots are to learn: which id is which file) */
			s->ops->say(s->context, "sahara: IMAGE %u IS ASKED FOR AND THERE IS NO SUCH IMAGE (error %d): offset %llu, %llu bytes\n", id, error,
			    (unsigned long long)offset, (unsigned long long)length);
			return MODEM_SAHARA_FAILED;
		}
		s->have_current = true;
		s->current_id = id;
		s->ops->say(s->context, "sahara: image %u: %llu bytes\n", id, (unsigned long long)s->current_size);
	}
	if (length == 0 || offset > s->current_size || length > s->current_size - offset) {
		s->ops->say(s->context, "sahara: image %u: asked for %llu bytes at %llu of %llu\n", id, (unsigned long long)length, (unsigned long long)offset,
		    (unsigned long long)s->current_size);
		return MODEM_SAHARA_FAILED;
	}
	while (length > 0) {
		size_t take = length < MODEM_SAHARA_CHUNK ? (size_t)length : MODEM_SAHARA_CHUNK;

		if ((error = s->ops->image_read(s->context, s->current, offset, s->chunk, take)) != 0) {
			s->ops->say(s->context, "sahara: image %u: a read at %llu failed: %d\n", id, (unsigned long long)offset, error);
			return MODEM_SAHARA_FAILED;
		}
		if ((error = s->ops->send(s->context, s->chunk, take, length > take)) != 0) {
			s->ops->say(s->context, "sahara: image %u: a send failed: %d\n", id, error);
			return MODEM_SAHARA_FAILED;
		}
		offset += take;
		length -= take;
		s->total += take;
		count(s, id, take);
	}
	return MODEM_SAHARA_MORE;
}

enum modem_sahara_result
modem_sahara_input(struct modem_sahara *s, const void *packet, size_t length)
{
	const uint8_t *p = packet;
	uint8_t out[HELLO_LENGTH];
	uint32_t command, declared;
	int error;

	if (length < 8) {
		s->ops->say(s->context, "sahara: a packet of %zu bytes\n", length);
		return MODEM_SAHARA_FAILED;
	}
	command = get32(p);
	declared = get32(p + 4);
	if (declared > length) {
		s->ops->say(s->context, "sahara: packet %#x says %u bytes, and has %zu\n", command, declared, length);
		return MODEM_SAHARA_FAILED;
	}
	switch (command) {
	case CMD_HELLO:
		if (declared < 0x18) {
			return MODEM_SAHARA_FAILED;
		}
		s->ops->say(s->context, "sahara: hello: version %u, compatible with %u, packets of up to %u bytes, mode %u\n", get32(p + 8), get32(p + 12), get32(p + 16),
		    get32(p + 20));
		if (get32(p + 20) == MODE_MEMORY_DEBUG) {
			s->ops->say(s->context, "sahara: the device has crashed and offers its memory: not taken\n");
			return MODEM_SAHARA_FAILED;
		}
		memset(out, 0, sizeof(out));
		put32(out, CMD_HELLO_RESP);
		put32(out + 4, HELLO_LENGTH);
		put32(out + 8, VERSION);
		put32(out + 12, VERSION);
		put32(out + 16, 0);
		put32(out + 20, get32(p + 20));
		error = s->ops->send(s->context, out, HELLO_LENGTH, false);
		return error == 0 ? MODEM_SAHARA_MORE : MODEM_SAHARA_FAILED;
	case CMD_READ_DATA:
		if (declared < 0x14) {
			return MODEM_SAHARA_FAILED;
		}
		return read_data(s, get32(p + 8), get32(p + 12), get32(p + 16));
	case CMD_READ_DATA64:
		if (declared < 0x20) {
			return MODEM_SAHARA_FAILED;
		}
		return read_data(s, (uint32_t)get64(p + 8), get64(p + 16), get64(p + 24));
	case CMD_END_OF_IMAGE:
		if (declared < 0x10) {
			return MODEM_SAHARA_FAILED;
		}
		s->ops->say(s->context, "sahara: image %u ended, status %u\n", get32(p + 8), get32(p + 12));
		if (get32(p + 12) != 0) {
			return MODEM_SAHARA_FAILED;
		}
		memset(out, 0, 8);
		put32(out, CMD_DONE);
		put32(out + 4, 8);
		error = s->ops->send(s->context, out, 8, false);
		return error == 0 ? MODEM_SAHARA_MORE : MODEM_SAHARA_FAILED;
	case CMD_DONE_RESP:
		if (declared < 0xc) {
			return MODEM_SAHARA_FAILED;
		}
		s->ops->say(s->context, "sahara: done, image transfer status %u (%s)\n", get32(p + 8), get32(p + 8) == MODE_IMAGE_TX_COMPLETE ? "all taken" : "more to come");
		s->have_current = false;
		return get32(p + 8) == MODE_IMAGE_TX_COMPLETE ? MODEM_SAHARA_DONE : MODEM_SAHARA_MORE;
	case CMD_RESET_RESP:
		s->ops->say(s->context, "sahara: reset answered\n");
		return MODEM_SAHARA_MORE;
	case CMD_READY:
		/*
		 * It has trained its DDR and offers the numbers (Linux's Documentation/sahara/sahara_protocol.rst, 2026). Android keeps them so that the next boot need not train; nothing of
		 * the phone's storage is written here, so they are not asked for: on with the images.
		 */
		s->ops->say(s->context, "sahara: command mode: not asked for its training data, on with the images\n");
		memset(out, 0, 12);
		put32(out, CMD_SWITCH_MODE);
		put32(out + 4, 12);
		put32(out + 8, MODE_IMAGE_TX_PENDING);
		error = s->ops->send(s->context, out, 12, false);
		return error == 0 ? MODEM_SAHARA_MORE : MODEM_SAHARA_FAILED;
	default:
		s->ops->say(s->context, "sahara: packet %#x, %u bytes: not one this knows\n", command, declared);
		return MODEM_SAHARA_FAILED;
	}
}
