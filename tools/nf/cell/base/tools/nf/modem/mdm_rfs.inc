/* One send callback consumes its packet before returning. No QRTR/MHI lock is
 * held across a FAT read. Protocol state is per source and per transfer port. */
#include "mdm_rfs.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <libkern/libkern.h>
#else
#include <errno.h>
#include <string.h>
#endif

enum { RFS_PATH_DENIED = 256 };
enum { RFS_RRQ = 1, RFS_WRQ, RFS_DATA, RFS_ACK, RFS_ERROR, RFS_OACK };
enum { RFS_BSIZE, RFS_TSIZE, RFS_SEEK, RFS_RSIZE, RFS_WSIZE, RFS_TIMEOUT, RFS_TIMEOUTMS, RFS_WINDOWSIZE, RFS_UNLINK, RFS_APPEND, RFS_OPTIONS };
static const char *const rfs_names[RFS_OPTIONS] = {
	"blksize", "tsize", "seek", "rsize", "wsize", "timeout", "timeoutms", "windowsize", "unlink", "append"
};

static unsigned
rfs_be16(const uint8_t *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static bool
rfs_same(const char *a, const char *b)
{
	while (*a && *b) {
		char x = *a++, y = *b++;
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y) return false;
	}
	return *a == *b;
}

/* The wire strings are borrowed only after finding a terminating NUL. */
static const char *
rfs_token(const uint8_t **p, const uint8_t *end)
{
	const uint8_t *start = *p;
	const uint8_t *q = start;
	while (q < end && *q) q++;
	if (q == end || q == start) return NULL;
	*p = q + 1;
	return (const char *)start;
}

static bool
rfs_decimal(const char *s, uint32_t *value)
{
	uint32_t n = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') return false;
		unsigned digit = (unsigned)(*s - '0');
		if (n > (0xffffffffu - digit) / 10u) return false;
		n = n * 10u + digit;
	}
	*value = n;
	return true;
}

/* Names in the handset's read-only SDX55M distribution. KEFS, DDR
 * training and files outside the approved FAT volume are never exposed. */
static bool
rfs_static_firmware(const char *name)
{
	static const char *const files[] = {
		"sbl1.mbn", "xbl_cfg.elf", "aop.mbn", "tz.mbn", "hyp.mbn", "devcfg.mbn",
		"apdp.mbn", "sec.elf", "multi_image.mbn", "multi_image_qti.mbn",
		"apps.mbn", "qdsp6sw.mbn", "acdb.mbn"
	};
	if (strncmp(name, "sdx55m/", sizeof("sdx55m/") - 1)) return false;
	name += sizeof("sdx55m/") - 1;
	for (unsigned i = 0; i < sizeof(files)/sizeof(files[0]); i++)
		if (rfs_same(name, files[i])) return true;
	return false;
}

static bool
rfs_firmware_path(const char *name, char out[MDM_RFS_PATH])
{
	/* Both spellings are relative to the instance root on Android. Only this
	 * FAT-backed subtree is mapped. A catalogue can contain many carriers. */
	static const char prefix[] = "readonly/firmware/image/";
	if (*name == '/') name++;
	if (strncmp(name, prefix, sizeof(prefix) - 1)) return false;
	name += sizeof(prefix) - 1;
	size_t len = strlen(name);
	if (!len || len + sizeof("image/") > MDM_RFS_PATH) return false;
	const char *part = name;
	for (const char *p = name;; p++) {
		char c = *p;
		if (c == '/' || !c) {
			if (p == part || *part == '.' || p[-1] == '.' || p - part > 255) return false;
			if (!c) break;
			part = p + 1;
		} else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
	}
	/* Authorize the shipped MCFG catalogue and its public index. Other image
	 * trees also contain device-specific EFS or DDR files. Default-deny them. */
	if (strncmp(name, "modem_pr/mcfg/", sizeof("modem_pr/mcfg/") - 1) &&
	    strcmp(name, "modem_pr/mbn_ota.txt") && !rfs_static_firmware(name)) return false;
	memcpy(out, "image/", sizeof("image/") - 1);
	memcpy(out + sizeof("image/") - 1, name, len + 1);
	return true;
}

/* TFTP error codes. Unsupported optional extensions are omitted from OACK;
 * options that would change data or the interpretation of its offset are rejected. */
