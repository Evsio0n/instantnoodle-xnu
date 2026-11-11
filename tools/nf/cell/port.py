#!/usr/bin/env python3
"""port.py REF_DIR OUT_DIR: the OnePlus 8 Pro port's modem driver (dekefake/hackios-op8pro @ e5631cd, APSL 2.0) made into a
second cellular path of our tree, behind boot-arg cell=1. REF_DIR holds pro_modem.c, pro_hk_mhi.c, pro_hk_mhi.h and pro_hk_net.h
as fetched; OUT_DIR gets instantnoodle_cell.c, hk_mhi.c, hk_mhi.h and cell_net.h. Every change is made here, so the port can be
made again from a newer copy of the Pro's file. It stops if a place it changes is not found exactly once."""
import re
import sys
from pathlib import Path

REF, OUT = Path(sys.argv[1]), Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)


def once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit('port.py: %d of %r' % (text.count(old), old[:70]))
    return text.replace(old, new)


def span(text, start, end, new):
    """The text from `start` up to and with `end`, replaced."""
    if text.count(start) != 1:
        raise SystemExit('port.py: %d of %r' % (text.count(start), start[:70]))
    a = text.index(start)
    b = text.index(end, a) + len(end)
    return text[:a] + new + text[b:]


HEAD = '''/*
 * THE SECOND CELLULAR PATH OF THE ONEPLUS 8 (boot-arg cell=1): the OnePlus 8 Pro port's modem driver, as it is, on this phone.
 * From github.com/dekefake/hackios-op8pro, commit e5631cd2523a8a7a897618a3535fe2a08b1672dc, xnu/files/bsd/dev/instantnoodle_modem.c
 * (Apple Public Source License 2.0, as this tree). Same SoC, same Snapdragon X55 behind PCIe 2, same control lines. On the Pro it
 * reaches the network, mobile data, SMS and calls; our own path (tools/nf/modem, modem=3) stayed "not ready" (DMS error 52) for a
 * day, with no DIAG channel to say why and no answer to the modem's EFS sync. Made by tools/nf/cell/port.py, which is the list of
 * what differs from the Pro's file:
 *   - storage is read through /dev/rufs_<partition> (the Pro reads logical units by its own UFS driver and walks their GPT);
 *   - the firmware is modem_a's unless modem_fw=b;
 *   - it does not start a thread: our instantnoodle_modem.c does the warm reset this phone's RAM runs need and calls hk_cell_run(),
 *     and at the run's end calls hk_cell_quiesce() before its own modem_stop();
 *   - the network interface (en1) is cell_net.c; what CommCenter's stand-in reads (SMS, calls) and a call's sound are port.py's later sections.
 * The Pro's own header follows.
 */
'''

STORAGE = r'''
/* ---- storage, this tree's way: a partition is /dev/rufs_<name> (bsd/dev/hk_ufs.c), read in whole pieces of CELL_CACHE --------- */

#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/disk.h>
#include <sys/kauth.h>
#include <sys/proc.h>

extern int vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset, enum uio_seg segflg, int ioflg, kauth_cred_t cred, int *aresid, proc_t p);

#define CELL_CACHE              (256u * 1024u)
#define CELL_PARTS              6
static struct cell_part {
	char name[24];
	vnode_t vp;
	uint8_t *cache;
	uint64_t cached_at, bytes;
	bool have;
} cell_parts[CELL_PARTS];

static struct cell_part *
cell_part(const char *name)
{
	struct cell_part *p = NULL;
	char path[48];

	for (unsigned i = 0; i < CELL_PARTS; i++) {
		if (cell_parts[i].vp != NULL && strcmp(cell_parts[i].name, name) == 0) {
			return &cell_parts[i];
		}
		if (p == NULL && cell_parts[i].vp == NULL) {
			p = &cell_parts[i];
		}
	}
	if (p == NULL) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/dev/rufs_%s", name);
	if (p->cache == NULL && (p->cache = kalloc_data(CELL_CACHE, Z_WAITOK | Z_ZERO)) == NULL) {
		return NULL;
	}
	if (vnode_open(path, FREAD, 0, 0, &p->vp, vfs_context_kernel()) != 0) {
		p->vp = NULL;
		return NULL;
	}
	strlcpy(p->name, name, sizeof(p->name));
	p->have = false;
	{
		uint64_t blocks = 0;
		uint32_t size = 0;

		if (VNOP_IOCTL(p->vp, DKIOCGETBLOCKCOUNT, (caddr_t)&blocks, 0, vfs_context_kernel()) == 0 &&
		    VNOP_IOCTL(p->vp, DKIOCGETBLOCKSIZE, (caddr_t)&size, 0, vfs_context_kernel()) == 0) {
			p->bytes = blocks * size;
		}
	}
	return p;
}

static int
cell_part_read(const char *name, uint64_t offset, void *dst, uint64_t length)
{
	struct cell_part *p = cell_part(name);
	uint8_t *to = dst;

	if (p == NULL) {
		return ENOENT;
	}
	while (length > 0) {
		uint64_t base = offset & ~((uint64_t)CELL_CACHE - 1);
		size_t within = (size_t)(offset - base), take = CELL_CACHE - within;

		if (!p->have || p->cached_at != base) {
			int resid = 0, error = vn_rdwr(UIO_READ, p->vp, (caddr_t)p->cache, CELL_CACHE, (off_t)base, UIO_SYSSPACE, IO_NOAUTH | IO_NOCACHE, kauth_cred_get(), &resid, current_proc());

			if (error != 0 || resid == CELL_CACHE) {
				p->have = false;
				return error != 0 ? error : EIO;
			}
			if (resid > 0) {
				bzero(p->cache + CELL_CACHE - resid, (size_t)resid);    /* (past the partition's end) */
			}
			p->have = true;
			p->cached_at = base;
		}
		take = take < length ? take : (size_t)length;
		memcpy(to, p->cache + within, take);
		to += take;
		offset += take;
		length -= take;
	}
	return 0;
}

static const char *fw_partition;
static const char *cell_lun5_name = "mdm1m9kefs1";

/* The Pro's reads name a logical unit: 4 is the firmware's partition here, 5 the EFS partition last asked for by gpt_find(). */
static int
instantnoodle_ufs_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	return cell_part_read(lun == 5 ? cell_lun5_name : fw_partition, offset, dst, len);
}

/* ---- what this tree does not have yet: the audio DSP's side of a call (the Pro's hk_adsp.c), the PMIC's clock ------------------ */

static void hk_adsp_qrtr_from_modem(uint32_t type, uint32_t from_node, uint32_t from_port, uint32_t to_node, uint32_t to_port, bool confirm, const void *data, uint32_t length)
{
	(void)type; (void)from_node; (void)from_port; (void)to_node; (void)to_port; (void)confirm; (void)data; (void)length;
}
static void hk_adsp_qrtr_announce(void) { }
static uint32_t hk_adsp_ssctl_port(void) { return 0; }
static bool hk_adsp_sat_open(void) { return false; }
static int hk_adsp_sat_send(const void *data, uint32_t length) { (void)data; (void)length; return ENXIO; }
static uint32_t hk_rtc_seconds(void) { return 0; }
'''

QUIESCE = r'''
/*
 * At the run's end (osfmk/arm64/loader_return.c, through our instantnoodle_modem.c's hk_modem_quiesce(), which then does the warm
 * reset): the link down and the stream given back before the memory the modem may reach is reused. No lock and no sleep: the
 * other processors have stopped, and this is on a panic's way out as well.
 */
void hk_cell_quiesce(void);
void
hk_cell_quiesce(void)
{
	pcie.up = false;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (pcie.clocked) {
		instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
		wr(pcie.phy + PHY_POWER_DOWN, 0);
		set_bits(pcie.parf + PARF_PHY_CTRL, 0, 1);
	}
	if (smmu.on) {
		smmu.global[(0x800 + 4 * smmu.match) / 4] = smmu.old_smr;
		smmu.global[(0xc00 + 4 * smmu.match) / 4] = smmu.old_s2cr;
		smmu.cb[0] = smmu.old_sctlr;
		__asm__ volatile ("dsb sy" ::: "memory");
		smmu.on = false;
	}
}

/* Whether this path has brought PCIe 2 up (our instantnoodle_modem.c asks at the run's end). */
bool hk_cell_started(void);
bool
hk_cell_started(void)
{
	return pcie.clocked;
}
'''

t = (REF / 'pro_modem.c').read_text(encoding='utf-8')
t = HEAD + t
t = once(t, '#include <sys/hk_mhi.h>\n', '#include "hk_mhi.h"\n')
t = once(t, '#include <sys/hk_net.h>\n', '#include "cell_net.h"\n')
t = once(t, 'extern int instantnoodle_ufs_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len);\n', '')
# the adsp and RTC stand-ins are static: their extern declarations go
for line in ('extern void hk_adsp_qrtr_from_modem(uint32_t type, uint32_t from_node, uint32_t from_port, uint32_t to_node, uint32_t to_port, bool confirm, const void *data, uint32_t length);\n',
             'extern void hk_adsp_qrtr_announce(void);\n', 'extern uint32_t hk_adsp_ssctl_port(void);\n', 'extern bool hk_adsp_sat_open(void);\n',
             'extern int hk_adsp_sat_send(const void *data, uint32_t length);\n'):
    t = once(t, line, '')
t = once(t, '\t\textern uint32_t hk_rtc_seconds(void);\n', '')
# storage: before the firmware section, and the firmware's partition is modem_a here
t = once(t, 'static const char *fw_partition = "modem_b";', 'static const char *fw_partition = "modem_a";')
t = once(t, '/* ---- the firmware: the phone\'s own modem partition (LUN 4, FAT16), read only ------------------------------- */\n',
         STORAGE.replace('static const char *fw_partition;\n', '') +
         '\n/* ---- the firmware: the phone\'s own modem partition (FAT16), read only ------------------------------- */\n')
# (fw_partition is defined after STORAGE in the Pro's text: declare it first)
t = once(t, '\n/* ---- storage, this tree\'s way', '\nstatic const char *fw_partition;\n\n/* ---- storage, this tree\'s way')
t = once(t, 'static const char *fw_partition = "modem_a";', 'static const char *fw_partition = "modem_a";')
# fat_mount: no GPT to walk, the partition is the device
t = span(t, '\tfor (unsigned tries = 0; instantnoodle_ufs_read(FW_LUN, FW_BLOCK, block, FW_BLOCK) != 0; tries++) {', 'found:\n',
         '''\t(void)count; (void)size; (void)at;
\tfor (unsigned tries = 0; cell_part_read(fw_partition, 0, block, FW_BLOCK) != 0; tries++) {
\t\tif (tries > 600) {
\t\t\tsay("modem: fw: the storage is not up (/dev/rufs_%s does not read)\\n", fw_partition);
\t\t\treturn false;
\t\t}
\t\tdelay_for_interval(100000, NSEC_PER_USEC);
\t}
\tfat.base = 0;
''')
# gpt_find: a partition by its name is a device of its own; lun 5's reads then go to it
t = span(t, '''gpt_find(uint8_t lun, const char *want, uint64_t *base, uint64_t *bytes)
{''', '''\tsay("modem: efs: no partition %s on LUN %u\\n", want, lun);
\treturn false;
}
''', '''gpt_find(uint8_t lun, const char *want, uint64_t *base, uint64_t *bytes)
{
\tstruct cell_part *p = cell_part(want);

\tif (p == NULL) {
\t\tsay("modem: efs: no /dev/rufs_%s (asked for as LUN %u)\\n", want, lun);
\t\treturn false;
\t}
\tcell_lun5_name = p->name;
\t*base = 0;
\t*bytes = p->bytes != 0 ? p->bytes : 2u * 1024u * 1024u;    /* (the Pro's notes: 2 MB each) */
\treturn true;
}
''')
# exported names: this tree has its own hk_modem_*
for old, new in (('hk_modem_dma_sync', 'hk_cell_dma_sync'), ('hk_modem_generation', 'hk_cell_generation'), ('hk_modem_map_contig', 'hk_cell_map_contig'),
                 ('hk_fw_partition_file', 'hk_cell_fw_partition_file'), ('hk_modem_qrtr_from_dsp', 'hk_cell_qrtr_from_dsp'),
                 ('hk_modem_sat_from_dsp', 'hk_cell_sat_from_dsp')):
    if old not in t:
        raise SystemExit('port.py: no ' + old)
    t = t.replace(old, new)
