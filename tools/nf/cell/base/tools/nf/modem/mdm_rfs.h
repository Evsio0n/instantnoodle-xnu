/* Read-only TFTP over the modem's QRTR route. The board serializes callbacks in a
 * sleepable context. The optional, content-free server check has no backing file.
 * The board owns the listener and transport; the server owns transfer TIDs. */
#ifndef MDM_RFS_H
#define MDM_RFS_H
#include "modem_fat.h"

#define MDM_RFS_SESSIONS 4
#define MDM_RFS_PATH 384
#define MDM_RFS_MARKER_MAX 4096u
#define MDM_RFS_SCRATCH_FILES 8
/* Leave space for the QRTR header and the TFTP header in an 8 KiB MHI element. */
#define MDM_RFS_BLOCK 7680u
/* One outstanding block leaves the shared IPCR UL ring available to QMI. */
#define MDM_RFS_WINDOW 1u
struct mdm_rfs_ops {
	uint32_t (*open_port)(void *);
	void (*close_port)(void *, uint32_t local);
	int (*send)(void *, uint32_t local, uint32_t node, uint32_t peer, const void *, size_t);
};
struct mdm_rfs_stats {
	uint32_t rrq, wrq, denied, missing, malformed, opened, oack, tsize_oack;
	uint32_t data_blocks, acked_blocks, completed, eot, metadata_eot;
	uint32_t expired, revoked, errors, retransmits, namespace_denied;
	uint64_t data_bytes, acked_bytes;
};
/* Anonymous WRQ observations. The two last-peer numbers are transport state,
 * never exported. A namespace and option mask cannot identify a private file
 * or distinguish a retransmission from a new request on the same TID. */