static unsigned
rfs_options(const uint8_t *p, const uint8_t *end, struct mdm_rfs_request *q, bool sink, bool allow_unlink, bool scratch)
{
	q->block = 512;
	q->window = 1;
	q->timeout_ms = 1000;
	while (p < end) {
		const char *name = rfs_token(&p, end);
		const char *arg = rfs_token(&p, end);
		if (!name || !arg) return 8;
		if ((rfs_same(name, "append") && !(sink && scratch)) || (rfs_same(name, "unlink") && !allow_unlink)) return 2;
		unsigned opt = 0;
		while (opt < RFS_OPTIONS && !rfs_same(name, rfs_names[opt])) opt++;
		if (sink && opt != RFS_BSIZE && opt != RFS_WSIZE && opt != RFS_WINDOWSIZE &&
		    opt != RFS_TIMEOUT && opt != RFS_TIMEOUTMS && opt != RFS_UNLINK &&
		    !(scratch && (opt == RFS_APPEND || opt == RFS_SEEK || opt == RFS_TSIZE))) return 2;
		if (opt == RFS_OPTIONS) continue;
		uint32_t n;
		if ((q->options & (1u << opt)) || !rfs_decimal(arg, &n)) return 8;
		q->options |= 1u << opt;
		switch (opt) {
		case RFS_BSIZE:
			if (n < 8 || n > 65464) return 8;
			q->block = n < MDM_RFS_BLOCK ? n : MDM_RFS_BLOCK;
			break;
		case RFS_WSIZE:
		case RFS_WINDOWSIZE:
			if (!n || n > 65535 || ((q->options & (1u << RFS_WSIZE)) &&
			    (q->options & (1u << RFS_WINDOWSIZE)))) return 8;
			q->window = n < MDM_RFS_WINDOW ? n : MDM_RFS_WINDOW;
			break;
		case RFS_UNLINK:
			if (n > 1) return 8;
			/* Native option ID 5 records zero in its successful OACK. */
			q->unlink_value = 0;
			break;
		case RFS_APPEND: if (n > 1) return 8; break;
		case RFS_SEEK: q->seek = n; break;
		case RFS_RSIZE: if (!n) return 8; q->rsize = n; break;
		case RFS_TSIZE:
			if ((!sink && n != 0) || (sink && n > MDM_RFS_MARKER_MAX)) return 8;
			q->write_size = n;
			break;
		case RFS_TIMEOUT:
			if (!n || n > 255 || (q->options & (1u << RFS_TIMEOUTMS))) return 8;
			q->timeout_ms = n * 1000;
			break;
		case RFS_TIMEOUTMS:
			if (!n || n > 255000 || (q->options & (1u << RFS_TIMEOUT))) return 8;
			q->timeout_ms = n;
			break;
		}
	}
	return 0;
}

static int
rfs_error(struct mdm_rfs *r, uint32_t local, uint32_t node, uint32_t peer, unsigned code)
{
	static const char *const messages[] = {
		"Unavailable", "File not found", "Access denied", "Busy", "Illegal operation",
		"Unknown transfer", "File exists", "Unknown user", "Invalid options"
	};
	size_t n = strlen(messages[code]) + 1;
	r->wire[0] = 0; r->wire[1] = RFS_ERROR; r->wire[2] = 0; r->wire[3] = (uint8_t)code;
	memcpy(r->wire + 4, messages[code], n);
	int rc = r->ops->send(r->context, local, node, peer, r->wire, 4 + n);
	if (r->reads.observing) {
		r->reads.last_response = RFS_ERROR; r->reads.last_error = code;
		r->reads.last_submitted = !rc;
	}
	return rc;
}

static bool
rfs_under(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);
	return !strncmp(path, prefix, n) && path[n] == '/';
}

static enum mdm_rfs_root
rfs_wrq_root(const char *path)
{
	if (!path || !*path || strlen(path) >= MDM_RFS_PATH) return MDM_RFS_INVALID;
	const char *part = path;
	for (const char *p = path; ; p++) {
		if (!*p || *p == '/') {
			size_t n = (size_t)(p - part);
			if (!n || n > 255 || (n == 1 && *part == '.') ||
			    (n == 2 && part[0] == '.' && part[1] == '.')) return MDM_RFS_INVALID;
			if (!*p) break;
			part = p + 1;
		} else if ((unsigned char)*p < 32 || (unsigned char)*p > 126 || *p == '\\') {
			return MDM_RFS_INVALID;
		}
	}
	if (rfs_under(path, "readwrite")) return MDM_RFS_PERSIST;
	if (rfs_under(path, "shared")) return MDM_RFS_SHARED;
	if (rfs_under(path, "hlos")) return MDM_RFS_HLOS;
	if (rfs_under(path, "readonly/firmware/image/modem_pr/mcfg")) return MDM_RFS_MCFG;
	if (rfs_under(path, "readonly/firmware")) return MDM_RFS_FIRMWARE;
	if (rfs_under(path, "readonly/vendor/firmware")) return MDM_RFS_VENDOR_FIRMWARE;
	if (rfs_under(path, "ramdumps")) return MDM_RFS_RAMDUMP;
	return MDM_RFS_OTHER;
}

static unsigned
rfs_wrq_option(const char *name)
{
	static const unsigned bits[RFS_OPTIONS] = {
		MDM_RFS_OPT_BLOCK, MDM_RFS_OPT_TSIZE, MDM_RFS_OPT_SEEK, MDM_RFS_OPT_RSIZE,
		MDM_RFS_OPT_WINDOW, MDM_RFS_OPT_TIMEOUT, MDM_RFS_OPT_TIMEOUT, MDM_RFS_OPT_WINDOW, MDM_RFS_OPT_UNLINK, MDM_RFS_OPT_APPEND
	};
	for (unsigned i = 0; i < RFS_OPTIONS; i++)
		if (rfs_same(name, rfs_names[i])) return bits[i];
	if (rfs_same(name, "append")) return MDM_RFS_OPT_APPEND;
	if (rfs_same(name, "unlink")) return MDM_RFS_OPT_UNLINK;
	return MDM_RFS_OPT_UNKNOWN;
}