# the thread's body is run by our thread, after our warm reset
t = once(t, '''static void
modem_thread(void *arg, wait_result_t wr_unused)
{
\t(void)arg;
\t(void)wr_unused;
''', '''static void
cell_main(void)
{
''')
t = once(t, '''\t\tmodem_reset_state();
\t}
\tthread_terminate(current_thread());
}
''', '''\t\tmodem_reset_state();
\t}
}
''')
t = once(t, '/* Called while the system starts (bsd/dev/hk_input.c). */\nvoid\nhk_modem_init(void)\n{\n\tuint32_t n = 0, k = 0;\n\tthread_t thread;\n',
         '/* Called by our instantnoodle_modem.c\'s thread, after its look and its warm reset (boot-arg cell=1). It returns when the Pro\'s run is over. */\nvoid hk_cell_run(void);\nvoid\nhk_cell_run(void)\n{\n\tuint32_t n = 0, k = 0;\n')
t = span(t, '\tif (kernel_thread_start(modem_thread, NULL, &thread) != KERN_SUCCESS) {', '\tprintf("modem: steps up to %u, on a thread beside the system\'s start\\n", last_step);\n}\n',
         '\tprintf("modem: cell=1: the Pro\'s steps up to %u, on our modem thread\\n", last_step);\n\tcell_main();\n}\n' + QUIESCE)
t = once(t, 'extern kern_return_t thread_terminate(thread_t thread);\n', '')
# nothing of the Pro owner's goes out from this phone: their SIM PIN, and the test message to their number
t = once(t, 'if (sim.app_state == 2 && sim.pin1_state == 1 && sim.pin1_retries >= 3 && !pin_sent) {',
         'if (false && sim.app_state == 2 && sim.pin1_state == 1 && sim.pin1_retries >= 3 && !pin_sent) {       /* (the Pro owner\'s PIN: never sent on this phone) */')
# (found by its text, not by the number: the Pro owner's telephone number is not copied into this script)
t, found = re.subn(r'int r = wms_send_text\("\d+", "HackiOS test: [^"\n]*"\);',
                   "int r = 0;      /* (the Pro's test message went to its owner's number: not from this phone) */", t)
if found != 1:
    raise SystemExit('port.py: %d of the Pro\'s test message' % found)

# this phone's modem keeps no subscription of its own (card status: primary GW index 0xffff, the USIM application only "detected"): Android's RIL
# gives it one at every start with UIM Change Provisioning Session. The same here, at most three times a start; the status line shows the index.
t = once(t, """		static uint32_t last_sim;
		uint32_t now = sim.card_state | sim.app_state << 8 | sim.pin1_state << 16 | sim.pin1_retries << 24;
""", """		static uint64_t last_sim;
		uint64_t now = sim.card_state | sim.app_state << 8 | sim.pin1_state << 16 | (uint64_t)sim.pin1_retries << 24 | (uint64_t)sim.gw_index << 32;
""")
t = once(t, """		} else if (sim.known) {
			if (false && sim.app_state == 2""", """		} else if (sim.known) {
			static unsigned provision_tries;

			if (sim.card_state == 1 && sim.app_type == 2 && sim.gw_index == 0xffff && sim.aid_len >= 5 && (sim.app_state == 1 || sim.app_state == 7) && provision_tries < 3) {
				/* UIM Change Provisioning Session: session type 0 (primary GW) activated with the card's USIM application (slot, its identifier) */
				uint8_t pt[40], app[2 + sizeof(sim.aid)];
				uint32_t pk = 0;
				static const uint8_t session[2] = { 0, 1 };

				app[0] = sim.slot;
				app[1] = sim.aid_len;
				memcpy(app + 2, sim.aid, sim.aid_len);
				tlv_put(pt, &pk, 0x01, session, 2);
				tlv_put(pt, &pk, 0x10, app, 2u + sim.aid_len);
				e = qmi_call(11, 0x0038, pt, pk, 5000);
				provision_tries++;
				say("modem: sim: the USIM application asked to be the primary subscription (try %u): %s (result %u error %u)\\n", provision_tries, e == 0 ? "answered" : "NO REPLY",
				    qmi_reply.result, qmi_reply.error);
			}
			if (false && sim.app_state == 2""")

# cell=1 alone is the working set that the 8 Pro's line spells out argument by argument (the loader's line, 607 bytes, has no room for it
# beside the daily one): an argument that is on the line still wins.
a_run = t.index('void\nhk_cell_run(void)\n{')
b_run = t.index('\n}\n', a_run)
body = t[a_run:b_run]
assert body.count('PE_parse_boot_argn(') > 30
body = body.replace('PE_parse_boot_argn(', 'cell_arg(')
body = once(body, 'if (!cell_arg("modem", &n, sizeof(n)) || n == 0) {', 'if (!PE_parse_boot_argn("modem", &n, sizeof(n)) || n == 0) {')
t = t[:a_run] + body + t[b_run:]
t = once(t, 'void hk_cell_run(void);\n', """static boolean_t
cell_arg(const char *name, void *value, int size)
{
	static const struct { const char *name; uint32_t value; } defaults[] = {
		{ "modem_quiet", 3 }, { "modem_chans", 2187 }, { "modem_bringup", 1 }, { "modem_tftp", 1 }, { "modem_blk", 1024 }, { "modem_efs", 1 },
		{ "modem_service", 1 }, { "modem_data", 1 }, { "modem_qrtr", 31536000 },
	};

	if (PE_parse_boot_argn(name, value, size)) {
		return TRUE;
	}
	for (unsigned i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		if (size == (int)sizeof(uint32_t) && strcmp(name, defaults[i].name) == 0) {
			memcpy(value, &defaults[i].value, sizeof(uint32_t));
			return TRUE;
		}
	}
	return FALSE;
}

void hk_cell_run(void);
""")

# iOS gets the packets of mux 1 (its IPv4 session) and mux 2 (an IPv6 one) only: a session started for the modem's own use on another mux id
# (the IMS one, mux 3, from user space) must not reach en1, or its router advertisements would give iOS addresses of a network it cannot use.
t = once(t, """				} else if (size > pad) {
					data_receive(d + o + 4, size - pad);
				}""", """				} else if (size > pad && d[o + 1] > CL6_MUX_MAX) {
					static unsigned others;

					if (others++ < 4) {
						say("modem: data: a packet of mux %u (%u bytes) is not iOS's: dropped\\n", d[o + 1], size - pad);
					}
				} else if (size > pad) {
					data_receive(d + o + 4, size - pad);
				}""")
t = once(t, "static void\ndata_poll(void)\n", "#define CL6_MUX_MAX 2       /* (CL6_MUX, defined further down) */\nstatic void\ndata_poll(void)\n")

# the modem's messages: the radio layers' own chatter (hundreds of lines a second once LTE is up) used up the 9000 lines that are printed before
# anything of IMS was said: left out, and the limit raised
t = once(t, '"TFTP_STAT", "wms.c:" };', '"TFTP_STAT", "wms.c:",\n\t\t"lte_LL1", "lte_ml1", "nr5g_ml1", "nr5g_ll1", "nrfw_", "wfw_", "lte_rrc_stm", "ipa_mhi_bw_vote", "ps_sys_conf", "rfcommon_efs", "rflm_", "rfnr_", "rflte_", "rfc_", "intf_dl", "intf_ul",\n\t\t"lte_mac", "lte_rlc", "lte_pdcp", "nr5g_mac", "nr5g_rlc", "nr5g_pdcp", "a2_", "ipa_hal", "ipa_dpl", "remotefs_sio", "ds_andsf", "ds_ursp", "ds_mppm", "hdrsrch", "srch", "stm.c", "rr_gprs_debug", "tle_", "loc_", "gnss_", "mcs_", "cxm_", "trm_", "wwan_coex", "sleepctl", "npa_", "rcinit_" };')
t = once(t, "if (diag_state.printed_lines < 9000 && !diag_noise(line)) {", "if (diag_state.printed_lines < 60000 && !diag_noise(line)) {")

# The modem's own files (its EFS: what it writes back over the EFS channel, Qualcomm's "ks -m" on Android, which stores each region in the
# partition its file name says: m9kefs1 -> mdm1m9kefs1). Nothing is written to the phone here, so what the modem saved was gone at its next
# start: a carrier configuration it had selected for the SIM stayed "pending" for ever, and with it IMS (VoLTE) had no settings (2026-11-10).
# Kept in memory instead: the latest copy of each file, given back (Sahara ids 16, 17, 20) when the modem is started again in this run.
t = once(t, "/* an image's bytes [offset, offset + length), to the device in pieces of what a transfer element holds */\n", """static struct {
	uint8_t *data;
	uint64_t length;
	uint32_t syncs;
} efs_kept[3];          /* m9kefs1, m9kefs2, m9kefs3 */
static char efs_file[16][21];

static int
efs_kept_index(uint32_t id)
{
	return id == 16 ? 0 : id == 17 ? 1 : id == 20 ? 2 : -1;
}

/* an image's bytes [offset, offset + length), to the device in pieces of what a transfer element holds */
""")
t = once(t, """					memcpy(efs.name[i], e + 24, 20);
				} else {""", """					memcpy(efs.name[i], e + 24, 20);
					memcpy(efs_file[i], e + 44, 20);
				} else {""")
t = once(t, """					memcpy(efs.name[i], e + 12, 20);
				}
				efs.name[i][20] = 0;
				say("modem: efs: region %u: \\"%s\\" at %#llx, %llu bytes\\n", i, efs.name[i], efs.base[i], efs.length[i]);""", """					memcpy(efs.name[i], e + 12, 20);
					memcpy(efs_file[i], e + 32, 20);
				}
				efs.name[i][20] = 0;
				efs_file[i][20] = 0;
				say("modem: efs: region %u: \\"%s\\", file \\"%s\\", at %#llx, %llu bytes\\n", i, efs.name[i], efs_file[i], efs.base[i], efs.length[i]);""")
t = once(t, """		efs.state = EFS_TABLE;
		efs_read(address, efs.table_length);
		break;""", """		efs.state = EFS_TABLE;
		efs.dump_length = 0;            /* (each sync from the buffer's start: its regions are kept by their place in it) */
		efs_read(address, efs.table_length);
		break;""")
