# The cellular path (boot-arg `cell=1`)

The OnePlus 8's modem is a Snapdragon X55 on PCIe 2, a chip of its own beside the SoC. This directory holds the kernel
driver that boots it and talks to it, and the scripts that build and start a kernel with it. With it the installed
system has mobile data, SMS both ways and VoLTE calls with sound. What works, what does not and how each part was
checked is in [docs/subsystems/cellular.md](../../../docs/subsystems/cellular.md).

## Where the driver comes from

The driver is the OnePlus 8 Pro port's, moved to this phone:
[dekefake/hackios-op8pro](https://github.com/dekefake/hackios-op8pro) at commit
`e5631cd2523a8a7a897618a3535fe2a08b1672dc`, under the Apple Public Source License 2.0 like the rest of this tree. The
two phones share the SoC, the modem and its control lines. This tree's own modem path (`tools/nf/modem`, `modem=3`)
booted the modem but stayed at "not ready" for a day, so the Pro's driver, which already reached the network, was
ported instead of continuing that one.

The port is a script, so it can be made again from a newer copy of the Pro's files:

| File | What it is |
|---|---|
| `fetch-ref.sh` | Fetches the Pro's 14 files at that commit into `ref-pro/` (not in git) |
| `port.py REF_DIR OUT_DIR` | Makes `instantnoodle_cell.c`, `hk_mhi.c`, `hk_mhi.h`, `cell_net.c`, `cell_net.h` from them. Every change to the Pro's files is a named replacement in this script, and it stops if a place it changes is not found exactly once |
| `voice_port.py` | Makes the audio drivers in `overlay/` from this tree's (`voice-base/`) plus the Pro's call additions |
| `cell_dt.cpp` | Written here: adds the `baseband` entry iOS looks for in the device tree |

`python3 port.py ref-pro .` and `python3 voice_port.py` give back exactly the sources in this directory.

What differs from the Pro's driver, in short: storage is read through `/dev/rufs_<partition>`; the modem thread is
started by this tree's `instantnoodle_modem.c` after the warm reset a RAM boot needs; SMS and call state are published
as `kern.hackios.*` sysctls for the CommCenter stand-in; long SMS parts are joined; the modem's log lines that carry
numbers, addresses or session keys are not printed.

## Building

```sh
python3 tools/nf/cell/build.py          # writes out/kernels/cell-SHA256.mach, names it in out/cell-kernel.txt
bash tools/nf/cell/mk_userland.sh       # hk_commcenter, libhackios-call.dylib, oslog into userland/obj
```

`build.py` compiles in the `oneplus8-build` container: the sources in `base/` (the 45 files of the last kernel tested
before the port, as they were then; the tree's own copies have moved on), `overlay/` on top, and the driver. It relinks
against the objects of the container's `nf-tree` build. The build of 2026-11-11 from this directory gives
`cell-0a663690…`, the kernel that is installed on the phone.

It is not yet a build anyone can repeat from a fresh clone. Three inputs are outside git:

- the container's `nf-tree` build of the kernel (`BUILD_NF`), whose objects are relinked;
- `vfs_attrlist.o`, built from the patched `bsd/vfs/vfs_attrlist.c` (`CELL_VFS_OBJ`);
- the Wi-Fi driver's object built against this kernel's `sys/hk_qmi.h` by `tools/mk-ath11k.sh` (`CELL_ATH11K_OBJ`).

Moving the driver into the normal `./xl build` is open work.

The overlay also carries one fix to XNU itself: `bsd/kern/subr_log_stream.c` freed the log stream's block map with the
wrong call, and closing `/dev/oslog_stream` ended the system.

## Starting it on the phone

Every start goes through `tools/bootq`, and enters the loader with `tools/sensors/cold_recover.py --developer`.

| Script | What it does | Writes to the phone |
|---|---|---|
| `boot.sh TAG SECS ["MORE"]` | One RAM boot of the built kernel. `CELL_ROOT`, `CELL_LINE` and `CELL_FULL=FILE` choose the root, the argument line and a copy of the whole returned log | No |
| `mk_root.sh TAG JOBS [FILE=DEST…]` | A root image for a run, from the shared test root | No |
| `state.sh` | Reads what is installed: the two bundle slots and the boot guard | No |
| `lastlog.sh FILE` | Reads the log the last run left | No |
| `preview.sh TAG SECS` | Starts the system as it will be installed, with the kept `/private/var` | Yes, the data volume |
| `install.sh BUNDLE` | Writes a bundle into the boot slot that does not hold the newest good one | Yes, one boot slot |
| `pull-modem.sh` | Copies the modem firmware partition to `out/cell-fw/` for the DIAG message database | No |

`daily-installed-line.txt` is the installed bundle's argument line. `jobs/rc.jobs.daily` is its job list: DHCP on the
modem's interface and nothing else. `inject.conf.calls` is its `etc/inject.conf`.

The installed bundle was made with:

```sh
python3 common/flash/mkbundle.py -o out/instantnoodle-cell-bundle.img --kernel "$(cat out/cell-kernel.txt)" \
    --resident userland/flashboot.apfs --ramdisk out/cell-daily-root.apfs \
    --cmdline "$(cat tools/nf/cell/daily-installed-line.txt)" --label "Cellular 2026-11-11"
```

## Reading the modem's own messages

`diag/` reads a run's log: `merge.py` joins the snapshots of the kernel's log ring, `sum.py` gives one page on what
a run did, `q4.py` and `qdb.py` put texts to the modem's terse DIAG messages from the firmware's own database, `lg.py`
and `ev.py` take log packets and events apart, `fat.py` reads files out of the firmware partition's image. They mask
long digit runs, so identities and phone numbers do not reach a page.

## Rules that hold here

- The modem's EFS partitions are read only. Its crash memory is never read.
- No IMSI, IMEI, ICCID, phone number or address assigned by the carrier goes into a log, a document or a commit.
- Vendor firmware and the Pro's fetched files stay out of git.