static unsigned
rfs_read_header(struct mdm_rfs *r, uint32_t node, const uint8_t *p, const uint8_t *end,
    struct mdm_rfs_request *q, const char **scratch_key)
{
	struct mdm_rfs_reads *obs = &r->reads;
	const char *path = rfs_token(&p, end);
	const char *rel = path && *path == '/' ? path + 1 : path;
	const char *mode = rfs_token(&p, end);
	enum mdm_rfs_root root = rfs_wrq_root(rel);
	bool marker = root == MDM_RFS_PERSIST && !strcmp(rel, "readwrite/server_check.txt");
	unsigned kind;
	switch (root) {
	case MDM_RFS_INVALID: kind = RFS_READ_INVALID; break;
	case MDM_RFS_PERSIST: kind = marker ? RFS_READ_MARKER : RFS_READ_PERSIST; break;
	case MDM_RFS_SHARED: kind = RFS_READ_SHARED; break;
	case MDM_RFS_HLOS: kind = RFS_READ_HLOS; break;
	case MDM_RFS_VENDOR_FIRMWARE: kind = RFS_READ_VENDOR; break;
	case MDM_RFS_FIRMWARE:
		kind = rfs_under(rel, "readonly/firmware/image") ? RFS_READ_IMAGE : RFS_READ_FIRMWARE_OTHER;
		break;
	case MDM_RFS_MCFG: kind = RFS_READ_MCFG; break;
	default: kind = RFS_READ_OTHER; break;
	}
	obs->roots[kind]++;
	obs->last_root = kind;
	obs->last_options = 0;
	obs->last_valid = root != MDM_RFS_INVALID && mode && rfs_same(mode, "octet");
	const uint8_t *opts = p;
	while (mode && p < end) {
		const char *name = rfs_token(&p, end), *value = rfs_token(&p, end);
		if (!name || !value) { obs->last_valid = false; break; }
		obs->last_options |= rfs_wrq_option(name);
	}
	memset(q, 0, sizeof(*q));
	bool scratch = !marker && root == MDM_RFS_PERSIST && node == 3 && r->scratch_enabled;
	*scratch_key = scratch ? rel : NULL;
	obs->last_allowed = marker ? (r->marker_file_enabled && node == 3) :
	    (scratch || (path && rfs_firmware_path(path, q->path)));
	if (!path || !mode || !rfs_same(mode, "octet")) return 4;
	if (!obs->last_allowed) return RFS_PATH_DENIED;
	if (!obs->last_valid) return 8;
	return rfs_options(opts, end, q, false, false, false);
}

/* All callers run on the board's current MPSS owner epoch. Nested names use
 * implicit directories. The key lives only in RAM while the object exists
 * or an in-flight transfer still owns the slot. */
static struct mdm_rfs_marker *
rfs_scratch_file(struct mdm_rfs *r, const char *key, bool create, unsigned *error)
{
	struct mdm_rfs_scratch_entry *empty = NULL;
	*error = 1;
	if (rfs_under(key, "readwrite/server_check.txt")) { *error = 2; return NULL; }
	for (unsigned i=0; i<MDM_RFS_SCRATCH_FILES; i++) {
		struct mdm_rfs_scratch_entry *e = &r->scratch.entries[i];
		if (!e->key[0]) { if (!empty) empty = e; continue; }
		if (!strcmp(e->key, key)) return &e->file;
		if (rfs_under(e->key, key) || rfs_under(key, e->key)) { *error = 2; return NULL; }
	}
	if (!create) return NULL;
	if (!empty) { r->scratch.full++; *error = 3; return NULL; }
	memcpy(empty->key, key, strlen(key) + 1);
	r->scratch.cleared = false;
	return &empty->file;
}

static struct mdm_rfs_sink_stats *
rfs_write_stats(struct mdm_rfs *r, const struct mdm_rfs_session *s)
{
	return s->scratch_file ? &r->scratch.sink : &r->sink;
}

static void
rfs_release_scratch(struct mdm_rfs *r, struct mdm_rfs_marker *file)
{
	if (!file || file->present) return;
	for (unsigned i=0; i<MDM_RFS_SESSIONS; i++)
		if (r->sessions[i].local && r->sessions[i].ram_file == file) return;
	for (unsigned i=0; i<MDM_RFS_SCRATCH_FILES; i++) {
		if (&r->scratch.entries[i].file == file) {
			memset(&r->scratch.entries[i], 0, sizeof(r->scratch.entries[i]));
			break;
		}
	}
}
static int rfs_sink_open(struct mdm_rfs *, uint32_t, uint32_t, const uint8_t *, const uint8_t *, const char *, uint64_t);