t = once(t, """		say("modem: efs: memory debug done: %u regions read, thrown away\\n", efs.regions);""", """		{
			uint64_t at = 0;

			for (uint32_t i = 0; i < efs.regions && efs.dump != NULL; i++) {
				size_t n = strlen(efs_file[i]);
				int k = n >= 4 && strcmp(efs_file[i] + n - 4, "efs1") == 0 ? 0 : n >= 4 && strcmp(efs_file[i] + n - 4, "efs2") == 0 ? 1 :
				    n >= 4 && strcmp(efs_file[i] + n - 4, "efs3") == 0 ? 2 : -1;

				if (k >= 0 && efs.length[i] > 0 && efs.length[i] <= (4u << 20) && at + efs.length[i] <= efs.dump_length) {
					if (efs_kept[k].data == NULL) {
						efs_kept[k].data = (uint8_t *)kalloc_data(4u << 20, Z_WAITOK | Z_ZERO);
					}
					if (efs_kept[k].data != NULL) {
						memcpy(efs_kept[k].data, efs.dump + at, (size_t)efs.length[i]);
						efs_kept[k].length = efs.length[i];
						efs_kept[k].syncs++;
						say("modem: efs: the modem's file \\"%s\\" (%llu bytes) is kept in memory (its save number %u): its next start in this run gets it, the phone's partition is not written\\n",
						    efs_file[i], efs.length[i], efs_kept[k].syncs);
					}
				}
				at += efs.length[i];
			}
		}
		say("modem: efs: memory debug done: %u regions read, kept in memory only\\n", efs.regions);""")
t = once(t, """		if (!gpt_find(5, names[id - 16], &base, &bytes)) {
			sahara.done = true;
			return;
		}""", """		if (efs_kept_index(id) >= 0 && efs_kept[efs_kept_index(id)].data != NULL && efs_kept[efs_kept_index(id)].length > 0) {
			/* what the modem itself saved last in this run, in place of the partition */
			const uint8_t *kept = efs_kept[efs_kept_index(id)].data;
			uint64_t kept_bytes = efs_kept[efs_kept_index(id)].length;

			say("modem: sahara: image %u = the modem's own last save of %s, kept in memory (%llu bytes): %llu bytes from %llu\\n", id, names[id - 16], kept_bytes, length, offset);
			while (length > 0) {
				uint64_t take = length < sizeof(part_chunk) ? length : sizeof(part_chunk);
				uint64_t have = offset >= kept_bytes ? 0 : (kept_bytes - offset < take ? kept_bytes - offset : take);

				memset(part_chunk, 0, (size_t)take);
				memcpy(part_chunk, kept + offset, (size_t)have);
				if (sahara_send(part_chunk, (size_t)take) != 0) {
					say("modem: sahara: sending %llu bytes of the kept file failed\\n", take);
					sahara.done = true;
					return;
				}
				offset += take;
				length -= take;
				sahara.bytes_lo += (uint32_t)take;
				if (sahara.bytes_lo < take) {
					sahara.bytes_hi++;
				}
			}
			return;
		}
		if (!gpt_find(5, names[id - 16], &base, &bytes)) {
			sahara.done = true;
			return;
		}""")

# The modem's terse messages (DIAG 0x99: a hash of the text and its arguments; the texts are in the firmware's qdsp6m.qdb, read on the computer)
# and a way to send it a DIAG request by hand (kern.hackios.modem_diag, hex bytes): with it the message masks are narrowed to the subsystems
# wanted, and from the first such request on the terse messages are printed ("q4: HASH NN ARGS") instead of dropped.
t = once(t, """				if (diag_state.frame[0] != 0x79 && diag_state.frames < 12) {""", """				if (diag_terse_on && diag_state.frame[0] == 0x99 && diag_state.frame_length >= 20) {
					char hex[2 * 40 + 1];
					uint32_t have = diag_state.frame_length - 2 - 18;

					hex[0] = 0;
					for (uint32_t k = 0; k < have && k < 40; k++) {
						snprintf(hex + 2 * k, 3, "%02x", diag_state.frame[18 + k]);
					}
					say("modem: q4: %02x%02x%02x%02x %02x %s\\n", diag_state.frame[15], diag_state.frame[14], diag_state.frame[13], diag_state.frame[12], diag_state.frame[2], hex);
				} else if (diag_terse_on && diag_state.frame[0] != 0x79 && diag_state.frame[0] != 0x99) {
					say("modem: diag: answer of %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\\n", diag_state.frame_length - 2, diag_state.frame[0], diag_state.frame[1], diag_state.frame[2],
					    diag_state.frame[3], diag_state.frame[4], diag_state.frame[5], diag_state.frame[6], diag_state.frame[7]);
				}
				if (diag_state.frame[0] != 0x79 && diag_state.frames < 12) {""")
t = once(t, "static void\ndiag_packet(const uint8_t *d, uint32_t length)\n", "static bool diag_terse_on;\nstatic char diag_by_hand[200];\nstatic volatile bool diag_by_hand_pending;\n\nstatic void\ndiag_packet(const uint8_t *d, uint32_t length)\n")
t = once(t, "	if (cell.qmi_pending) {\n		char *p = cell.qmi_request, *end;\n", """	if (diag_by_hand_pending) {
		uint8_t request[90];
		uint32_t n = 0;

		for (const char *h = diag_by_hand; h[0] != 0 && h[1] != 0 && n < sizeof(request); h += 2) {
			int hi = h[0] <= '9' ? h[0] - '0' : (h[0] | 0x20) - 'a' + 10, lo = h[1] <= '9' ? h[1] - '0' : (h[1] | 0x20) - 'a' + 10;

			request[n++] = (uint8_t)(hi << 4 | lo);
		}
		diag_terse_on = true;
		say("modem: diag: a request of %u bytes by hand (%02x %02x ...): %d; terse messages are printed from now on\\n", n, request[0], n > 1 ? request[1] : 0, n > 0 ? diag_request(request, n) : -1);
		diag_by_hand_pending = false;
	}
	if (cell.qmi_pending) {
		char *p = cell.qmi_request, *end;
""")
t = once(t, 'SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_qmi,', """static int
cell_diag_sysctl(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[sizeof(diag_by_hand)];
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, "", 1);
	}
	if (req->newlen == 0 || req->newlen >= sizeof(text) || diag_by_hand_pending) {
		return diag_by_hand_pending ? EBUSY : EINVAL;
	}
	bzero(text, sizeof(text));
	if ((error = SYSCTL_IN(req, text, req->newlen)) != 0) {
		return error;
	}
	strlcpy(diag_by_hand, text, sizeof(diag_by_hand));
	diag_by_hand_pending = true;
	return 0;
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_diag, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, cell_diag_sysctl, "A", "a DIAG request by hand: hex bytes (framed here); terse messages are printed from then on");
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_qmi,""")

# the DIAG requests made by hand are made again at every later start of the modem (after the kernel's own "all masks"): what a subsystem says
# while it starts is then seen with the masks already narrowed
t = once(t, "static bool diag_terse_on;\n", "static bool diag_terse_on;\nstatic uint8_t diag_again[10][90];\nstatic uint32_t diag_again_length[10], diag_again_count;\n")
t = once(t, """		diag_terse_on = true;
		say("modem: diag: a request of %u bytes by hand""", """		if (n > 0 && diag_again_count < 10) {
			memcpy(diag_again[diag_again_count], request, n);
			diag_again_length[diag_again_count++] = n;
		}
		diag_terse_on = true;
		say("modem: diag: a request of %u bytes by hand""")
t = once(t, """	say("modem: diag: version request %d, all message masks %d\\n", diag_request(version, 1), diag_request(all_masks, 8));
	diag_state.on = true;""", """	say("modem: diag: version request %d, all message masks %d\\n", diag_request(version, 1), diag_request(all_masks, 8));
	for (uint32_t i = 0; i < diag_again_count; i++) {
		say("modem: diag: the request made by hand before (%02x %02x ..., %u bytes) made again: %d\\n", diag_again[i][0], diag_again[i][1], diag_again_length[i],
		    diag_request(diag_again[i], diag_again_length[i]));
	}
	diag_state.on = true;""")

# a packet of one subscription comes wrapped (0x98, version, 2 bytes, 4 bytes of identity, then the packet): most of what the data and IMS tasks say. Unwrapped
# here, then read as any other
t = once(t, """				diag_message(diag_state.frame, diag_state.frame_length - 2);
				if (diag_terse_on && diag_state.frame[0] == 0x99 && diag_state.frame_length >= 20) {
					char hex[2 * 40 + 1];
					uint32_t have = diag_state.frame_length - 2 - 18;
""", """				const uint8_t *f = diag_state.frame;
				uint32_t n = diag_state.frame_length - 2;

				if (f[0] == 0x98 && n > 12) {
					f += 8;
					n -= 8;
				}
				diag_message(f, n);
				if (diag_terse_on && f[0] == 0x99 && n >= 18) {
					char hex[2 * 40 + 1];
					uint32_t have = n - 18;
""")
t = once(t, """						snprintf(hex + 2 * k, 3, "%02x", diag_state.frame[18 + k]);""", """						snprintf(hex + 2 * k, 3, "%02x", f[18 + k]);""")
t = once(t, """					say("modem: q4: %02x%02x%02x%02x %02x %s\\n", diag_state.frame[15], diag_state.frame[14], diag_state.frame[13], diag_state.frame[12], diag_state.frame[2], hex);
				} else if (diag_terse_on && diag_state.frame[0] != 0x79 && diag_state.frame[0] != 0x99) {
					say("modem: diag: answer of %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\\n", diag_state.frame_length - 2, diag_state.frame[0], diag_state.frame[1], diag_state.frame[2],
					    diag_state.frame[3], diag_state.frame[4], diag_state.frame[5], diag_state.frame[6], diag_state.frame[7]);""", """					say("modem: q4: %02x%02x%02x%02x %02x %s\\n", f[15], f[14], f[13], f[12], f[2], hex);
				} else if (diag_terse_on && f[0] != 0x79 && f[0] != 0x99) {
					say("modem: diag: answer of %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\\n", n, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);""")

# the indication that answers a request made by hand (PDC's lists and descriptions come this way) is printed whole, 64 bytes a line
t = once(t, "static uint32_t qmi_src_port = QMI_PORT;       /* the port the next request goes out from */\n",
         "static uint32_t qmi_src_port = QMI_PORT;       /* the port the next request goes out from */\nstatic uint32_t qmi_hand_message = ~0u;          /* the message of the last request made by hand: its indication is printed whole */\n")
t = once(t, """				say("modem: qmi: INDICATION %#06x from node %u port %u (%u bytes): %s\\n", q[3] | q[4] << 8, h.from_node, h.from_port, tl, hex);
			}
""", """				say("modem: qmi: INDICATION %#06x from node %u port %u (%u bytes): %s\\n", q[3] | q[4] << 8, h.from_node, h.from_port, tl, hex);
			}
			if ((uint32_t)(q[3] | q[4] << 8) == qmi_hand_message) {
				for (uint32_t at2 = 0; at2 < tl && at2 < 2048; at2 += 64) {
					char row[2 * 64 + 1];
					uint32_t m = tl - at2 < 64 ? tl - at2 : 64;

					for (uint32_t i = 0; i < m; i++) {
						snprintf(row + 2 * i, 3, "%02x", q[7 + at2 + i]);
					}
					say("modem: qmi: by hand: indication %#06x at %u: %s\\n", q[3] | q[4] << 8, at2, row);
				}
			}
""")
t = once(t, """		say("modem: qmi: hand-made request: service %u, message %#x, client port %#x, %u TLV bytes\\n", service, message, qmi_src_port, n);
""", """		say("modem: qmi: hand-made request: service %u, message %#x, client port %#x, %u TLV bytes\\n", service, message, qmi_src_port, n);
		qmi_hand_message = message;
""")

