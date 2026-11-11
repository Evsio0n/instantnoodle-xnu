/* A small FAT16 volume exercises the normal file reader and the TFTP wire
 * contract. No modem firmware, system partition, or hardware is accessed. */
#include "mdm_rfs.inc"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#define SECTOR 512u
static uint8_t volume[5024 * SECTOR], sent[8192];
static size_t sent_size;
static uint32_t source, next_port = 40;
static unsigned closed;
static bool busy;

static int read_volume(void *ctx, uint64_t offset, void *out, size_t n)
{
	(void)ctx;
	if (offset > sizeof(volume) || n > sizeof(volume) - offset) return EIO;
	memcpy(out, volume + offset, n);
	return 0;
}
static void *alloc_volume(void *ctx, size_t n) { (void)ctx; return calloc(1, n); }
static void free_volume(void *ctx, void *v, size_t n) { (void)ctx; (void)n; free(v); }
static uint32_t open_port(void *ctx) { (void)ctx; return ++next_port; }
static void close_port(void *ctx, uint32_t port) { (void)ctx; assert(port > 40); closed++; }
static int send_packet(void *ctx, uint32_t local, uint32_t node, uint32_t peer,
    const void *packet, size_t n)
{
	(void)ctx;
	assert(node && peer && n <= sizeof(sent));
	if (busy) { busy = false; return EAGAIN; }
	memcpy(sent, packet, n); source = local; sent_size = n;
	return 0;
}
static void entry(unsigned sector, const char *name, bool directory, unsigned cluster,
    unsigned size)
{
	uint8_t *p = volume + SECTOR * sector;
	memcpy(p, name, 11);
	p[11] = directory ? 16 : 32;
	p[26] = (uint8_t)cluster; p[28] = (uint8_t)size;
}
static void fixture(void)
{
	volume[11] = 0; volume[12] = 2; volume[13] = 1; volume[14] = 1;
	volume[16] = 1; volume[17] = 16; volume[19] = 0xa0; volume[20] = 0x13;
	volume[22] = 20; volume[510] = 0x55; volume[511] = 0xaa;
	for (unsigned c = 2; c <= 5; c++) {
		volume[SECTOR + c * 2] = 0xff; volume[SECTOR + c * 2 + 1] = 0xff;
	}
	entry(21, "IMAGE      ", true, 2, 0);
	entry(22, "MODEM_PR   ", true, 3, 0);
	entry(23, "MCFG       ", true, 4, 0);
	entry(24, "CATALOG BIN", false, 5, 22);
	memcpy(volume + 25 * SECTOR, "abcdefghijklmnopqrstuv", 22);
}
#define Z(s) s "\0"
#define CATALOG Z("/readonly/firmware/image/modem_pr/mcfg/catalog.bin")
#define RRQ "\0\1"
#define ACK "\0\4"
#define PUT(port,peer,p,now) mdm_rfs_receive(&r,port,3,peer,p,sizeof(p)-1,now)