static void
rfs_wrq(struct mdm_rfs *r, uint32_t node, uint32_t peer, const uint8_t *packet,
    size_t length, uint64_t now)
{
	/* Inspect the bounded request header in place. Only the fixed public
	 * check can allocate a TID. No WRQ ever opens FAT or retains its path. */
	const size_t scan = length > 1024 ? 1024 : length;
	const uint8_t *p = packet + 2, *end = packet + scan;
	const char *path = rfs_token(&p, end);
	if (path && *path == '/') path++;
	const enum mdm_rfs_root root = rfs_wrq_root(path);
	const bool marker = root == MDM_RFS_PERSIST && !strcmp(path, "readwrite/server_check.txt");
	const char *mode = rfs_token(&p, end);
	const uint8_t *option_start = p;
	bool valid = length == scan && root != MDM_RFS_INVALID && mode && rfs_same(mode, "octet");
	unsigned opts = 0;
	if (mode) {
		while (p < end) {
			const char *name = rfs_token(&p, end);
			const char *arg = rfs_token(&p, end);
			if (!name || !arg) { valid = false; break; }
			opts |= rfs_wrq_option(name);
		}
	}
	struct mdm_rfs_wrq_stats *w = &r->writes;
	bool previous = r->stats.wrq != 0;
	w->last_same_peer = previous && w->last_node == node && w->last_peer == peer;
	w->last_same_shape = w->last_same_peer && valid && w->last_valid &&
	    root == w->last_root && opts == w->last_options && marker == w->last_public_server_check;
	w->same_peer_adjacent += w->last_same_peer;
	w->same_shape_adjacent += w->last_same_shape;
	if (!previous) w->first_ms = now;
	w->delta_ms = previous && now >= w->last_ms ? now - w->last_ms : 0;
	w->last_ms = now;
	w->last_node = node; w->last_peer = peer;
	w->last_root = root; w->last_options = opts; w->last_valid = valid;
	w->last_public_server_check = marker;
	w->public_server_check += marker;
	w->options_seen |= opts;
	w->roots[root]++;
	r->stats.wrq++;
	w->last_submitted = false;
	w->last_sink_accepted = w->last_scratch_accepted = false;
	unsigned code = 2;
	bool scratch = !marker && root == MDM_RFS_PERSIST && r->scratch_enabled;
	if (node == 3 && valid && ((marker && r->check_enabled) || scratch)) {
		/* Negative success sentinel leaves all actual TFTP error codes,
		 * including generic error zero, representable. */
		int result = rfs_sink_open(r, node, peer, option_start, end, scratch ? path : NULL, now);
		if (result < 0) {
			w->last_sink_accepted = marker;
			w->last_scratch_accepted = scratch;
			return;
		}
		code = (unsigned)result;
	}
	r->stats.denied++;
	w->last_submitted = rfs_error(r, r->listener, node, peer, code) == 0;
	if (w->last_submitted) w->submitted++; else w->submit_failed++;
}

static void
rfs_close(struct mdm_rfs *r, struct mdm_rfs_session *s)
{
	uint32_t local = s->local;
	if (!local) return;
	struct mdm_rfs_marker *file = s->scratch_file ? s->ram_file : NULL;
	if (!s->is_sink && !s->marker_read && !s->scratch_file) modem_fat_close(r->fat, &s->file);
	memset(s, 0, sizeof(*s));
	rfs_release_scratch(r, file);
	r->ops->close_port(r->context, local);
}

static void
mdm_rfs_drop_peer(struct mdm_rfs *r, uint32_t node, uint32_t peer)
{
	if (node == r->writes.last_node && (!peer || peer == r->writes.last_peer)) {
		r->writes.last_node = 0;
		r->writes.last_peer = 0;
	}
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (s->local && s->node == node && (!peer || s->peer == peer)) {
			if (s->is_sink) rfs_write_stats(r,s)->revoked += !s->sink_done;
			else r->stats.revoked++;
			rfs_close(r, s);
		}
	}
	if (node == 3 && !peer) {
		memset(r->scratch.entries, 0, sizeof(r->scratch.entries));
		r->scratch.cleared = true;
	}
}

static void
mdm_rfs_stop(struct mdm_rfs *r)
{
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (s->is_sink) rfs_write_stats(r,s)->revoked += !s->sink_done;
		else if (s->local) r->stats.revoked++;
		rfs_close(r, s);
	}
	memset(r->scratch.entries, 0, sizeof(r->scratch.entries));
	r->scratch.cleared = true;
	memset(r->marker.bytes, 0, sizeof(r->marker.bytes));
	memset(r->wire, 0, sizeof(r->wire));
	r->marker.size = 0; r->marker.present = false; r->marker.cleared = true;
	r->listener = 0;
}

static void
mdm_rfs_start(struct mdm_rfs *r, struct modem_fat *fat, const struct mdm_rfs_ops *ops,
    void *context, uint32_t listener)
{
	/* The first instance is zero-initialized, and reset always frees old TIDs
	 * using their original ops before installing the new transport. */
	mdm_rfs_stop(r);
	memset(r, 0, sizeof(*r));
	r->fat = fat; r->ops = ops; r->context = context; r->listener = listener;
}

static size_t
rfs_number(uint8_t *dst, uint64_t value)
{
	uint8_t digits[20];
	size_t used = 0;
	do { digits[used++] = (uint8_t)('0' + value % 10u); value /= 10u; } while (value);
	for (size_t i = 0; i < used; i++) dst[i] = digits[used - 1 - i];
	dst[used] = 0;
	return used + 1;
}