# a log mask is long (one bit a log code: 263 bytes for IMS's): requests by hand of up to 720 bytes; and the log packets (0x10) and the events (0x60) that
# then come are printed whole, 64 bytes a line, for a decoder on the host
t = once(t, "diag_request(const uint8_t *payload, uint32_t n)\n{\n\tuint8_t out[96];\n", "diag_request(const uint8_t *payload, uint32_t n)\n{\n\tstatic uint8_t out[2 * 722 + 4];        /* (the modem's thread only) */\n")
t = once(t, "static uint8_t diag_again[10][90];\n", "static uint8_t diag_again[10][720];\n")
t = once(t, "static char diag_by_hand[200];\n", "static char diag_by_hand[1500];\n")
t = once(t, "\tif (diag_by_hand_pending) {\n\t\tuint8_t request[90];\n", "\tif (diag_by_hand_pending) {\n\t\tstatic uint8_t request[720];\n")
t = once(t, """				} else if (diag_terse_on && f[0] != 0x79 && f[0] != 0x99) {
""", """				} else if (diag_terse_on && (f[0] == 0x10 || f[0] == 0x60)) {
					static uint32_t whole;

					for (uint32_t at2 = 0; at2 < n && at2 < 1536 && whole < 200000; at2 += 64, whole++) {
						char row[2 * 64 + 1];
						uint32_t m = n - at2 < 64 ? n - at2 : 64;

						for (uint32_t k = 0; k < m; k++) {
							snprintf(row + 2 * k, 3, "%02x", f[at2 + k]);
						}
						say("modem: %s: %u at %u: %s\\n", f[0] == 0x10 ? "lg" : "ev", n, at2, row);
					}
				} else if (diag_terse_on && f[0] != 0x79 && f[0] != 0x99) {
""")

# the lines that carry the subscriber's number or the addresses the carrier gave are not printed (IMS's "MSISDN_obtained", the data stack's address lines);
# and the count that caps the whole-packet rows starts again at each request by hand (a wide log mask used it up in seconds)
t = once(t, """	static const char *const words[] = { "UIM_PUP_MSG", """, """	static const char *const words[] = { "MSISDN", "IMPU", "IMPI", "IMSI", "IPV4 address", "IPV4 Address", "DNS addr", "Gateway Addr", "UIM_PUP_MSG", """)
t = once(t, """					static uint32_t whole;

""", """
""")
t = once(t, "static bool diag_terse_on;\n", "static bool diag_terse_on;\nstatic uint32_t whole;          /* rows of whole packets printed since the last request by hand */\n")
t = once(t, """		diag_terse_on = true;
		say("modem: diag: a request of %u bytes by hand""", """		diag_terse_on = true;
		whole = 0;
		say("modem: diag: a request of %u bytes by hand""")

# cell_dcm=1: the AP's "IMS data" QMI service (0x302). Android's imsdatadaemon serves it and the modem's IMS connection manager is its client (public:
# codeberg.org/DylanVanAssche/imsd, IMS-QUALCOMM.md; our modem offers no such service itself). Announced to the modem; what the modem asks is printed and
# answered with the bytes that write-up records; kern.hackios.modem_dcm sends a message by hand to that client (an indication, while its form is worked out)
t = once(t, "static bool option_ipa;", "static bool option_dcm;                 /* cell_dcm=1: our \"IMS data\" QMI service (0x302) is announced to the modem and its requests answered */\nstatic bool option_ipa;")
t = once(t, "static void\nqrtr_packet(const uint8_t *d, uint32_t length, uint32_t dl_channel)\n", """#define DCM_SERVICE             0x302u
#define DCM_PORT                0x4006u
static uint32_t dcm_client_node, dcm_client_port;
static char dcm_by_hand[700];
static volatile bool dcm_by_hand_pending;

/* the value of one TLV of a QMI message's body, or NULL */
static const uint8_t *
dcm_tlv(const uint8_t *b, uint32_t n, uint8_t type, uint32_t *length)
{
	for (uint32_t at = 0; at + 3 <= n;) {
		uint32_t l = b[at + 1] | b[at + 2] << 8;

		if (at + 3 + l > n) {
			break;
		}
		if (b[at] == type) {
			*length = l;
			return b + at + 3;
		}
		at += 3 + l;
	}
	return NULL;
}

static void
dcm_request(uint32_t from_node, uint32_t from_port, const uint8_t *q, uint32_t length)
{
	uint16_t message = (uint16_t)(q[3] | q[4] << 8);
	uint32_t tl = q[5] | q[6] << 8, n = 0, l = 0;
	static const uint8_t ok[7] = { 0x02, 0x04, 0x00, 0, 0, 0, 0 };
	const uint8_t *v;
	uint8_t r[48];

	if (7 + tl > length) {
		tl = length - 7;
	}
	dcm_client_node = from_node;
	dcm_client_port = from_port;
	for (uint32_t at = 0; at < 7 + tl && at < 512; at += 64) {
		char row[2 * 64 + 1];
		uint32_t m = 7 + tl - at < 64 ? 7 + tl - at : 64;

		for (uint32_t i = 0; i < m; i++) {
			snprintf(row + 2 * i, 3, "%02x", q[at + i]);
		}
		say("modem: dcm: from node %u port %u: type %u message %#06x, %u bytes, at %u: %s\\n", from_node, from_port, q[0], message, 7 + tl, at, row);
	}
	if (q[0] != 0) {
		return;         /* (not a request: nothing to answer) */
	}
	r[n++] = 2;
	r[n++] = q[1];
	r[n++] = q[2];
	r[n++] = q[3];
	r[n++] = q[4];
	n += 2;
	memcpy(r + n, ok, 7);
	n += 7;
	if (message == 0x2e) {
		static const uint8_t more[7] = { 0x10, 0x04, 0x00, 0x5f, 0, 0, 0 };

		memcpy(r + n, more, 7);
		n += 7;
	} else if (message == 0x20) {
		/* (the write-up's answer: TLV 0x10 one byte 0x14, 0x11 the request's 0x10 back, 0x12 one) */
		uint8_t more[18] = { 0x10, 0x01, 0x00, 0x14, 0x11, 0x04, 0x00, 0x64, 0, 0, 0, 0x12, 0x04, 0x00, 0x01, 0, 0, 0 };

		if ((v = dcm_tlv(q + 7, tl, 0x10, &l)) != NULL && l == 4) {
			memcpy(more + 7, v, 4);
		}
		memcpy(r + n, more, 18);
		n += 18;
	}
	r[5] = (uint8_t)(n - 7);
	r[6] = 0;
	say("modem: dcm: message %#06x answered as Android's daemon does (%u bytes): %d\\n", message, n, qrtr_send(QRTR_DATA, DCM_PORT, from_node, from_port, r, n));
}

static void
qrtr_packet(const uint8_t *d, uint32_t length, uint32_t dl_channel)
""")
t = once(t, """			if (option_tftp) {
				for (uint32_t instance = 0; instance < 11; instance++) {
					uint32_t serv[5] = { QRTR_NEW_SERVER, TFTP_SERVICE, instance < 5 ? 1u | instance << 8 : instance - 5, QRTR_US, TFTP_PORT };
""", """			if (option_dcm) {
				for (uint32_t instance = 0; instance < 2; instance++) {
					uint32_t serv[5] = { QRTR_NEW_SERVER, DCM_SERVICE, 1u | instance << 8, QRTR_US, DCM_PORT };

					say("modem: qrtr: our IMS data service (0x302, version 1 instance %u, port %#x) announced to node %u: %d\\n", instance, DCM_PORT, h.from_node,
					    qrtr_send(QRTR_NEW_SERVER, QRTR_CONTROL, h.from_node, QRTR_CONTROL, serv, 20));
				}
			}
			if (option_tftp) {
				for (uint32_t instance = 0; instance < 11; instance++) {
					uint32_t serv[5] = { QRTR_NEW_SERVER, TFTP_SERVICE, instance < 5 ? 1u | instance << 8 : instance - 5, QRTR_US, TFTP_PORT };
""")
t = once(t, """			if (option_ipa && (w[1] == 0 || w[1] == 0x31)) {
				qrtr_send(QRTR_NEW_SERVER, QRTR_CONTROL, h.from_node, QRTR_CONTROL, serv, 20);
			}
""", """			if (option_ipa && (w[1] == 0 || w[1] == 0x31)) {
				qrtr_send(QRTR_NEW_SERVER, QRTR_CONTROL, h.from_node, QRTR_CONTROL, serv, 20);
			}
			if (option_dcm && (w[1] == 0 || w[1] == DCM_SERVICE)) {
				for (uint32_t instance = 0; instance < 2; instance++) {
					uint32_t dcm[5] = { QRTR_NEW_SERVER, DCM_SERVICE, 1u | instance << 8, QRTR_US, DCM_PORT };

					qrtr_send(QRTR_NEW_SERVER, QRTR_CONTROL, h.from_node, QRTR_CONTROL, dcm, 20);
				}
			}
""")
t = once(t, """	if (h.type == QRTR_DATA && h.to_port == IPA_SERVER_PORT && h.size >= 7) {
""", """	if (h.type == QRTR_DATA && h.to_port == DCM_PORT && h.size >= 7) {
		dcm_request(h.from_node, h.from_port, d + at, h.size);
		return;
	}
	if (h.type == QRTR_DATA && h.to_port == IPA_SERVER_PORT && h.size >= 7) {
""")
t = once(t, """		cell_arg("modem_ipa", &k, sizeof(k));
		option_ipa = k == 1;
""", """		cell_arg("modem_ipa", &k, sizeof(k));
		option_ipa = k == 1;
		k = 1;
		cell_arg("cell_dcm", &k, sizeof(k));
		option_dcm = k == 1;
""")
t = once(t, """	if (diag_by_hand_pending) {
		static uint8_t request[720];
""", """	if (dcm_by_hand_pending) {
		static uint8_t m[340];
		uint32_t n = 0;

		for (const char *h = dcm_by_hand; h[0] != 0 && h[1] != 0 && n < sizeof(m); h += 2) {
			int hi = h[0] <= '9' ? h[0] - '0' : (h[0] | 0x20) - 'a' + 10, lo = h[1] <= '9' ? h[1] - '0' : (h[1] | 0x20) - 'a' + 10;

			m[n++] = (uint8_t)(hi << 4 | lo);
		}
		say("modem: dcm: %u bytes by hand (type %u message %#04x%02x) to node %u port %u: %d\\n", n, m[0], m[4], m[3], dcm_client_node, dcm_client_port,
		    n >= 7 && dcm_client_port != 0 ? qrtr_send(QRTR_DATA, DCM_PORT, dcm_client_node, dcm_client_port, m, n) : -1);
		dcm_by_hand_pending = false;
	}
	if (diag_by_hand_pending) {
		static uint8_t request[720];
""")
t = once(t, "static int\ncell_diag_sysctl(", """static int
cell_dcm_sysctl(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[sizeof(dcm_by_hand)];
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, "", 1);
	}
	if (req->newlen == 0 || req->newlen >= sizeof(text) || dcm_by_hand_pending) {
		return dcm_by_hand_pending ? EBUSY : EINVAL;
	}
	bzero(text, sizeof(text));
	if ((error = SYSCTL_IN(req, text, req->newlen)) != 0) {
		return error;
	}
	if (strncmp(text, "again", 5) == 0) {
		dcm_resend_pending = true;      /* the activation indication once more, with the address the kernel holds */
		return 0;
	}
	strlcpy(dcm_by_hand, text, sizeof(dcm_by_hand));
	dcm_by_hand_pending = true;
	return 0;
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_dcm, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, cell_dcm_sysctl, "A", "a QMI message by hand (hex: type, transaction, message, length, TLVs) from our IMS data service to the modem's client");

static int
cell_diag_sysctl(""")
t = once(t, """	static const char *const words[] = { "MSISDN", "IMPU", "IMPI", "IMSI", """, """	static const char *const words[] = { "ICCID", "PAURI", "EHPLMN", "MSISDN", "IMPU", "IMPI", "IMSI", """)