enum mdm_rfs_root {
	MDM_RFS_INVALID, MDM_RFS_PERSIST, MDM_RFS_FIRMWARE, MDM_RFS_MCFG,
	MDM_RFS_VENDOR_FIRMWARE, MDM_RFS_RAMDUMP, MDM_RFS_OTHER, MDM_RFS_SHARED, MDM_RFS_HLOS, MDM_RFS_ROOTS
};
enum mdm_rfs_option_bits {
	MDM_RFS_OPT_BLOCK = 1, MDM_RFS_OPT_TSIZE = 2, MDM_RFS_OPT_SEEK = 4,
	MDM_RFS_OPT_RSIZE = 8, MDM_RFS_OPT_WINDOW = 16, MDM_RFS_OPT_TIMEOUT = 32,
	MDM_RFS_OPT_APPEND = 64, MDM_RFS_OPT_UNLINK = 128, MDM_RFS_OPT_UNKNOWN = 256
};
struct mdm_rfs_wrq_stats {
	uint32_t roots[MDM_RFS_ROOTS], submitted, submit_failed, options_seen;
	uint32_t same_peer_adjacent, same_shape_adjacent, public_server_check;
	uint64_t first_ms, last_ms, delta_ms;
	uint32_t last_root, last_options, last_node, last_peer;
	bool last_valid, last_same_peer, last_same_shape, last_submitted, last_public_server_check, last_sink_accepted, last_scratch_accepted;
};
struct mdm_rfs_sink_stats {
	uint32_t accepted, oack, ack0, data_blocks, duplicates, out_of_order;
	uint32_t ack_submitted, ack_busy, send_failed, completed, expired, revoked, rejected_data;
	uint64_t data_bytes, last_completed_ms;
};
enum mdm_rfs_read_root {
	RFS_READ_INVALID, RFS_READ_MARKER, RFS_READ_PERSIST, RFS_READ_VENDOR,
	RFS_READ_IMAGE, RFS_READ_MCFG, RFS_READ_OTHER, RFS_READ_FIRMWARE_OTHER,
	RFS_READ_SHARED, RFS_READ_HLOS, RFS_READ_ROOTS
};
enum mdm_rfs_read_outcome {
	RFS_BAD_REQUEST, RFS_DENIED, RFS_FAT_MISSING, RFS_MARKER_MISSING,
	RFS_FAT_OPEN, RFS_MARKER_OPEN, RFS_REPEATED, RFS_BUSY, RFS_IO, RFS_RANGE,
	RFS_SCRATCH_MISSING, RFS_SCRATCH_OPEN, RFS_OUTCOMES
};
struct mdm_rfs_read_traffic {
	uint32_t opened, tsize_oack, metadata_eot, data_blocks, completed;
	uint64_t sent_bytes, acked_bytes;
};
struct mdm_rfs_reads {
	uint32_t roots[RFS_READ_ROOTS], outcomes[RFS_OUTCOMES];
	uint32_t last_root, last_options, last_outcome, last_response, last_error;
	uint64_t last_ms;
	bool last_valid, last_allowed, last_submitted, observing;
	struct mdm_rfs_read_traffic firmware, marker, scratch;
	struct { uint64_t acked_bytes,last_ms; uint32_t completed,node; } mcfg_gate;
};
struct mdm_rfs_marker {
	uint32_t size, version, commits, unlinks, unlink_missing, unlink_oack, last_commit_size;
	bool present, cleared;
	uint8_t bytes[MDM_RFS_MARKER_MAX];
};
struct mdm_rfs_scratch_entry {
	/* A transient lookup key. Never emitted, resolved against a real path
	 * or populated by copying from any preexisting cellular data store. */
	char key[MDM_RFS_PATH];
	struct mdm_rfs_marker file;
};
struct mdm_rfs_scratch {
	uint32_t commits, unlinks, unlink_missing, missing, full;
	bool cleared;
	struct mdm_rfs_sink_stats sink;
	struct mdm_rfs_scratch_entry entries[MDM_RFS_SCRATCH_FILES];
};
struct mdm_rfs_request {
	char path[MDM_RFS_PATH];
	uint32_t options, block, window, timeout_ms, seek, rsize, unlink_value, write_size;
};
struct mdm_rfs_session {
	struct mdm_rfs_request req;
	struct modem_fat_file file;
	uint32_t local, node, peer;
	uint64_t size, acked, highest_sent, next, window_end, last_block;
	uint64_t born, due;
	unsigned retries;
	bool negotiating, oack_sent;
	/* The check holds only a byte count and block number, never DATA. */
	bool is_sink, sink_ack_sent, sink_final, sink_done;
	uint32_t sink_block, sink_bytes, sink_last_length;
	bool marker_read, unlink_request, scratch_file;
	bool mcfg_data;
	struct mdm_rfs_marker *ram_file;
	uint32_t write_at, write_size;
	uint32_t marker_version;
	uint8_t marker_snapshot[MDM_RFS_MARKER_MAX];
};
struct mdm_rfs {
	const struct mdm_rfs_ops *ops;
	struct modem_fat *fat;
	void *context;
	uint32_t listener;
	struct mdm_rfs_stats stats;
	struct mdm_rfs_wrq_stats writes;
	bool check_enabled, marker_file_enabled, scratch_enabled;
	struct mdm_rfs_scratch scratch;
	struct mdm_rfs_marker marker;
	struct mdm_rfs_reads reads;
	struct mdm_rfs_sink_stats sink;
	struct mdm_rfs_session sessions[MDM_RFS_SESSIONS];
	uint8_t wire[4 + MDM_RFS_BLOCK];
};

static void mdm_rfs_start(struct mdm_rfs *, struct modem_fat *, const struct mdm_rfs_ops *, void *, uint32_t);
static void mdm_rfs_receive(struct mdm_rfs *, uint32_t, uint32_t, uint32_t, const void *, size_t, uint64_t);
static void mdm_rfs_tick(struct mdm_rfs *, uint64_t);
static void mdm_rfs_drop_peer(struct mdm_rfs *, uint32_t, uint32_t);
static void mdm_rfs_stop(struct mdm_rfs *);
#endif