static size_t
rfs_oack(struct mdm_rfs *r, const struct mdm_rfs_session *s)
{
	const struct mdm_rfs_request *q = &s->req;
	const uint64_t values[RFS_OPTIONS] = {q->block, s->is_sink ? q->write_size : s->file.size, q->seek, q->rsize,
	    q->window, q->timeout_ms / 1000u, q->timeout_ms, q->window, q->unlink_value, 0};
	r->wire[0] = 0; r->wire[1] = RFS_OACK;
	size_t length = 2;
	for (unsigned i = 0; i < RFS_OPTIONS; i++) {
		if (!(q->options & (1u << i))) continue;
		size_t n = strlen(rfs_names[i]) + 1;
		memcpy(r->wire + length, rfs_names[i], n);
		length += n;
		length += rfs_number(r->wire + length, values[i]);
	}
	return length;
}

/* The IN2010 vendor routes this named check to a discard device when its
 * regular file is absent. Reproduce the wire handshake, with no backing
 * file, no retained payload and a 64 KiB lifetime byte budget per TID. */
#define RFS_CHECK_LIMIT (64u * 1024u)

static void
rfs_sink_reply(struct mdm_rfs *r, struct mdm_rfs_session *s, uint64_t now)
{
	struct mdm_rfs_marker *file = s->ram_file;
	struct mdm_rfs_sink_stats *stats = rfs_write_stats(r,s);
	size_t length = 4;
	bool options = !s->sink_block && s->req.options;
	if (s->unlink_request && !s->sink_done &&
	    (!file || !file->present || file->version != s->marker_version)) {
		rfs_error(r, s->local, s->node, s->peer, 2);
		rfs_close(r, s); return;
	}
	if (options) length = rfs_oack(r, s);
	else {
		r->wire[0] = 0; r->wire[1] = RFS_ACK;
		r->wire[2] = (uint8_t)(s->sink_block >> 8); r->wire[3] = (uint8_t)s->sink_block;
	}
	int rc = r->ops->send(r->context, s->local, s->node, s->peer, r->wire, length);
	if (rc) {
		if (rc == EAGAIN || rc == EBUSY || rc == ENOBUFS) {
			stats->ack_busy++;
			if (!s->sink_done) s->due = now + 25;
		} else {
			stats->send_failed++;
			rfs_close(r, s);
		}
		return;
	}
	s->sink_ack_sent = true;
	if (s->unlink_request) {
		file->unlink_oack++;
		if (!s->sink_done) {
			memset(file->bytes, 0, sizeof(file->bytes));
			file->size = 0; file->present = false;
			file->unlinks++; file->version++;
			if (s->scratch_file) r->scratch.unlinks++;
			s->sink_done = true;
			s->due = now + 2000;
		}
		return;
	}
	if (options) stats->oack++;
	else if (!s->sink_block) stats->ack0++;
	else stats->ack_submitted++;
	if (s->sink_done) return;
	if (s->sink_final) {
		s->sink_done = true;
		stats->completed++;
		stats->last_completed_ms = now;
		if (file) {
			memset(file->bytes, 0, sizeof(file->bytes));
			memcpy(file->bytes, s->marker_snapshot, s->write_size);
			file->present = true; file->size = s->write_size;
			file->last_commit_size = s->write_size;
			file->commits++; file->version++;
			if (s->scratch_file) r->scratch.commits++;
			memset(s->marker_snapshot, 0, sizeof(s->marker_snapshot));
		}
		/* Dally so a lost final ACK can be sent again to this exact TID. */
		uint32_t dally = 2 * s->req.timeout_ms;
		s->due = now + (dally < 1000 ? 1000 : dally);
	} else {
		s->due = now + s->req.timeout_ms;
	}
}