# the IMS data service does its part itself: at PDP_ACTIVATE_REQ (0x20: APN, type, RAT, family, profile; TLV 0x10 a sequence number) it answers, then the
# modem's thread starts that profile's network on a QMI client of its own (port 0x4007, mux 3: its packets are the modem's, none come to us), reads the
# address and sends PDP_ACTIVATE_IND (0x20 as an indication: TLV 0x01 the PDP's number, 0x02 result, 0x10 the sequence number, 0x11 family and the address
# as text). GET_IP_ADDRESS (0x22) is answered and indicated the same way. The address stays in the kernel's memory: it is never printed.
# (interface: QMI IMS DCM, the public ip_multimedia_subsystem_dcm_v01.h; the bytes of Android's answers: the imsd write-up)
t = once(t, "#define QMI_PORT6               0x4003u", "#define QMI_PORT_IMS            0x4007u         /* the QMI client that holds the IMS network for the modem's IMS (cell_dcm=1) */\n#define QMI_PORT6               0x4003u")
t = once(t, "	if (h.type == QRTR_DATA && (h.to_port == QMI_PORT || h.to_port == QMI_PORT6) && h.size >= 7) {", "	if (h.type == QRTR_DATA && (h.to_port == QMI_PORT || h.to_port == QMI_PORT6 || h.to_port == QMI_PORT_IMS) && h.size >= 7) {")
t = once(t, "static char dcm_by_hand[700];\n", """static char dcm_by_hand[700];
#define DCM_PDP_ID              0x14u           /* (the number Android's daemon gave its first) */
static volatile bool dcm_activate_pending, dcm_address_pending, dcm_resend_pending;
static uint32_t dcm_sequence, dcm_profile, dcm_family, dcm_requests, dcm_word12 = 1;     /* (the request's TLV 0x12, sent back in the answer and in the indication) */
static char dcm_address[48];                    /* the IMS network's address as text, for the modem's IMS only: never printed */
""")
t = once(t, """		if ((v = dcm_tlv(q + 7, tl, 0x10, &l)) != NULL && l == 4) {
			memcpy(more + 7, v, 4);
		}
		memcpy(r + n, more, 18);
		n += 18;
	}
""", """		if ((v = dcm_tlv(q + 7, tl, 0x10, &l)) != NULL && l == 4) {
			memcpy(more + 7, v, 4);
			memcpy(&dcm_sequence, v, 4);
		}
		if ((v = dcm_tlv(q + 7, tl, 0x12, &l)) != NULL && l == 4) {
			memcpy(more + 14, v, 4);
			memcpy(&dcm_word12, v, 4);
		}
		more[3] = DCM_PDP_ID;
		memcpy(r + n, more, 18);
		n += 18;
		/* PDP information: the APN (a length byte and its letters), then four 32-bit words: APN type, RAT, family (0 IPv4, 1 IPv6), profile */
		dcm_family = 0;
		dcm_profile = 0;
		if ((v = dcm_tlv(q + 7, tl, 0x01, &l)) != NULL && l >= 1u + v[0] + 16) {
			const uint8_t *w = v + 1 + v[0];

			dcm_family = w[8];
			dcm_profile = w[12] | w[13] << 8;
			say("modem: dcm: the modem's IMS asks for its network: APN of %u letters, type %u, RAT %u, family %u, profile %u, sequence %u\\n", v[0], w[0], w[4], dcm_family, dcm_profile, dcm_sequence);
		}
		dcm_requests++;
		dcm_activate_pending = true;
	} else if (message == 0x21) {
		/* PDP_DEACTIVATE_REQ: answered with the PDP's number (the network itself is left as it is: a later request finds it up) */
		static const uint8_t more[4] = { 0x10, 0x01, 0x00, DCM_PDP_ID };

		memcpy(r + n, more, 4);
		n += 4;
	} else if (message == 0x22) {
		dcm_address_pending = true;         /* GET_IP_ADDRESS: answered here, the address follows as an indication */
	}
""")
t = once(t, """	if (dcm_by_hand_pending) {
		static uint8_t m[340];
""", """	if (dcm_activate_pending || dcm_address_pending || dcm_resend_pending) {
		static const uint8_t bind[15] = { 0x10, 0x08, 0x00, 0x03, 0, 0, 0, 0x04, 0, 0, 0, 0x11, 0x01, 0x00, 0x03 };
		static const uint8_t settings[7] = { 0x10, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00 };
		bool resend = dcm_resend_pending && !dcm_activate_pending, activate = dcm_activate_pending || resend;
		uint8_t family[4] = { 0x01, 0x01, 0x00, dcm_family == 1 ? 0x06 : 0x04 };
		uint8_t start[8] = { 0x31, 0x01, 0x00, (uint8_t)dcm_profile, 0x19, 0x01, 0x00, dcm_family == 1 ? 0x06 : 0x04 };
		uint8_t ind[96];
		uint32_t n = 0, l = 0, al;
		const uint8_t *c;
		int e1 = 0, e2 = 0, e3 = 0, e4, r3 = 0;

		dcm_activate_pending = false;
		dcm_address_pending = false;
		dcm_resend_pending = false;
		qmi_src_port = QMI_PORT_IMS;
		if (activate && !resend) {
			e1 = qmi_call(1, 0xa2, bind, sizeof(bind), 5000);
			e2 = qmi_call(1, 0x4d, family, sizeof(family), 5000);
			e3 = qmi_call(1, 0x20, start, sizeof(start), 30000);
			r3 = e3 == 0 ? (int)qmi_reply.result : -1;
			dcm_address[0] = 0;
		}
		e4 = qmi_call(1, 0x2d, settings, sizeof(settings), 5000);
		if (e4 == 0 && qmi_reply.result == 0 && dcm_family != 1 && (c = qmi_tlv(0x1e, &l)) != NULL && l == 4) {
			snprintf(dcm_address, sizeof(dcm_address), "%u.%u.%u.%u", c[3], c[2], c[1], c[0]);
		}
		qmi_src_port = QMI_PORT;
		al = (uint32_t)strlen(dcm_address);
		ind[n++] = 4;
		ind[n++] = 0;                 /* (an indication: transaction 0) */
		ind[n++] = 0;
		ind[n++] = activate ? 0x20 : 0x22;
		ind[n++] = 0;
		n += 2;
		ind[n++] = 0x01;
		ind[n++] = 1;
		ind[n++] = 0;
		ind[n++] = DCM_PDP_ID;
		if (activate) {
			ind[n++] = 0x02;                /* the PDP's result: success, or failure with "call failed" (14) */
			ind[n++] = 4;
			ind[n++] = 0;
			ind[n++] = al != 0 ? 0 : 1;
			ind[n++] = 0;
			ind[n++] = al != 0 ? 0 : 14;
			ind[n++] = 0;
			ind[n++] = 0x10;
			ind[n++] = 4;
			ind[n++] = 0;
			memcpy(ind + n, &dcm_sequence, 4);
			n += 4;
		}
		if (al != 0) {
			ind[n++] = activate ? 0x11 : 0x10;
			ind[n++] = (uint8_t)(5 + al);
			ind[n++] = 0;
			ind[n++] = (uint8_t)(dcm_family == 1);
			ind[n++] = 0;
			ind[n++] = 0;
			ind[n++] = 0;
			ind[n++] = (uint8_t)al;
			memcpy(ind + n, dcm_address, al);
			n += al;
		}
		if (activate) {
			/* (measured: without this TLV the modem's IMS drops the indication silently; with it it goes on to register) */
			ind[n++] = 0x12;
			ind[n++] = 4;
			ind[n++] = 0;
			memcpy(ind + n, &dcm_word12, 4);
			n += 4;
		}
		ind[5] = (uint8_t)(n - 7);
		ind[6] = 0;
		say("modem: dcm: %s: bind %d, family %d, start %d (result %d), settings %d: an address of %u letters; indication %#04x of %u bytes to node %u port %u: %d\\n",
		    activate ? "the IMS network for the modem's IMS" : "its address asked again", e1, e2, e3, r3, e4, al, ind[3], n, dcm_client_node, dcm_client_port,
		    dcm_client_port != 0 ? qrtr_send(QRTR_DATA, DCM_PORT, dcm_client_node, dcm_client_port, ind, n) : -1);
	}
	if (dcm_by_hand_pending) {
		static uint8_t m[340];
""")

# the IPv6 session is not asked for six times when the network's answer is final: "PDN type IPv4 only allowed" (3GPP cause 50) or the modem's own
# refusal of the family (type 2, reason 210). Each try held the modem's thread 3 s: the IPv4 session and the IMS service's work waited 18 s behind it.
t = once(t, """			if (e == 0 && qmi_reply.result == 0 && (c = qmi_tlv(0x01, &n)) != NULL && n >= 4) {
				cl6.handle = data_le32(c);
				break;
			}
			mission_pump(3000, NULL);
""", """			if (e == 0 && qmi_reply.result == 0 && (c = qmi_tlv(0x01, &n)) != NULL && n >= 4) {
				cl6.handle = data_le32(c);
				break;
			}
			if (e == 0 && (c = qmi_tlv(0x11, &n)) != NULL && n >= 4 && ((c[0] == 6 && c[2] == 50) || (c[0] == 2 && c[2] == 210))) {
				say("modem: data6: the network allows IPv4 only here (reason type %u, %u): not asked again\\n", c[0], c[2]);
				break;
			}
			mission_pump(3000, NULL);
""")

# a received message's sender is not printed (the kernel log is not the place for phone numbers): its length is
t = once(t, """	say("modem: sms: RECEIVED from %s (%u characters)\\n", from, out);""", """	say("modem: sms: RECEIVED from a number of %u characters (%u characters of text)\\n", (uint32_t)strlen(from), out);""")

# a received text whose length in septets is a multiple of eight lost its last character: the loop asked for more bytes although the last septet was
# already in hand (measured 2026-11-11: 24 characters sent, 23 received)
t = once(t, "		while (got < udl && at + i < n && out < sizeof(text) - 1) {", "		while (got < udl && (bits >= 7 || at + i < n) && out < sizeof(text) - 1) {")