int main(void)
{
	static struct mdm_rfs r;
	static const struct mdm_rfs_ops ops = {open_port, close_port, send_packet};
	static const struct modem_fat_ops fops = {read_volume, alloc_volume, free_volume};
	struct modem_fat fat;
	fixture(); assert(!modem_fat_mount(&fat, &fops, NULL));
	mdm_rfs_start(&r, &fat, &ops, NULL, 8);
	PUT(8, 7, RRQ CATALOG Z("octet") Z("tsize") Z("0"), 0);
	uint32_t metadata_port = source;
	assert(rfs_be16(sent) == RFS_OACK && sent_size == 11 && !memcmp(sent + 2, "tsize\0" "22\0", 9));
	PUT(metadata_port, 7, "\0\5\0\11" Z("EOT"), 5);
	assert(r.stats.metadata_eot == 1 && !r.stats.completed && !r.stats.data_bytes);

	static const char range[] = RRQ CATALOG Z("octet") Z("seek") Z("3")
	    Z("rsize") Z("16") Z("blksize") Z("8") Z("wsize") Z("9");
	PUT(8, 7, range, 10);
	const uint32_t tid = source;
	assert(tid != metadata_port && rfs_be16(sent) == RFS_OACK && sent[sent_size - 2] == '1');
	PUT(tid, 99, ACK "\0\0", 11); assert(rfs_be16(sent + 2) == 5 && !r.stats.data_blocks);
	busy = true; PUT(tid, 7, ACK "\0\0", 12); assert(!r.stats.data_blocks);
	mdm_rfs_tick(&r, 37);
	assert(sent_size == 12 && rfs_be16(sent + 2) == 1 && !memcmp(sent + 4, "defghijk", 8));
	mdm_rfs_tick(&r, 1038); assert(r.stats.retransmits == 1 && rfs_be16(sent + 2) == 1);
	PUT(tid, 7, ACK "\0\1", 1039);
	assert(rfs_be16(sent + 2) == 2 && !memcmp(sent + 4, "lmnopqrs", 8));
	PUT(tid, 7, ACK "\0\2", 1040); assert(sent_size == 4 && rfs_be16(sent + 2) == 3);
	PUT(tid, 7, ACK "\0\3", 1041);
	assert(r.stats.completed == 1 && r.stats.acked_bytes == 16 && r.stats.acked_blocks == 3);
	assert(r.stats.data_bytes == 24 && closed == 2);

	PUT(8, 7, RRQ Z("readonly/firmware/image/modem_pr/mcfg/missing.bin") Z("octet"), 1100);
	assert(rfs_be16(sent + 2) == 1 && r.stats.missing == 1);
	PUT(8, 7, RRQ Z("readonly/firmware/image/modem_pr/mcfg/../x") Z("octet"), 1101);
	PUT(8, 7, RRQ Z("/readonly/vendor/firmware/x") Z("octet"), 1102);
	PUT(8, 7, RRQ Z("/readonly/firmware/image/sdx55m/efs1.bin") Z("octet"), 1103);
	PUT(8, 7, "\0\2" CATALOG Z("octet"), 1104);
	assert(r.stats.namespace_denied == 3 && r.stats.denied == 4 && r.stats.wrq == 1);
	assert(rfs_be16(sent + 2) == 2);
	assert(r.writes.submitted == 1 && r.writes.last_root == MDM_RFS_MCFG);
	unsigned opens_before_wrq = next_port;
	PUT(8, 7, "\0\2" Z("/readwrite/server_check.txt") Z("octet") Z("unlink") Z("1"), 1105);
	assert(r.writes.last_public_server_check && r.writes.public_server_check == 1);
	assert(r.writes.last_options == MDM_RFS_OPT_UNLINK && r.writes.last_same_peer && !r.writes.last_same_shape);
	busy = true;
	PUT(8, 7, "\0\2" Z("readwrite/server_check.txt") Z("octet") Z("unlink") Z("1"), 1109);
	assert(r.writes.delta_ms == 4 && r.writes.last_same_shape && r.writes.submit_failed == 1);
	assert(!r.writes.last_submitted && r.writes.public_server_check == 2);
	PUT(8, 9, "\0\2" Z("readonly/vendor/firmware/x") Z("octet") Z("blksize") Z("512"), 1111);
	assert(!r.writes.last_same_peer && r.writes.last_root == MDM_RFS_VENDOR_FIRMWARE);
	PUT(8, 9, "\0\2" Z("readwrite/../bad") Z("octet"), 1112);
	assert(!r.writes.last_valid && r.writes.last_root == MDM_RFS_INVALID);
	assert(next_port == opens_before_wrq && r.writes.submitted + r.writes.submit_failed == r.stats.wrq);
	PUT(8, 7, RRQ CATALOG Z("octet") Z("seek") Z("21") Z("rsize") Z("16"), 1105);
	assert(rfs_be16(sent + 2) == 8 && r.stats.data_bytes == 24);
	PUT(8, 7, RRQ CATALOG Z("octet") Z("seek") Z("4294967296"), 1106);
	assert(rfs_be16(sent + 2) == 8);
	PUT(8, 7, RRQ CATALOG Z("octet") Z("blksize") "16", 1107);
	assert(rfs_be16(sent + 2) == 8);
	PUT(8, 7, RRQ CATALOG Z("octet"), 1200);
	assert(rfs_be16(sent) == RFS_DATA && !memcmp(sent + 4, "abcdefghijklmnopqrstuv", 22));
	PUT(8, 8, RRQ CATALOG Z("octet"), 1201);
	mdm_rfs_drop_peer(&r, 3, 7);
	assert(r.stats.revoked == 1);
	mdm_rfs_tick(&r, 121201); assert(r.stats.expired == 1);
	PUT(8, 7, RRQ CATALOG Z("octet"), 121202);
	mdm_rfs_stop(&r); assert(r.stats.revoked == 2 && r.listener == 0);
	/* Enabling the public marker affects only ordinary WRQ on the MPSS node.
	 * Unknown options, other names and unlink follow the original denial. */
	mdm_rfs_start(&r, &fat, &ops, NULL, 8);
	r.check_enabled = true;
	#define MARKER Z("/readwrite/server_check.txt")
	#define WRQ "\0\2"
	#define DATA "\0\3"
	static const char check[] = WRQ MARKER Z("octet") Z("blksize") Z("8")
	    Z("wsize") Z("3") Z("timeoutms") Z("1000");
	PUT(8, 7, WRQ MARKER Z("octet") Z("unlink") Z("1"), 0);
	PUT(8, 7, WRQ MARKER Z("octet") Z("other") Z("1"), 0);
	PUT(8, 7, WRQ Z("/readwrite/server_check.txtx") Z("octet"), 0);
	mdm_rfs_receive(&r, 8, 2, 7, check, sizeof(check) - 1, 0);
	assert(r.writes.submitted == 4 && !r.sink.accepted);
	PUT(8, 7, check, 1);
	const uint32_t write_tid = source;
	assert(r.sink.accepted == 1 && r.sink.oack == 1 && !r.sink.completed && !r.sink.data_blocks);
	PUT(8, 7, check, 2);
	assert(source == write_tid && r.sink.accepted == 1 && r.sink.oack == 2);
	PUT(write_tid, 9, DATA "\0\1" "abc", 3);
	assert(rfs_be16(sent) == RFS_ERROR && !r.sink.data_blocks);
	PUT(write_tid, 7, DATA "\0\2" "wrong", 4);
	assert(r.sink.out_of_order == 1 && rfs_be16(sent) == RFS_OACK && !r.sink.data_blocks);
	PUT(write_tid, 7, DATA "\0\1" "abcdefgh", 5);
	assert(r.sink.data_bytes == 8 && r.sink.data_blocks == 1 && !r.sink.completed);
	assert(rfs_be16(sent) == RFS_ACK && rfs_be16(sent + 2) == 1);
	PUT(write_tid, 7, DATA "\0\1" "abcdefgh", 6);
	assert(r.sink.duplicates == 1 && r.sink.data_blocks == 1 && r.sink.ack_submitted == 2);
	busy = true;
	PUT(write_tid, 7, DATA "\0\2", 7);
	assert(r.sink.data_blocks == 2 && r.sink.ack_busy == 1 && !r.sink.completed);
	mdm_rfs_tick(&r, 32);
	assert(r.sink.completed == 1 && r.sink.last_completed_ms == 32 && rfs_be16(sent + 2) == 2);
	PUT(write_tid, 7, DATA "\0\2", 33);
	assert(r.sink.duplicates == 2 && r.sink.completed == 1 && r.sink.data_bytes == 8);
	PUT(8, 9, WRQ MARKER Z("octet"), 34);
	uint32_t other_tid = source;
	assert(rfs_be16(sent) == RFS_ACK && rfs_be16(sent + 2) == 0 && r.sink.ack0 == 1);
	PUT(other_tid, 9, DATA "\0\1" "x", 35);
	assert(r.sink.data_bytes == 9 && r.sink.completed == 2 && !r.stats.opened);
	PUT(8, 7, check, 40);
	assert(r.sink.accepted == 3 && r.sink.completed == 2);
	mdm_rfs_stop(&r);
	assert(r.sink.revoked == 1 && r.stats.denied == 4);
	mdm_rfs_start(&r, &fat, &ops, NULL, 8);
	r.check_enabled = r.marker_file_enabled = true;
	PUT(8, 7, RRQ MARKER Z("octet"), 100);
	assert(r.reads.last_outcome == RFS_MARKER_MISSING && rfs_be16(sent + 2) == 1);
	PUT(8, 7, WRQ MARKER Z("octet"), 101);
	uint32_t mt = source;
	busy = true; PUT(mt, 7, DATA "\0\1" "first", 102);
	assert(!r.marker.present && !r.marker.commits);
	mdm_rfs_tick(&r, 127);
	assert(r.marker.present && r.marker.commits == 1 && r.marker.size == 5);
	PUT(8, 7, RRQ MARKER Z("octet") Z("tsize") Z("0") Z("seek") Z("1") Z("rsize") Z("3"), 128);
	uint32_t rt = source;
	assert(r.reads.last_outcome == RFS_MARKER_OPEN && r.reads.last_response == RFS_OACK);
	assert(r.reads.marker.tsize_oack == 1 && !r.reads.marker.data_blocks);
	PUT(8, 9, WRQ MARKER Z("octet"), 129);
	uint32_t wt2 = source;
	PUT(wt2, 9, DATA "\0\1" "new", 130);
	assert(r.marker.size == 3 && r.marker.commits == 2);
	PUT(rt, 7, ACK "\0\0", 131);
	assert(sent_size == 7 && !memcmp(sent + 4, "irs", 3));
	PUT(rt, 7, ACK "\0\1", 132);
	assert(r.reads.marker.acked_bytes == 3 && r.reads.marker.completed == 1 && !r.reads.firmware.completed);
	PUT(8, 11, RRQ MARKER Z("octet"), 133);
	rt = source; assert(sent_size == 7 && !memcmp(sent + 4, "new", 3)); PUT(rt, 11, ACK "\0\1", 134);
	PUT(8, 10, WRQ MARKER Z("octet") Z("unlink") Z("1"), 135);
	assert(!r.marker.present && r.marker.unlinks == 1 && r.marker.unlink_oack == 1);
	assert(rfs_be16(sent) == RFS_OACK && !memcmp(sent + 2, "unlink\0" "0\0", 9));
	PUT(8, 10, WRQ MARKER Z("octet") Z("unlink") Z("1"), 136);
	assert(r.marker.unlinks == 1 && r.marker.unlink_oack == 2);
	PUT(8, 12, RRQ MARKER Z("octet"), 137);
	assert(r.reads.last_outcome == RFS_MARKER_MISSING);
	PUT(8, 15, WRQ MARKER Z("octet") Z("unlink") Z("1"), 138);
	assert(r.marker.unlink_missing == 1 && rfs_be16(sent + 2) == 2);
	PUT(8, 12, RRQ Z("/readonly/vendor/firmware/x") Z("octet"), 139);
	assert(r.reads.last_root == RFS_READ_VENDOR && r.reads.last_outcome == RFS_DENIED);
	PUT(8, 12, RRQ Z("/readonly/firmware/image/sdx55m/apps.mbn") Z("octet"), 140);
	assert(r.reads.last_allowed && r.reads.last_root == RFS_READ_IMAGE && r.reads.last_outcome == RFS_FAT_MISSING);
	PUT(8, 12, RRQ Z("/readonly/firmware/image/sdx55m/efs1.bin") Z("octet"), 141);
	assert(!r.reads.last_allowed && r.reads.last_outcome == RFS_DENIED);
	mdm_rfs_stop(&r); assert(r.marker.cleared && !r.marker.present && !r.marker.size);
	for (unsigned i=0; i<sizeof(r.marker.bytes); i++) assert(r.marker.bytes[i]==0);
	mdm_rfs_start(&r, &fat, &ops, NULL, 8);
	r.check_enabled = r.marker_file_enabled = r.scratch_enabled = true;
	#define TEMP Z("/readwrite/test/subdir/item")
	PUT(8, 7, RRQ TEMP Z("octet") Z("tsize") Z("0"), 0);
	assert(r.reads.last_root == RFS_READ_PERSIST && r.reads.last_allowed &&
	    r.reads.last_outcome == RFS_SCRATCH_MISSING && rfs_be16(sent + 2) == 1);
	PUT(8, 7, RRQ Z("/shared/item") Z("octet"), 1);
	assert(r.reads.last_root == RFS_READ_SHARED && rfs_be16(sent + 2) == 2);
	PUT(8, 7, RRQ Z("/hlos/item") Z("octet"), 2);
	assert(r.reads.last_root == RFS_READ_HLOS && rfs_be16(sent + 2) == 2);
	PUT(8, 7, WRQ Z("/readwrite/../outside") Z("octet"), 3);
	assert(rfs_be16(sent + 2) == 2);
	PUT(8, 7, WRQ TEMP Z("octet") Z("tsize") Z("3"), 4);
	assert(r.writes.last_scratch_accepted && !r.writes.last_sink_accepted);
	wt2 = source;
	PUT(8, 8, RRQ TEMP Z("octet"), 5);
	assert(r.reads.last_outcome == RFS_SCRATCH_MISSING);
	busy=true; PUT(wt2, 7, DATA "\0\1" "abc", 6);
	assert(!r.scratch.commits);
	mdm_rfs_tick(&r,31); assert(r.scratch.commits==1 && !r.marker.commits);
	PUT(8,8,RRQ TEMP Z("octet") Z("tsize") Z("0"),32);
	rt=source; assert(r.reads.last_outcome==RFS_SCRATCH_OPEN && r.reads.scratch.tsize_oack==1);
	PUT(8,9,WRQ TEMP Z("octet") Z("append") Z("1"),33);
	wt2=source; PUT(wt2,9,DATA "\0\1" "de",34); assert(r.scratch.commits==2);
	PUT(rt,8,ACK "\0\0",35);
	assert(sent_size==7 && !memcmp(sent+4,"abc",3)); PUT(rt,8,ACK "\0\1",36);
	mdm_rfs_tick(&r,3000);
	PUT(8,7,WRQ TEMP Z("octet") Z("seek") Z("1"),3001);
	wt2=source; PUT(wt2,7,DATA "\0\1" "Z",3002);
	PUT(8,9,RRQ TEMP Z("octet"),3003);
	rt=source; assert(sent_size==9 && !memcmp(sent+4,"aZcde",5)); PUT(rt,9,ACK "\0\1",3004);
	assert(r.reads.scratch.completed==2 && !r.reads.marker.completed && !r.reads.firmware.completed);
	mdm_rfs_receive(&r,8,2,7,RRQ TEMP Z("octet"),sizeof(RRQ TEMP Z("octet"))-1,3005);
	assert(!r.reads.last_allowed && rfs_be16(sent+2)==2);
	PUT(8,10,WRQ TEMP Z("octet") Z("unlink") Z("1"),3006);
	assert(r.scratch.unlinks==1 && rfs_be16(sent)==RFS_OACK);
	PUT(8,8,RRQ TEMP Z("octet"),3007);
	assert(r.reads.last_outcome==RFS_SCRATCH_MISSING && rfs_be16(sent+2)==1);
	mdm_rfs_drop_peer(&r,3,0); mdm_rfs_stop(&r);
	assert(r.scratch.cleared);
	for(unsigned i=0;i<MDM_RFS_SCRATCH_FILES;i++) {
		assert(!r.scratch.entries[i].key[0] && !r.scratch.entries[i].file.present);
		for(unsigned b=0;b<MDM_RFS_MARKER_MAX;b++) assert(!r.scratch.entries[i].file.bytes[b]);
	}
	puts("RFS read-only FAT, metadata, offset/size, final ACK, backpressure, isolation, denial and teardown verified");
	return 0;
}