static int
rfs_sink_open(struct mdm_rfs *r, uint32_t node, uint32_t peer, const uint8_t *opts,
    const uint8_t *end, const char *key, uint64_t now)
{
	struct mdm_rfs_request q = {0};
	bool scratch = key != NULL;
	unsigned err = rfs_options(opts, end, &q, true, r->marker_file_enabled || scratch, scratch);
	if (err) return scratch ? (int)err : 2;
	bool unlink = (q.options & (1u << RFS_UNLINK)) != 0;
	bool seek = (q.options & (1u << RFS_SEEK)) != 0;
	bool append = (q.options & (1u << RFS_APPEND)) != 0;
	if ((seek && append) || (unlink && (seek || append))) return 8;
	struct mdm_rfs_marker *file = r->marker_file_enabled ? &r->marker : NULL;
	if (scratch) {
		file = rfs_scratch_file(r, key, false, &err);
		if (!file && (unlink || seek)) {
			if (unlink && err == 1) r->scratch.unlink_missing++;
			return (int)err;
		}
		if (!file && err != 1) return (int)err;
	}
	struct mdm_rfs_session *free_slot = NULL;
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (!s->local) { if (!free_slot) free_slot = s; continue; }
		bool same_peer = s->node == node && s->peer == peer;
		if (s->sink_done) {
			if (same_peer && file && s->ram_file == file && unlink &&
			    s->unlink_request && !memcmp(&q, &s->req, sizeof(q))) {
				rfs_sink_reply(r, s, now); return -1;
			}
			continue;
		}
		if (!same_peer) {
			/* Reject a second writer to an object still being staged. A
			 * reader using its own immutable snapshot never blocks it. */
			if (scratch && file && s->is_sink && s->ram_file == file) return 0;
			continue;
		}
		if (!s->is_sink || s->ram_file != file || s->sink_block || memcmp(&q,&s->req,sizeof(q))) return 0;
		rfs_sink_reply(r, s, now); return -1;
	}
	if (unlink && (!file || !file->present)) {
		if (scratch) r->scratch.unlink_missing++; else r->marker.unlink_missing++;
		return scratch ? 1 : 2;
	}
	if (seek && (!file || !file->present)) return 1;
	uint32_t at = append && file ? file->size : q.seek;
	if (at > MDM_RFS_MARKER_MAX || q.write_size > MDM_RFS_MARKER_MAX - at) return 3;
	if (!free_slot) return 0;
	if (scratch && !file) {
		file = rfs_scratch_file(r, key, true, &err);
		if (!file) return (int)err;
	}
	uint32_t local = r->ops->open_port(r->context);
	if (!local) { if (scratch) rfs_release_scratch(r,file); return 0; }
	struct mdm_rfs_session *s = free_slot;
	s->local=local; s->node=node; s->peer=peer; s->req=q; s->born=now; s->is_sink=true;
	s->ram_file=file; s->scratch_file=scratch; s->unlink_request=unlink;
	s->marker_version=file ? file->version : 0;
	s->write_at=at;
	if ((append || seek) && file->present) {
		s->write_size=file->size;
		memcpy(s->marker_snapshot,file->bytes,file->size);
	}
	if (!unlink) rfs_write_stats(r,s)->accepted++;
	rfs_sink_reply(r,s,now);
	return -1;
}

static void
rfs_sink_receive(struct mdm_rfs *r, struct mdm_rfs_session *s, const uint8_t *p,
    size_t length, uint64_t now)
{
	struct mdm_rfs_sink_stats *stats = rfs_write_stats(r,s);
	uint32_t op = rfs_be16(p);
	if (op == RFS_ERROR) { rfs_close(r, s); return; }
	if (s->unlink_request || op != RFS_DATA || length < 4 || length - 4 > s->req.block) {
		stats->rejected_data++;
		rfs_error(r, s->local, s->node, s->peer, 4);
		rfs_close(r, s); return;
	}
	uint32_t block = rfs_be16(p + 2);
	if (s->sink_block && block == s->sink_block) {
		if (length - 4 != s->sink_last_length) {
			stats->rejected_data++;
			rfs_error(r, s->local, s->node, s->peer, 4);
			return;
		}
		stats->duplicates++;
		rfs_sink_reply(r, s, now); return;
	}
	if (s->sink_final || !s->sink_ack_sent || block != s->sink_block + 1) {
		stats->out_of_order++;
		rfs_sink_reply(r, s, now); return;
	}
	size_t bytes = length - 4;
	uint32_t limit = s->ram_file ? MDM_RFS_MARKER_MAX - s->write_at : RFS_CHECK_LIMIT;
	bool final = bytes < s->req.block;
	if ((s->req.options & (1u << RFS_TSIZE)) &&
	    (s->sink_bytes + bytes > s->req.write_size || (final && s->sink_bytes + bytes != s->req.write_size))) {
		stats->rejected_data++; rfs_error(r,s->local,s->node,s->peer,8); rfs_close(r,s); return;
	}
	if (bytes > limit - s->sink_bytes) {
		stats->rejected_data++;
		rfs_error(r, s->local, s->node, s->peer, 3);
		rfs_close(r, s); return;
	}
	/* Only the exact public marker writes to a private pending RAM snapshot.
	 * Readers see it after the final ACK has been submitted. */
	if (s->ram_file && bytes) {
		uint32_t offset = s->write_at + s->sink_bytes;
		memcpy(s->marker_snapshot + offset, p + 4, bytes);
		if (offset + bytes > s->write_size) s->write_size = (uint32_t)(offset + bytes);
	}
	s->sink_bytes += (uint32_t)bytes;
	s->sink_block = block;
	s->sink_last_length = (uint32_t)bytes;
	s->sink_ack_sent = false;
	s->sink_final = bytes < s->req.block;
	s->retries = 0;
	stats->data_bytes += bytes;
	stats->data_blocks++;
	rfs_sink_reply(r, s, now);
}

/* Window base is the last cumulative ACK. Sequence numbers remain wide in
 * memory; the wire rolls over at 65536. A whole final block is followed by
 * an empty DATA block. In particular a size query may end before any DATA. */
static void
rfs_window(struct mdm_rfs_session *s)
{
	s->next = s->acked + 1;
	s->window_end = s->acked + s->req.window;
	if (s->window_end > s->last_block) s->window_end = s->last_block;
}

static struct mdm_rfs_read_traffic *
rfs_read_traffic(struct mdm_rfs *r, const struct mdm_rfs_session *s)
{
	if (s->scratch_file) return &r->reads.scratch;
	return s->marker_read ? &r->reads.marker : &r->reads.firmware;
}