# ---- calls: the modem's voice service (2026-11-11) ------------------------------------------------------------------------------------------
t = once(t, "static bool qmi_find(uint32_t service, uint32_t *node, uint32_t *port);\n", """static bool qmi_find(uint32_t service, uint32_t *node, uint32_t *port);
#define VOICE_SERVICE           9u              /* QMI VOICE: the calls */
static void voice_indication(uint32_t message, const uint8_t *tlv, uint32_t tl);
""")
t = once(t, """		if (q[0] == 4) {
			static unsigned shown;
""", """		if (q[0] == 4) {
			{
				uint32_t voice_node, voice_port;

				if (qmi_find(VOICE_SERVICE, &voice_node, &voice_port) && h.from_node == voice_node && h.from_port == voice_port) {
					/* the voice service's: taken apart there and never printed as bytes (a call's indication carries the other party's number) */
					voice_indication(q[3] | q[4] << 8, q + 7, tl);
					return;
				}
			}
			static unsigned shown;
""")
t = once(t, "static int\ncell_sms_send_sysctl(", """/* ---- calls: the modem's voice service (QMI VOICE, service 9) ---------------------------------------------------------------------------
 *
 * What a call needs of this side is small: the modem's IMS (registered through cell_dcm, see above) or its circuit-switched side does the
 * signalling. Requests: Dial 0x20 (TLV 0x01 the number as text), Answer 0x22 and End 0x21 (TLV 0x01 the call's id), Burst DTMF 0x28 (TLV 0x01:
 * id, count, digits). The modem says what its calls do in the indication All Call Status 0x2e: TLV 0x01 = count, then per call id, state
 * (1 origination, 2 incoming, 3 conversation, 4 call control in progress, 5 alerting, 6 hold, 7 waiting, 8 disconnecting, 9 end, 10 setup),
 * type, direction (1 from here, 2 to here), mode (2 GSM, 3 UMTS, 4 LTE), multiparty, ALS; TLV 0x10 = the other parties' numbers (count, then id,
 * presentation, length, digits); TLV 0x14 = why a call ended (count, then id, a 16-bit reason). Layouts: libqmi's data/qmi-service-voice.json
 * for the requests and 0x01/0x10 [source]; 0x14 as Qualcomm's public voice_service_v02.h has it [source, from memory: to be confirmed on the air].
 *
 * To user space (hk_commcenter, which is iOS's CallKit provider for them):
 *   kern.hackios.modem_call          write "dial NUMBER", "answer ID", "end ID", "dtmf ID DIGITS"; read: how the last request went
 *   kern.hackios.modem_call_events   the count of events so far, then the last sixteen, one a line: N ID STATE DIRECTION TYPE MODE REASON NUMBER
 * An event is a call's change of state. A call that is no longer in the modem's list without having been reported ended gets an event with
 * state 9 made here. The other party's number goes to user space only: the log has its length.
 */
#define VOICE_CALLS             8
#define VOICE_EVENTS            16
static struct voice_call {
	uint8_t id, state, type, direction, mode;
	uint16_t end_reason;
	char number[32];
} voice_call[VOICE_CALLS];
static uint32_t voice_calls;
static char voice_event[VOICE_EVENTS][96];
static uint32_t voice_events;
static char voice_request[64];
static volatile bool voice_pending;
static char voice_status[120] = "none";
static bool voice_reported;

static const char *
voice_state_name(uint8_t state)
{
	static const char *const names[] = { "?", "origination", "incoming", "conversation", "call control in progress", "alerting", "hold", "waiting", "disconnecting", "end", "setup" };

	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

static void
voice_note(const struct voice_call *c, const char *how)
{
	snprintf(voice_event[voice_events % VOICE_EVENTS], sizeof(voice_event[0]), "%u %u %u %u %u %u %u %s", voice_events, c->id, c->state, c->direction, c->type, c->mode,
	    c->end_reason, c->number[0] != 0 ? c->number : "-");
	voice_events++;
	say("modem: voice: call %u: %s%s (%s, type %u, mode %u; a number of %u characters; end reason %u)\\n", c->id, voice_state_name(c->state), how,
	    c->direction == 1 ? "from here" : c->direction == 2 ? "to here" : "direction ?", c->type, c->mode, (uint32_t)strlen(c->number), c->end_reason);
}

static void
voice_indication(uint32_t message, const uint8_t *tlv, uint32_t tl)
{
	struct voice_call now[VOICE_CALLS];
	uint32_t calls = 0;
	char kinds[160];
	uint32_t k = 0;
	static unsigned said, others;

	if (message != 0x002e) {
		/* (the service's other indications: by number and size; what they carry is not printed) */
		if (others++ < 80) {
			say("modem: voice: indication %#06x (%u bytes)\\n", message, tl);
		}
		return;
	}
	bzero(now, sizeof(now));
	kinds[0] = 0;
	for (uint32_t t = 0; t + 3 <= tl;) {
		uint32_t n = tlv[t + 1] | tlv[t + 2] << 8;
		const uint8_t *v = tlv + t + 3;

		if (t + 3 + n > tl) {
			break;
		}
		if (k + 8 < sizeof(kinds)) {
			k += (uint32_t)snprintf(kinds + k, sizeof(kinds) - k, " %02x/%u", tlv[t], n);
		}
		if (tlv[t] == 0x01 && n >= 1) {
			for (uint32_t i = 0; i < v[0] && 1 + 7 * (i + 1) <= n && calls < VOICE_CALLS; i++) {
				const uint8_t *c = v + 1 + 7 * i;

				now[calls].id = c[0];
				now[calls].state = c[1];
				now[calls].type = c[2];
				now[calls].direction = c[3];
				now[calls].mode = c[4];
				calls++;
			}
		}
		t += 3 + n;
	}
	for (uint32_t t = 0; t + 3 <= tl;) {
		uint32_t n = tlv[t + 1] | tlv[t + 2] << 8;
		const uint8_t *v = tlv + t + 3;

		if (t + 3 + n > tl) {
			break;
		}
		if (tlv[t] == 0x10 && n >= 1) {
			uint32_t at = 1;

			for (uint32_t i = 0; i < v[0] && at + 3 <= n; i++) {
				uint32_t len = v[at + 2];

				if (at + 3 + len > n) {
					break;
				}
				for (uint32_t c = 0; c < calls; c++) {
					if (now[c].id == v[at]) {
						uint32_t l = len < sizeof(now[c].number) - 1 ? len : (uint32_t)sizeof(now[c].number) - 1;

						memcpy(now[c].number, v + at + 3, l);
						now[c].number[l] = 0;
						/* (only what a number is made of goes on: this text is split at spaces by its reader) */
						for (uint32_t d = 0; d < l; d++) {
							char ch = now[c].number[d];

							if (!((ch >= '0' && ch <= '9') || ch == '+' || ch == '*' || ch == '#')) {
								now[c].number[d] = '?';
							}
						}
					}
				}
				at += 3 + len;
			}
		} else if (tlv[t] == 0x14 && n >= 1) {
			for (uint32_t i = 0; i < v[0] && 1 + 3 * (i + 1) <= n; i++) {
				for (uint32_t c = 0; c < calls; c++) {
					if (now[c].id == v[1 + 3 * i]) {
						now[c].end_reason = (uint16_t)(v[2 + 3 * i] | v[3 + 3 * i] << 8);
					}
				}
			}
		}
		t += 3 + n;
	}
	if (said++ < 40) {
		say("modem: voice: all call status: %u call(s); TLVs (type/bytes):%s\\n", calls, kinds);
	}
	/* a call new in the list, or in another state than before: an event */
	for (uint32_t c = 0; c < calls; c++) {
		const struct voice_call *was = NULL;

		for (uint32_t o = 0; o < voice_calls; o++) {
			if (voice_call[o].id == now[c].id) {
				was = &voice_call[o];
			}
		}
		if (was != NULL && now[c].number[0] == 0) {
			strlcpy(now[c].number, was->number, sizeof(now[c].number));
		}
		if (was == NULL || was->state != now[c].state) {
			voice_note(&now[c], "");
		}
	}
	/* a call that left the list without the modem having said "end": ended, as far as anybody here can tell */
	for (uint32_t o = 0; o < voice_calls; o++) {
		bool there = false;

		for (uint32_t c = 0; c < calls; c++) {
			there = there || now[c].id == voice_call[o].id;
		}
		if (!there && voice_call[o].state != 9) {
			struct voice_call gone = voice_call[o];

			gone.state = 9;
			voice_note(&gone, " (it left the modem's list)");
		}
	}
	memcpy(voice_call, now, sizeof(voice_call));
	voice_calls = calls;
}

/* the modem's thread: the indications asked for once the service is there, and a request from user space carried out */
static void
voice_service(void)
{
	char *arg;
	int e = EINVAL;
	uint32_t n = 0;
	const uint8_t *v;

	if (!voice_reported) {
		/* Indication Register 0x03: DTMF (0x10), supplementary services (0x12), call events (0x13), handover (0x14), speech codec (0x15), each on */
		static const uint8_t want[] = { 0x10, 1, 0, 1, 0x12, 1, 0, 1, 0x13, 1, 0, 1, 0x14, 1, 0, 1, 0x15, 1, 0, 1 };
		static uint64_t again;
		static unsigned tries;
		uint64_t now = mhi_op_nanoseconds(NULL);

		if (now >= again) {
			again = now + 5000000000ull;
			e = qmi_call(VOICE_SERVICE, 0x0003, want, sizeof(want), 3000);
			if (e == 0 && qmi_reply.result == 0) {
				voice_reported = true;
				say("modem: voice: calls are reported\\n");
			} else if (tries++ < 4) {
				say("modem: voice: asking for call reports: %d (result %u error %u)\\n", e, e == 0 ? qmi_reply.result : 0, e == 0 ? qmi_reply.error : 0);
			}
		}
	}
	if (!voice_pending) {
		return;
	}
	arg = strchr(voice_request, ' ');
	arg = arg != NULL ? arg + 1 : voice_request + strlen(voice_request);
	if (strncmp(voice_request, "dial ", 5) == 0 && arg[0] != 0 && strlen(arg) <= 40) {
		uint8_t tlvs[3 + 40];
		uint32_t len = (uint32_t)strlen(arg);

		tlvs[0] = 0x01;
		tlvs[1] = (uint8_t)len;
		tlvs[2] = 0;
		memcpy(tlvs + 3, arg, len);
		e = qmi_call(VOICE_SERVICE, 0x0020, tlvs, 3 + len, 10000);
		v = e == 0 ? qmi_tlv(0x10, &n) : NULL;
		say("modem: voice: dial, a number of %u characters: %s (result %u error %u), call %u\\n", len, e == 0 ? "answered" : "NO REPLY", e == 0 ? qmi_reply.result : 0,
		    e == 0 ? qmi_reply.error : 0, v != NULL && n >= 1 ? v[0] : 0);
		if (e == 0 && qmi_reply.result == 0) {
			snprintf(voice_status, sizeof(voice_status), "dial: accepted, call %u", v != NULL && n >= 1 ? v[0] : 0);
		} else {
			snprintf(voice_status, sizeof(voice_status), "dial: failed (%d, result %u error %u)", e, e == 0 ? qmi_reply.result : 0, e == 0 ? qmi_reply.error : 0);
		}
	} else if ((strncmp(voice_request, "answer ", 7) == 0 || strncmp(voice_request, "end ", 4) == 0) && arg[0] >= '0' && arg[0] <= '9') {
		bool answer = voice_request[0] == 'a';
		uint8_t tlvs[4] = { 0x01, 1, 0, (uint8_t)strtoul(arg, NULL, 10) };

		e = qmi_call(VOICE_SERVICE, answer ? 0x0022 : 0x0021, tlvs, sizeof(tlvs), 10000);
		say("modem: voice: %s call %u: %s (result %u error %u)\\n", answer ? "answer" : "end", tlvs[3], e == 0 ? "answered" : "NO REPLY", e == 0 ? qmi_reply.result : 0,
		    e == 0 ? qmi_reply.error : 0);
		if (e == 0 && qmi_reply.result == 0) {
			snprintf(voice_status, sizeof(voice_status), "%s: accepted, call %u", answer ? "answer" : "end", tlvs[3]);
		} else {
			snprintf(voice_status, sizeof(voice_status), "%s: failed (%d, result %u error %u)", answer ? "answer" : "end", e, e == 0 ? qmi_reply.result : 0, e == 0 ? qmi_reply.error : 0);
		}
	} else if (strncmp(voice_request, "dtmf ", 5) == 0 && arg[0] >= '0' && arg[0] <= '9' && strchr(arg, ' ') != NULL) {
		const char *digits = strchr(arg, ' ') + 1;
		uint32_t len = (uint32_t)strlen(digits);
		uint8_t tlvs[3 + 2 + 32];

		if (len >= 1 && len <= 32) {
			tlvs[0] = 0x01;
			tlvs[1] = (uint8_t)(2 + len);
			tlvs[2] = 0;
			tlvs[3] = (uint8_t)strtoul(arg, NULL, 10);
			tlvs[4] = (uint8_t)len;
			memcpy(tlvs + 5, digits, len);
			e = qmi_call(VOICE_SERVICE, 0x0028, tlvs, 5 + len, 10000);
			say("modem: voice: %u tone(s) into call %u: %s (result %u error %u)\\n", len, tlvs[3], e == 0 ? "answered" : "NO REPLY", e == 0 ? qmi_reply.result : 0, e == 0 ? qmi_reply.error : 0);
		}
		if (e == 0 && qmi_reply.result == 0) {
			snprintf(voice_status, sizeof(voice_status), "dtmf: accepted");
		} else {
			snprintf(voice_status, sizeof(voice_status), "dtmf: failed (%d, result %u error %u)", e, e == 0 ? qmi_reply.result : 0, e == 0 ? qmi_reply.error : 0);
		}
	} else {
		snprintf(voice_status, sizeof(voice_status), "failed (write: dial NUMBER, answer ID, end ID, dtmf ID DIGITS)");
	}
	voice_pending = false;
}

static int
cell_call_sysctl(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[sizeof(voice_request)];
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, voice_status, strlen(voice_status) + 1);
	}
	if (req->newlen == 0 || req->newlen >= sizeof(text)) {
		return EINVAL;
	}
	bzero(text, sizeof(text));
	if ((error = SYSCTL_IN(req, text, req->newlen)) != 0) {
		return error;
	}
	if (voice_pending) {
		return EBUSY;
	}
	strlcpy(voice_request, text, sizeof(voice_request));
	snprintf(voice_status, sizeof(voice_status), "queued");
	voice_pending = true;
	/* (the request's first word: what follows a "dial" is a number) */
	printf("modem: voice: a request from user space is queued for the modem's thread: %.6s...\\n", voice_request);
	return 0;
}

static int
cell_call_events_sysctl(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[32 + VOICE_EVENTS * (sizeof(voice_event[0]) + 1)];
	uint32_t count = voice_events;
	size_t at = (size_t)snprintf(text, sizeof(text), "%u\\n", count);

	for (uint32_t i = count > VOICE_EVENTS ? count - VOICE_EVENTS : 0; i < count && at < sizeof(text) - sizeof(voice_event[0]) - 2; i++) {
		at += (size_t)snprintf(text + at, sizeof(text) - at, "%s\\n", voice_event[i % VOICE_EVENTS]);
	}
	return SYSCTL_OUT(req, text, at + 1);
}

static int
cell_sms_send_sysctl(""")
t = once(t, "SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_sms_send, ", """SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_call, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, cell_call_sysctl, "A", "write dial NUMBER, answer ID, end ID or dtmf ID DIGITS; read: how the last request went");
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_call_events, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, cell_call_events_sysctl, "A", "the count of call events, then the last sixteen: N ID STATE DIRECTION TYPE MODE REASON NUMBER");
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_sms_send, """)
t = once(t, """	if (cell.sms_pending) {
		char to[24], *text = cell.sms_request;
""", """	voice_service();
	if (cell.sms_pending) {
		char to[24], *text = cell.sms_request;
""")

# ---- a received message's header, and room for a whole message (2026-11-11) ------------------------------------------------------------------
# A message with a user data header (TP-UDHI: a part of a long message, or one that a gateway marks) had the header decoded as text. It is skipped
# now: its bytes for UCS-2, the septets it covers for the 7-bit alphabet; a part's place (n of m) is logged, the parts are NOT joined yet (each
# arrives as a message of its own). And a whole message did not fit: 70 Chinese characters are 210 bytes of UTF-8 and text[] had 200.
t = once(t, '	char from[24] = "", text[200];\n	uint32_t oa_len, digits;\n', '	char from[24] = "", text[340];\n	uint32_t oa_len, digits, skip = 0, part = 0, parts = 0;\n')
t = once(t, """	udl = pdu[at++];
	if ((dcs & 0x0c) == 0x08) {             /* UCS-2 */
""", """	udl = pdu[at++];
	if ((first & 0x40) != 0 && at < n) {
		/* a user data header: its length, then elements (id, length, data); 0x00 and 0x08 say which part of a long message this is */
		uint32_t udhl = pdu[at];

		for (uint32_t i = 1; i + 1 < 1 + udhl && at + i + 1 < n;) {
			uint32_t iei = pdu[at + i], iel = pdu[at + i + 1];

			if (at + i + 2 + iel > n) {
				break;
			}
			if (iei == 0x00 && iel == 3) {
				parts = pdu[at + i + 3];
				part = pdu[at + i + 4];
			} else if (iei == 0x08 && iel == 4) {
				parts = pdu[at + i + 4];
				part = pdu[at + i + 5];
			}
			i += 2 + iel;
		}
		skip = (dcs & 0x0c) == 0x08 ? udhl + 1 : ((udhl + 1) * 8 + 6) / 7;
	}
	if ((dcs & 0x0c) == 0x08) {             /* UCS-2 */
""")
t = once(t, "		for (uint32_t i = 0; i + 1 < udl && at + i + 1 < n && out + 5 < sizeof(text); i += 2) {\n			uint32_t c = (uint32_t)(pdu[at + i] << 8 | pdu[at + i + 1]);\n",
         "		for (uint32_t i = skip; i + 1 < udl && at + i + 1 < n && out + 5 < sizeof(text); i += 2) {\n			uint32_t c = (uint32_t)(pdu[at + i] << 8 | pdu[at + i + 1]);\n")
t = once(t, """				uint8_t c = (uint8_t)(acc & 0x7f);

				/* the accented letters of the default alphabet""", """				uint8_t c = (uint8_t)(acc & 0x7f);
				uint32_t before = out;

				/* the accented letters of the default alphabet""")
t = once(t, """				acc >>= 7;
				bits -= 7;
				got++;
""", """				if (got < skip) {
					out = before;           /* (a septet of the header) */
				}
				acc >>= 7;
				bits -= 7;
				got++;
""")
t = once(t, """	say("modem: sms: RECEIVED from a number of %u characters (%u characters of text)\\n", (uint32_t)strlen(from), out);""",
         """	say("modem: sms: RECEIVED from a number of %u characters (%u characters of text; coding %#x%s)\\n", (uint32_t)strlen(from), out, dcs, (first & 0x40) != 0 ? ", with a header" : "");
	if (parts > 1) {
		say("modem: sms: it is part %u of %u of a long message (the parts are not joined here)\\n", part, parts);
	}""")
t = once(t, '	char sms_last[240];             /* the latest message received: "NUMBER: TEXT" */', '	char sms_last[380];             /* the latest message received: "NUMBER: TEXT" */')
t = once(t, '	char sms_ring[8][240];', '	char sms_ring[8][380];')
t = once(t, "	char text[8 * 260 + 1];\n	size_t n = 0;\n	uint32_t count = cell.sms_received;\n", "	char text[8 * 400 + 1];\n	size_t n = 0;\n	uint32_t count = cell.sms_received;\n")
t = once(t, '	char text[300];\n\n	snprintf(text, sizeof(text), "%u %s", cell.sms_received, cell.sms_last);\n', '	char text[400];\n\n	snprintf(text, sizeof(text), "%u %s", cell.sms_received, cell.sms_last);\n')

# The modem's own messages during a call (2026-11-11, run 071113): its IMS call code prints the number dialled, the caller's, SIP addresses, the
# media addresses and the session's two keys ("ck-value", "ik-value"). None of that belongs in a log: those files' lines and those words are not printed.
# (vagent.c and vs_*.c, the voice agent that asks the audio DSP for a call's sound, say none of it and stay.)
t = once(t, '"UIM_PUP_MSG", "VAdcLog", ', '"ck-value", "ik-value", "qipcall", "qimfif", "sipConnection", "qpSip", "sipDialog", "remote_ip", "remote ip", "Remote_IP", "Local_IP", "remote_uri", "UIM_PUP_MSG", "VAdcLog", ')

# ---- the sound of a call (2026-11-11): the audio DSP's side is there now (voice_port.py makes the overlay's hk_adsp.c with the Pro's part), so the
# stand-ins for it go and the real functions are declared; the RTC's stays. The satellite is on with voice=3 and above, or as voice_sat says.
t = once(t, """static void hk_adsp_qrtr_from_modem(uint32_t type, uint32_t from_node, uint32_t from_port, uint32_t to_node, uint32_t to_port, bool confirm, const void *data, uint32_t length)
{
	(void)type; (void)from_node; (void)from_port; (void)to_node; (void)to_port; (void)confirm; (void)data; (void)length;
}
static void hk_adsp_qrtr_announce(void) { }
static uint32_t hk_adsp_ssctl_port(void) { return 0; }
static bool hk_adsp_sat_open(void) { return false; }
static int hk_adsp_sat_send(const void *data, uint32_t length) { (void)data; (void)length; return ENXIO; }
""", """extern void hk_adsp_qrtr_from_modem(uint32_t type, uint32_t from_node, uint32_t from_port, uint32_t to_node, uint32_t to_port, bool confirm, const void *data, uint32_t length);
extern void hk_adsp_qrtr_announce(void);
extern uint32_t hk_adsp_ssctl_port(void);
extern bool hk_adsp_sat_open(void);
extern int hk_adsp_sat_send(const void *data, uint32_t length);
""")
t = once(t, """				cell_arg("voice_sat", &sat_on, sizeof(sat_on));
				option_sat = sat_on != 0;
""", """				uint32_t voice_steps = 0;
				bool sat_said = cell_arg("voice_sat", &sat_on, sizeof(sat_on));

				if (!PE_parse_boot_argn("voice", &voice_steps, sizeof(voice_steps))) {
					voice_steps = 3;        /* (not given: 3 with the cellular path, which this is: hk_mic.c's voice_steps()) */
				}
				option_sat = sat_said ? sat_on != 0 : voice_steps >= 3;
""")

# ---- the modem thread's stack (2026-11-11, run 072818: "Invalid kernel stack pointer (probable overflow)") -------------------------------------
# A packet from the network came while the thread waited for a QMI answer deep in its own work: modem_thread 1008 bytes of stack, hk_cell_run 1632,
# mission_run 3056, qmi_call 4208, the pump, cl_receive 3296, and then the network's own way to an mbuf, which ran out of the 16 KB a kernel thread has
# when it took its slower path (m_clalloc). The two big buffers of that chain are not on the stack any more: they are the modem thread's alone
# (one QMI request at a time, and it is built and sent before the wait; one packet from the modem at a time).
t = once(t, "	uint8_t q[IPCR_PACKET / 2];\n	uint16_t transaction = ++qmi_transaction;\n", "	static uint8_t q[IPCR_PACKET / 2];     /* (not on the stack: see port.py, \"the modem thread's stack\") */\n	uint16_t transaction = ++qmi_transaction;\n")
t = once(t, "	uint8_t frame[HK_NET_FRAME_MAX];\n	uint32_t version = length > 0 ? packet[0] >> 4 : 0;\n", "	static uint8_t frame[HK_NET_FRAME_MAX]; /* (not on the stack: the modem's thread is the only one here) */\n	uint32_t version = length > 0 ? packet[0] >> 4 : 0;\n")
a = t.index('\ncl6_receive(const uint8_t *packet, uint32_t length)\n{\n')
b = t.index('	uint8_t frame[HK_NET_FRAME_MAX];\n', a)
assert b - a < 200, b - a
t = t[:b] + '	static uint8_t frame[HK_NET_FRAME_MAX]; /* (not on the stack: the modem\'s thread is the only one here) */\n' + t[b + len('	uint8_t frame[HK_NET_FRAME_MAX];\n'):]