static void
rfs_pump(struct mdm_rfs *r, struct mdm_rfs_session *s, uint64_t now)
{
	size_t length;
	while (s->negotiating || s->next <= s->window_end) {
		size_t n = 0;
		if (s->negotiating) {
			length = rfs_oack(r, s);
		} else {
			uint64_t offset = (s->next - 1) * s->req.block;
			uint64_t left = s->size - offset;
			n = left < s->req.block ? (size_t)left : s->req.block;
			if (s->marker_read || s->scratch_file) memcpy(r->wire + 4, s->marker_snapshot + s->req.seek + offset, n);
			else if (n && modem_fat_read(r->fat, &s->file, s->req.seek + offset, r->wire + 4, n)) {
				r->stats.errors++;
				rfs_error(r, s->local, s->node, s->peer, 0);
				rfs_close(r, s);
				return;
			}
			r->wire[0] = 0; r->wire[1] = RFS_DATA;
			r->wire[2] = (uint8_t)(s->next >> 8); r->wire[3] = (uint8_t)s->next;
			length = 4 + n;
		}
		int rc = r->ops->send(r->context, s->local, s->node, s->peer, r->wire, length);
		if (r->reads.observing) {
			r->reads.last_response = s->negotiating ? RFS_OACK : RFS_DATA;
			r->reads.last_submitted = !rc;
		}
		if ((s->marker_read || s->scratch_file) && n) memset(r->wire + 4, 0, n);
		if (rc) {
			if (rc == EBUSY || rc == ENOBUFS || rc == EAGAIN) {
				s->due = now + 25;
			} else {
				r->stats.errors++;
				rfs_close(r, s);
			}
			return;
		}
		if (s->negotiating) {
			s->oack_sent = true;
			r->stats.oack++;
			if (s->req.options & (1u << RFS_TSIZE)) {
				r->stats.tsize_oack++;
				rfs_read_traffic(r,s)->tsize_oack++;
			}
			break;
		}
		if (s->next > s->highest_sent) s->highest_sent = s->next;
		s->next++;
		r->stats.data_blocks++;
		r->stats.data_bytes += n;
		rfs_read_traffic(r,s)->data_blocks++;
		rfs_read_traffic(r,s)->sent_bytes += n;
	}
	s->due = now + s->req.timeout_ms;
}

static unsigned
rfs_open(struct mdm_rfs *r, uint32_t node, uint32_t peer, const uint8_t *data,
    size_t length, uint64_t now)
{
	struct mdm_rfs_request req;
	const char *key = NULL;
	unsigned code = rfs_read_header(r, node, data + 2, data + length, &req, &key);
	bool marker = r->reads.last_root == RFS_READ_MARKER;
	if (code) {
		if (code == RFS_PATH_DENIED) { r->stats.namespace_denied++; code = 2; }
		if (code == 2) r->stats.denied++; else r->stats.malformed++;
		rfs_error(r, r->listener, node, peer, code);
		return code == 2 ? RFS_DENIED : RFS_BAD_REQUEST;
	}
	struct mdm_rfs_marker *file = marker ? &r->marker : NULL;
	if (key) {
		unsigned error = 0;
		file = rfs_scratch_file(r,key,false,&error);
		if (!file || !file->present) {
			if (!file && error == 2) {
				rfs_error(r,r->listener,node,peer,2); return RFS_DENIED;
			}
			r->scratch.missing++; r->stats.missing++;
			rfs_error(r,r->listener,node,peer,1); return RFS_SCRATCH_MISSING;
		}
	}
	struct mdm_rfs_session *free_slot = NULL;
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (!s->local) { if (!free_slot) free_slot = s; continue; }
		if (s->node != node || s->peer != peer || s->sink_done) continue;
		if (s->is_sink || s->ram_file != file || memcmp(&req, &s->req, sizeof(req))) {
			rfs_error(r, r->listener, node, peer, 0);
			return RFS_BUSY;
		} else if (!s->acked) {
			rfs_window(s);
			rfs_pump(r, s, now);
		}
		return RFS_REPEATED;
	}
	if (!free_slot) { rfs_error(r, r->listener, node, peer, 0); return RFS_BUSY; }
	uint32_t local = r->ops->open_port(r->context);
	if (!local) { rfs_error(r, r->listener, node, peer, 0); return RFS_BUSY; }
	struct mdm_rfs_session *s = free_slot;
	s->local = local; s->node = node; s->peer = peer;
	int rc = 0;
	s->marker_read = marker; s->scratch_file = key != NULL; s->ram_file = file;
	if (file) {
		if (!file->present) rc = ENOENT;
		else {
			s->file.size = file->size;
			memcpy(s->marker_snapshot, file->bytes, file->size);
		}
	} else rc = modem_fat_open(r->fat, req.path, &s->file);
	if (rc || req.seek > s->file.size ||
	    ((req.options & (1u << RFS_RSIZE)) && req.rsize > s->file.size - req.seek)) {
		code = (rc == ENOENT || rc == ENOTDIR || rc == EISDIR) ? 1 : (rc ? 0 : 8);
		if (code == 1) r->stats.missing++; else r->stats.errors++;
		rfs_error(r, local, node, peer, code);
		rfs_close(r, s);
		return code == 1 ? (marker ? RFS_MARKER_MISSING : RFS_FAT_MISSING) : (rc ? RFS_IO : RFS_RANGE);
	}
	s->mcfg_data = !file && r->reads.last_root == RFS_READ_MCFG;
	s->req = req;
	s->size = s->file.size - req.seek;
	if ((req.options & (1u << RFS_RSIZE)) && req.rsize < s->size) s->size = req.rsize;
	s->last_block = s->size / req.block + 1;
	s->born = now;
	s->negotiating = req.options != 0;
	rfs_window(s);
	r->stats.opened++;
	rfs_read_traffic(r,s)->opened++;
	rfs_pump(r, s, now);
	return key ? RFS_SCRATCH_OPEN : (marker ? RFS_MARKER_OPEN : RFS_FAT_OPEN);
}