# ---- a long message's parts joined (2026-11-11) -------------------------------------------------------------------------------------------------
# A long message comes as parts, each an SMS of its own with a header that says which message (a reference), how many parts and which one this is.
# They are held here until all are there, then given on as one message; a message whose parts do not all come within a minute is given on with what
# came. Up to eight parts (1200 letters, or 530 Chinese characters); more than that, and every part goes on by itself as before.
t = once(t, "	uint32_t oa_len, digits, skip = 0, part = 0, parts = 0;\n", "	uint32_t oa_len, digits, skip = 0, part = 0, parts = 0, long_ref = 0;\n")
t = once(t, """			if (iei == 0x00 && iel == 3) {
				parts = pdu[at + i + 3];
				part = pdu[at + i + 4];
			} else if (iei == 0x08 && iel == 4) {
				parts = pdu[at + i + 4];
				part = pdu[at + i + 5];
			}
""", """			if (iei == 0x00 && iel == 3) {
				long_ref = pdu[at + i + 2];
				parts = pdu[at + i + 3];
				part = pdu[at + i + 4];
			} else if (iei == 0x08 && iel == 4) {
				long_ref = (uint32_t)(pdu[at + i + 2] << 8 | pdu[at + i + 3]);
				parts = pdu[at + i + 4];
				part = pdu[at + i + 5];
			}
""")
t = once(t, """	text[out] = 0;
	snprintf(cell.sms_last, sizeof(cell.sms_last), "%s: %s", from, text);
	strlcpy(cell.sms_ring[cell.sms_received % 8], cell.sms_last, sizeof(cell.sms_ring[0]));
	cell.sms_received++;
	say("modem: sms: RECEIVED from a number of %u characters (%u characters of text; coding %#x%s)\\n", (uint32_t)strlen(from), out, dcs, (first & 0x40) != 0 ? ", with a header" : "");
	if (parts > 1) {
		say("modem: sms: it is part %u of %u of a long message (the parts are not joined here)\\n", part, parts);
	}
}
""", """	text[out] = 0;
	say("modem: sms: RECEIVED from a number of %u characters (%u characters of text; coding %#x%s)\\n", (uint32_t)strlen(from), out, dcs, (first & 0x40) != 0 ? ", with a header" : "");
	if (parts > 1 && parts <= SMS_LONG_PARTS && part >= 1 && part <= parts) {
		sms_long_part(from, long_ref, parts, part, text);
		return;
	}
	if (parts > 1) {
		say("modem: sms: it is part %u of %u of a long message: more parts than are joined here, given on by itself\\n", part, parts);
	}
	sms_deliver(from, text);
}
""")
t = once(t, """/* an SMS-DELIVER (3GPP TS 23.040) as text: the sender and the message""", """/* a message for user space: the latest, and into the ring of the last eight */
static void
sms_deliver(const char *from, const char *text)
{
	snprintf(cell.sms_last, sizeof(cell.sms_last), "%s: %s", from, text);
	strlcpy(cell.sms_ring[cell.sms_received % 8], cell.sms_last, sizeof(cell.sms_ring[0]));
	cell.sms_received++;
}

/* the parts of long messages on their way: by sender, reference and number of parts */
#define SMS_LONG_SLOTS          3
#define SMS_LONG_PARTS          8
#define SMS_LONG_WAIT_NS        60000000000ull
static struct sms_long {
	bool used;
	char from[24];
	uint32_t ref, parts, have;      /* have: a bit for each part held */
	uint64_t since;
	char part[SMS_LONG_PARTS][340];
} sms_long[SMS_LONG_SLOTS];

static void
sms_long_give(struct sms_long *s, const char *why)
{
	static char joined[SMS_LONG_PARTS * 340 + 1];   /* (the modem's thread alone) */
	uint32_t held = 0;

	joined[0] = 0;
	for (uint32_t i = 0; i < s->parts; i++) {
		if ((s->have & 1u << i) != 0) {
			strlcat(joined, s->part[i], sizeof(joined));
			held++;
		}
	}
	say("modem: sms: a long message of %u parts, %u of them here (%s): given on as one, %u characters\\n", s->parts, held, why, (uint32_t)strlen(joined));
	sms_deliver(s->from, joined);
	bzero(s, sizeof(*s));
}

static void
sms_long_part(const char *from, uint32_t ref, uint32_t parts, uint32_t part, const char *text)
{
	struct sms_long *s = NULL, *oldest = &sms_long[0];
	uint32_t held = 0;

	for (unsigned i = 0; i < SMS_LONG_SLOTS; i++) {
		if (sms_long[i].used && sms_long[i].ref == ref && sms_long[i].parts == parts && strcmp(sms_long[i].from, from) == 0) {
			s = &sms_long[i];
		}
		if (sms_long[i].since < oldest->since) {
			oldest = &sms_long[i];
		}
	}
	for (unsigned i = 0; i < SMS_LONG_SLOTS && s == NULL; i++) {
		if (!sms_long[i].used) {
			s = &sms_long[i];
		}
	}
	if (s == NULL) {
		sms_long_give(oldest, "its place is needed for another");
		s = oldest;
	}
	if (!s->used) {
		bzero(s, sizeof(*s));
		s->used = true;
		strlcpy(s->from, from, sizeof(s->from));
		s->ref = ref;
		s->parts = parts;
		s->since = mhi_op_nanoseconds(NULL);
	}
	strlcpy(s->part[part - 1], text, sizeof(s->part[0]));
	s->have |= 1u << (part - 1);
	for (uint32_t i = 0; i < parts; i++) {
		held += (s->have >> i) & 1;
	}
	say("modem: sms: part %u of %u of a long message: %u of them here\\n", part, parts, held);
	if (held == parts) {
		sms_long_give(s, "whole");
	}
}

/* the modem's thread, now and then: a long message that has waited a minute for its parts goes on without them */
static void
sms_long_tick(void)
{
	uint64_t now = mhi_op_nanoseconds(NULL);

	for (unsigned i = 0; i < SMS_LONG_SLOTS; i++) {
		if (sms_long[i].used && now - sms_long[i].since > SMS_LONG_WAIT_NS) {
			sms_long_give(&sms_long[i], "a minute has passed");
		}
	}
}

/* an SMS-DELIVER (3GPP TS 23.040) as text: the sender and the message""")
t = once(t, """	voice_service();
	if (cell.sms_pending) {
""", """	voice_service();
	sms_long_tick();
	if (cell.sms_pending) {
""")
# room for a joined message in what user space reads; the ring's text is made on the heap (it was 3 KB of a sysctl's stack, and is 14 KB now)
t = once(t, '	char sms_last[380];             /* the latest message received: "NUMBER: TEXT" */', '	char sms_last[SMS_LONG_TEXT];   /* the latest message received: "NUMBER: TEXT" */')
t = once(t, '	char sms_ring[8][380];', '	char sms_ring[8][SMS_LONG_TEXT];')
t = once(t, "static volatile bool cell_mt_pending;", "#define SMS_LONG_TEXT           2760            /* a message of eight parts, 340 bytes each, behind its sender */\nstatic volatile bool cell_mt_pending;")
t = span(t, "static int\ncell_sms_ring_sysctl(", "\n}\n", """static int
cell_sms_ring_sysctl(__unused struct sysctl_oid *oid, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	const size_t size = 8 * (sizeof(cell.sms_ring[0]) + 16) + 1;
	char *text = kalloc_data(size, Z_WAITOK | Z_ZERO);
	size_t n = 0;
	uint32_t count = cell.sms_received;
	int error;

	if (text == NULL) {
		return ENOMEM;
	}
	for (uint32_t i = count > 8 ? count - 8 : 0; i < count; i++) {
		const char *m = cell.sms_ring[i % 8];

		n += (size_t)snprintf(text + n, size - n, "%u\\t", i);
		for (; *m != 0 && n + 2 < size; m++) {
			text[n++] = *m == '\\n' || *m == '\\r' ? 0x1f : *m;
		}
		text[n++] = '\\n';
		text[n] = 0;
	}
	error = SYSCTL_OUT(req, text, n + 1);
	kfree_data(text, size);
	return error;
}
""")
(OUT / 'instantnoodle_cell.c').write_text(t, encoding='utf-8')

m = (REF / 'pro_hk_mhi.c').read_text(encoding='utf-8')
m = once(m, '#include <sys/hk_mhi.h>\n', '#include "hk_mhi.h"\n')
(OUT / 'hk_mhi.c').write_text('/* From github.com/dekefake/hackios-op8pro @ e5631cd (APSL 2.0), bsd/dev/hk_mhi.c: only the include below differs. */\n' + m, encoding='utf-8')
(OUT / 'hk_mhi.h').write_text((REF / 'pro_hk_mhi.h').read_text(encoding='utf-8'), encoding='utf-8')

n = (REF / 'pro_hk_net.h').read_text(encoding='utf-8')
a = n.index('struct hk_net_link {')
b = n.index('};', a) + 2
(OUT / 'cell_net.h').write_text('''/*
 * The Pro's network link for the modem's packets (its sys/hk_net.h has units: en0 the radio, en1 the modem). This tree's hk_net.c
 * has one unit (en0, the radio), so the Pro's hk_net.c is here a second time as cell_net.c, every name of it hk_cell_net_*, and only
 * its unit 1 is used: en1. The radio's en0 and its code are untouched.
 */
#ifndef _CELL_NET_H_
#define _CELL_NET_H_
#include <sys/kpi_mbuf.h>

#define HK_NET_FRAME_MAX   1514
#define HK_NET_UNITS       2
#define hk_net_link cell_net_link
''' + n[a:b] + '''

bool hk_cell_net_attach(const struct hk_net_link *link);
void hk_cell_net_input_unit(unsigned unit, const uint8_t *frame, size_t length);
void hk_cell_net_link_state_unit(unsigned unit, bool up);
errno_t hk_apple80211_ioctl(u_long command, void *data);
#ifndef CELL_NET_ITSELF
#define hk_net_attach hk_cell_net_attach
#define hk_net_input_unit hk_cell_net_input_unit
#define hk_net_link_state_unit hk_cell_net_link_state_unit
#endif
#endif
''', encoding='utf-8')

# cell_net.c: the Pro's hk_net.c, its public names prefixed, without the part that looks for QEMU's device
c = (REF / 'pro_hk_net.c').read_text(encoding='utf-8')
c = once(c, '#include <sys/hk_net.h>', '#define CELL_NET_ITSELF\n#include "cell_net.h"')
a2 = c.index('void\nhk_net_init(void)')
c = c[:a2].rstrip() + '\n'
c = re.sub(r'\bhk_net_(attach|input_unit|input|link_state_unit|link_state|address)\b', r'hk_cell_net_\1', c)
for name in ('hk_cell_net_input', 'hk_cell_net_link_state', 'hk_cell_net_address'):
    assert re.search(r'^void\n' + name + r'\(', c, re.M), name
    c = re.sub(r'^void\n' + name + r'\(', 'static __unused void\n' + name + '(', c, flags=re.M)
assert 'hk_virtio_net_init' not in c and not re.search(r'\bhk_net_(?!link\b)', c)
(OUT / 'cell_net.c').write_text('/* From github.com/dekefake/hackios-op8pro @ e5631cd (APSL 2.0), bsd/dev/hk_net.c: its names prefixed (hk_cell_net_*), hk_net_init() left out. */\n' + c, encoding='utf-8')
print('ported:', ', '.join(sorted(p.name for p in OUT.iterdir())))