static void
mdm_rfs_receive(struct mdm_rfs *r, uint32_t local, uint32_t node, uint32_t peer,
    const void *payload, size_t length, uint64_t now)
{
	if (!r->listener || !payload || length < 2 || length > 8192 || !node || !peer) return;
	const uint8_t *p = payload;
	unsigned op = rfs_be16(p);
	if (local == r->listener) {
		if (op == RFS_RRQ) {
			r->stats.rrq++;
			r->reads.last_ms = now;
			r->reads.last_response = r->reads.last_error = 0;
			r->reads.last_submitted = false;
			r->reads.observing = true;
			r->reads.last_outcome = rfs_open(r, node, peer, p, length, now);
			r->reads.observing = false;
			r->reads.outcomes[r->reads.last_outcome]++;
		}
		else if (op == RFS_WRQ) rfs_wrq(r, node, peer, p, length, now);
		else if (op != RFS_ERROR) rfs_error(r, local, node, peer, 5);
		return;
	}
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (s->local != local) continue;
		if (s->node != node || s->peer != peer) {
			if (op != RFS_ERROR) rfs_error(r, local, node, peer, 5);
			return;
		}
		if (s->is_sink) { rfs_sink_receive(r, s, p, length, now); return; }
		if (op == RFS_ERROR && length >= 5 && p[length - 1] == 0) {
			if (rfs_be16(p + 2) == 9) {
				r->stats.eot++;
				if (s->oack_sent && (s->req.options & (1u << RFS_TSIZE)) && !s->highest_sent) {
					r->stats.metadata_eot++;
					rfs_read_traffic(r,s)->metadata_eot++;
				}
			} else r->stats.errors++;
			rfs_close(r, s); return;
		}
		if (op != RFS_ACK || length != 4) {
			r->stats.malformed++;
			rfs_error(r, local, node, peer, 4);
			rfs_close(r, s); return;
		}
		unsigned ack = rfs_be16(p + 2);
		if (s->negotiating) {
			if (ack || !s->oack_sent) return;
			s->negotiating = false;
		} else {
			uint16_t advance = (uint16_t)(ack - (uint16_t)s->acked);
			uint64_t last = s->acked + advance;
			if (!advance || last > s->highest_sent) return;
			uint64_t before = s->acked * s->req.block, after = last * s->req.block;
			if (before > s->size) before = s->size;
			if (after > s->size) after = s->size;
			r->stats.acked_blocks += advance;
			r->stats.acked_bytes += after - before;
			rfs_read_traffic(r,s)->acked_bytes += after - before;
			s->acked = last;
			if (last == s->last_block) {
				r->stats.completed++;
				rfs_read_traffic(r,s)->completed++;
                if (s->mcfg_data && s->size) {
                    r->reads.mcfg_gate.completed++;
                    r->reads.mcfg_gate.acked_bytes+=s->size;
                    r->reads.mcfg_gate.node=s->node;
                    r->reads.mcfg_gate.last_ms=now;
                }
				rfs_close(r, s); return;
			}
		}
		s->retries = 0;
		rfs_window(s);
		rfs_pump(r, s, now); return;
	}
	if (op != RFS_ERROR) rfs_error(r, local, node, peer, 5);
}

static void
mdm_rfs_tick(struct mdm_rfs *r, uint64_t now)
{
	if (!r->listener) return;
	for (unsigned i = 0; i < MDM_RFS_SESSIONS; i++) {
		struct mdm_rfs_session *s = &r->sessions[i];
		if (!s->local) continue;
		if (s->is_sink) {
			if (now - s->born >= 120000 || (s->sink_done && now >= s->due)) {
				rfs_write_stats(r,s)->expired += !s->sink_done;
				rfs_close(r, s); continue;
			}
			if (now < s->due) continue;
			if (s->sink_ack_sent && ++s->retries > 3) {
				rfs_write_stats(r,s)->expired++; rfs_close(r, s); continue;
			}
			rfs_sink_reply(r, s, now); continue;
		}
		if (now - s->born >= 120000) {
			r->stats.expired++; rfs_close(r, s); continue;
		}
		if (now < s->due) continue;
		if ((s->negotiating && s->oack_sent) || (!s->negotiating && s->next > s->window_end)) {
			if (++s->retries > 3) { r->stats.expired++; rfs_close(r, s); continue; }
			r->stats.retransmits++;
			rfs_window(s);
		}
		rfs_pump(r, s, now);
	}
}
