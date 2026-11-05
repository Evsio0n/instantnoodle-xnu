# Building HackiOS

Everything needed to build the kernel, loader, UEFI and images, and what `prebuilt/` is. Running is in
[devices/](devices/README.md); installing in [INSTALL.md](INSTALL.md); tests in [TESTING.md](TESTING.md).

`common/build.sh <device>` runs every step this machine can run and uses `prebuilt/` for the rest:

1. **kernel** `xnu/apply.sh`, `xnu-deps/setup.sh`, `xnu-build.sh` — only with theos + `iPhoneOS16.5.sdk`, else `prebuilt/mach.development.bcm2837`
2. **loader** `xnuloader/build.sh` — only with `clang` + `lld-link` (an EFI application without EDK2)
3. **images** `userland/build.sh` (`REST=1` and the profile's `USERLAND_VARS`) — needs `ios165/fetch.sh` to have run
4. **boot** `mkxnuimg.py` → `out/<device>-boot.img` (the UEFI image for `fastboot boot`)

`SKIP="kernel loader images boot"` leaves steps out. Profiles: `common/devices/{redwood,instantnoodle,qemu-virt}.conf`
(`_template.conf` documents every variable). `common/hackios build <device>` is the same through the one-command front end.

## Host requirements

Linux x86-64, ~12 GB free disk for the QEMU path (the whole working tree on the author's machine is far larger), 4 GB RAM.

```sh
# Arch
sudo pacman -S --needed git python 7zip lz4 gcc qemu-system-aarch64 qemu-ui-gtk edk2-aarch64 \
     base-devel curl clang lld perl xz
# Debian/Ubuntu: git python3 7zip liblz4-dev gcc qemu-system-arm qemu-efi-aarch64 build-essential curl clang lld perl xz-utils
```

7-Zip must be **23 or newer** (it reads APFS). `common/setup.sh` checks the host and prints what is missing for which step.
On the NixOS development host there is no `clang`/`cc` outside `nix-shell` (`shell.nix`): use
`nix-shell --run '...'` for kernel, Wi-Fi driver and `xl enter` (mkxnuimg needs clang) steps; `nix-shell -p gcc` for host tests;
Pillow/numpy via `nix-shell -p python3Packages.pillow python3Packages.numpy` for `tools/look` and `mkbootlogo.py`.

## Workspace

HackiOS is **two git repositories in one directory** — see [architecture.md](architecture.md#two-repositories-one-tree). `common/setup.sh`
checks `hackios-xnu-userland` out into the same tree as `.userland.git` (`common/ugit` is its git).
`tools/new-workspace.sh` makes another working copy with the iOS extractions as hard links. **Never make `ios165/rootfs`,
`cryptex`, `cryptex-app` symlinks to another copy's tree**: `build.sh` refuses, because a link made `rm -rf` delete 491
LaunchDaemons etc. from the main extraction on 19 October 2026.

`ios165/fetch.sh` downloads the iOS 16.5 IPSW (7.4 GB) from Apple's CDN and extracts what is needed (5-15 min). No Apple file
is in either repository.

## Kernel

```sh
xnu/apply.sh             # ~6 s: tag xnu-8796.121.2 of apple-oss-distributions/xnu + our changes -> xnu-src/xnu-8796.121.2 (branch redwood, changes uncommitted); also xnu/hfs-stage.sh
xnu-deps/setup.sh        # ~30 s: dependency sources, mig/unifdef/tcsh, the SDK overlay, two libraries
tools/mk-ath11k.sh       # optional once: Linux 7.0.14 + FreeBSD 15.1 sources at pinned hashes, builds ath11k (phones' Wi-Fi)
tools/mk-wlfw.sh         #   ... and its firmware-service message tables
./xnu-build.sh -j8 -k    # ~45 s on 12 threads
```

Needs [theos](https://theos.dev/docs/installation-linux) in `~/theos` (or `$THEOS`) for cctools (ld64, nm, otool, strip) and
`iPhoneOS16.5.sdk`. Result: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`; `xl run` and
`qemu/run-ios.sh` take it from there (from `prebuilt/` only when absent).

- **`-k` is required.** The build "errors" (`check_all_exports`, three symbolsets, 217 `exported name not in import list` about
  DriverKit RPC names from the `iig` stubs) all happen after linking; make exits 2; judge by the file's date.
- The path to the repository must not contain spaces. `xnu-deps/host/bin/xnu-clang` calls `/usr/bin/clang` (tested with 22.1).
- After changing kernel headers that outside libraries see (pthread kext, firehose): `xnu-deps/setup.sh --rebuild-libs`.
- If the `iig` undefined-symbol set changes: `xnu-deps/gen-iig-stubs.py xnu-deps/iig-impl/undefined-all.txt xnu-deps/iig-impl qemu/kernel.syms`, then `rm -rf BUILD`.
- For `qemu/sym.py`: `nm -n .../mach.development.bcm2837.unstripped > qemu/kernel.syms`.
- The RELEASE configuration does not build (abandoned). `DEVELOPMENT_ARM64_BCM2837` is the one.
- `tools/t` rebuilds the kernel only if sources are newer, and reports a build failure when the binary is older than sources.

### Editing the kernel: the export trap

You edit `xnu-src/xnu-8796.121.2` (Apple's tree, branch `redwood`). `xnu/export.sh` regenerates `xnu/hackios.patch` and `xnu/files/`
**from the whole tree**, not from your changes. If the tree fell behind the committed patch (somebody changed the kernel after
you set yours up) the export silently rolls their work back, and the sign is a kernel that **builds and crashes at boot right
after `Stackshot Succeeded`**. Before exporting, bring the tree to the current patch (`git checkout -- .` in `xnu-src`, then
`git apply xnu/hackios.patch && cp -a xnu/files/. .`), or paste a single diff into the patch by hand and say so. Several
sessions can share one tree: check `git -C xnu-src status` first. Hosts: `xnu/hfs-stage.sh` fetches Apple's HFS+ (`bsd/hk_hfs`)
and `xnu/export.sh` leaves it out.

## Loader

`xnuloader/build.sh` (clang + lld-link) → `xnuloader/loader.efi`. `OUT=loader-ci.efi xnuloader/build.sh` builds without touching it.
`mkidentity.py` makes `identity.h` from an iPhone's device tree. `common/flash/ownerkey.py`/`hackios key init` pins an owner key
into the loader (see INSTALL.md). A new SoC needs a `struct platform` in `loader.c` ([PORTING.md](PORTING.md)).

## UEFI

Not needed to run: `prebuilt/uefi/` has it and `mkxnuimg.py` takes it. Rebuilding: [../uefi/README.md](../uefi/README.md)
(`uefi/apply.sh` clones Mu-Silicium ~3 GB and applies the port; `python3 build_uefi.py -d redwood|instantnoodle -r RELEASE`).
A from-scratch rebuild by those instructions has not been re-run.

## Images (userland repository)

```sh
ios165/fetch.sh                       # once
REST=1 userland/build.sh              # root.apfs + system.apfs
GL=1 REST=1 userland/build.sh         # + Mesa OpenGL ES for the window server (needs tools/mk-mesa.sh; phone with gpu=1)
GL=1 REST=1 FLASHBOOT=1 CRYPTEXES=flash userland/build.sh     # OnePlus 8: flashboot.apfs for the flash-root boot
```

Variables are documented at the top of `userland/build.sh` (`RC`, `JOBS`, `REST`, `WIFID`, `OSLOG`, `INJECT`, `STOREFRONT`, `EXTRA`, `EXTRA_SHARE`,
`RC_JOBS`, `GL`, `ADSP`, `FLASHBOOT`, `CRYPTEXES`, `PREBUILT`, `CHECK`, `SYSTEM`, `SHARE`). Without the cross toolchain the build takes our programs
from `prebuilt/userland`. `tools/mk-mesa.sh` fetches Mesa 25.0.7 and applies `tools/mesa/*.patch`. `ADSP=1` puts the 20 MB audio DSP
firmware in the small image, not beside `GL=1` (they do not fit the 64 MB the loader takes with the kernel). Files of the userland
repo need `common/ugit add -f` (shared whitelist `.gitignore`). `tools/t` caches userland builds in `out/tcache/<hash>`.

### Mesa's Vulkan driver, turnip (Metal over Vulkan, TODO T38 (12))
Not part of `userland/build.sh` yet; test roots only. It is built in a second build dir beside gallium's
(`xnu-deps/build/mesa/build-softpipe-freedreno` stays as it is), from the same tree `tools/mk-mesa.sh` makes. The tree must
carry `tools/mesa/turnip-darwin.patch` (applied after `freedreno-darwin.patch`, by name order) and the current
`tools/mesa/hackios-drm`. In the build container, with `glslangValidator` installed (`apt-get install glslang-tools`):

```sh
cd /build/xnu-deps/build/mesa && export PATH=$PWD/venv/bin:/build/xnu-deps/host/bin:$PATH
meson setup build-turnip mesa-25.0.7 --cross-file cross.ini -Dbuildtype=release -Dwrap_mode=nofallback -Dprefix=/hackios \
  -Dlibdir=lib -Db_ndebug=true -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=false -Dllvm=disabled -Dshared-glapi=disabled -Dgles1=disabled \
  -Dgles2=disabled -Dopengl=false -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
  -Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false
ninja -C build-turnip              # -> build-turnip/src/freedreno/vulkan/libvulkan_freedreno.dylib
```

turnip's copy of `hk_drm.c` is built with `HK_DRM_SYNCOBJ`: `DRM_CAP_SYNCOBJ` is 1 there and its sync objects are kept in
the process; gallium's copy says 0, as before. The driver links with `-undefined dynamic_lookup`: Mach-O does not leave the
entrypoint tables' weak references NULL as ELF does. Its test is `userland/src/vk_test.c` (milestone 1 passed, run 002111).
On Darwin the same library also carries the AIR frontend and `tools/mesa/air/air_vk.c` (the rwa_* contract over Vulkan,
for Metal over Vulkan; copy it into the tree's `src/compiler/air` with the rest of `tools/mesa/air`), with
`src/freedreno/vulkan/tu_hackios.cc`. It exports only what `tu_hackios_exports.txt` lists: `vk_icd*`, `rwa_*` and
`rwa_vk_counts`. `air_metal_test` for the phone is built twice from `tools/mesa/air/air_metal_test.c`, once linked to
each library (`-Wl,-rpath,/hackios/lib` for turnip's `@rpath` install name).
To regenerate `tools/mesa/turnip-darwin.patch`, note that the "pristine" copy used so far (`mesa-pristine` in a session's
scratchpad) is NOT the untouched archive: it is the archive with the earlier patches already applied, so
`freedreno-darwin.patch` does not go on it again. Check a new patch on a fresh copy with `--fuzz=0` and diff the result
against the tree. The test binaries also need `-framework IOSurface -framework CoreFoundation` (air_metal_test, metal_drv_test).
The M5a probes are `tools/air/m5a_*.ll`, each `llvm-as-18 -o userland/obj/NAME.bc tools/air/NAME.ll` in the container, and go to
`/hackios/share/probes` in a test root; `air_metal_test --MODE --pixels DIR` reads them. To check them and the models of
`tools/mesa/air/draw_model.h` without the phone: in the container, a native `meson setup` of the same Mesa tree (`--buildtype=release
-Db_ndebug=true -Dgallium-drivers=softpipe,freedreno -Dvulkan-drivers= -Dllvm=disabled`, the rest as `tools/air/mk-host.sh`, about 90 MB and
a minute; libdrm-dev and pkgconf can be unpacked with `dpkg -x` to a temporary directory), `ninja src/gallium/drivers/freedreno/libhackiosair.so`,
and `cc -I tools/mesa/air tools/mesa/air/air_metal_test.c -lhackiosair`: softpipe draws every case but the base instance in
`[[instance_id]]` and a shallow line's rule. It says nothing about air_vk.c, which only the phone can run.
The M5b probes are the same kind: `tools/air/m5b_*.ll` (m5b_f2d, _f2dimp, _f2darr, _f1d, _f1darr, _fcube, _fcubearr, _f3d, _fcmp,
_fread, _freadu, _freadi, _fmsarr, _fuint, _fsint, _fown, _fownb, _fownlod, with m5a_fsrc), for `--textures`, `--pixel-formats` and `--blit`
with `--pixels DIR`; the models are `tools/mesa/air/tex_model.h`. On that host build `--draw-blend --pixels` is 128 ok, `--blit --pixels` 47, and
`--pixel-formats --pixels` 154 and 12 `gallium-mirror` rows (formats 40-43, which that library keeps mirrored on purpose: `draw-mirror`) with its 18 refusals; `--textures --pixels` 144, and five `sampler-own-*` differences: every sampler of a module past its first reads
black there (hypothesis, not checked: softpipe takes the texture unit from the sampler's index).
The goal boots' scripts are in `tools/metal/loop/` (M5e; they name their scratchpad in `S`): `build_m5.sh` builds Turnip and this round's userland in the container, without tools/mk-mesa.sh; `mk_root.sh`, `goal.sh`, `shots_when.sh`, `freshness.sh`, `drawall_regress.py`. Test tools built there and not by `userland/build.sh`: `hkhttpd` (a page on the phone's loopback; `tools/metal/mk_testpage.py`), `threadsof`, `vt_test`, `rwtouch` (step `alive NAME`). The frontend's disk cache is `/var/tmp/hk-air`; `AIR_CACHE_DIR` moves it, on the host it is off unless set.

The M5c rows are one source for real Metal and the phone: on the Mac `clang -fobjc-arc -framework Foundation -framework Metal -o tools/air/mtlref/airdump tools/air/mtlref/airdump.m`,
then `tools/air/mtlref/airdump tools/air/m5c.metal out/msl/m5c.metallib` (this Mac's Metal compiler; no Xcode Metal toolchain needed, it asks the running system), copied to
`share/probes` in a test root for `metal_drv_test --m5c`, and `tools/air/mtlref/mtlref --m5c out/msl/m5c.metallib` for real Metal. To read what it made: split the modules at their 0x0B17C0DE wrappers, `llvm-dis-18` in the container.
The Mac's library is refused as it stands on the phone (`newLibraryWithData`: "This library format is not supported on this platform"): `python3 tools/air/mtlref/ios_metallib.py IN OUT` rewrites the platform and version bytes of the header and each module's VERS tag to iOS 16's, and the phone then loads it (verified on phone, run 124507). Only those bytes change; the bitcode is what this Mac's compiler made (air64_v29 forms).
Reading what the phone's compiler made of a shader: `air_metal_test_vk --gpu --ir3-debug disasm ...` (the flag after `--gpu`; it is ir3's IR3_SHADER_DEBUG for that run, and the print goes to the log). Seeing a picture on the panel with nothing else drawing: `metal_drv_test --stop-compositor --picture N` (`--stop-compositor` first; it stops backboardd for the hold and continues it after, and prints both).
A frontend check needs no phone and no libdrm: in the container `meson setup /tmp/hf <mesa tree> --buildtype=release -Db_ndebug=true -Dplatforms= -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dllvm=disabled` and the rest of tools/air/mk-host.sh's options, `ninja src/compiler/air/air2nir`, `air2nir [--not-defined | --constants I=HEX,...] [--report] FILE`. `--not-defined` translates as a specialized function given no values does, every function constant unset, which is the reading to use for what a system process will be refused.
`libvulkan_freedreno.dylib` exports 51 since M5c (`rwa_compute_create_constants`, `rwa_module_function_constants`), and 49 symbols since M5b (three `vk_icd*`, 46 `rwa_*`): `rwa_texture_view_create` went into
`tu_hackios_exports.txt` and `tools/mesa/turnip-darwin.patch`, by hand, one line in the patch's hunk for that new file.

## Vendor firmware (never in the repository)

Taken from the phone itself; `userland/build.sh` prints which went into the image. `FIRMWARE` in the profile lists the directories
(`fw-gpu`, `fw-wifi`, `fw-audio`, `fw-haptic`, place them next to the workspace).

```sh
# POCO X5 Pro, from a running Android:
adb pull /vendor/firmware/a660_{gmu.bin,sqe.fw,zap.mdt,zap.b00,zap.b01,zap.b02} fw-gpu/
adb pull /vendor/firmware_mnt/image/wpss.{mdt,b02,b03,b04,b05,b06,b07} fw-wifi/
adb pull /vendor/firmware_mnt/image/adsp.{mdt,b02,...,b36} fw-audio/
adb pull /vendor/firmware/aw8697_haptic.bin fw-haptic/
```

OnePlus 8: from a backup of its vendor partition and `persist`: see [devices/instantnoodle.md](devices/instantnoodle.md).
Wi-Fi also needs `fw-wifi/mac` (the radio's address, one line) and optionally `fw-wifi/network`. **Never print the MAC or commit any firmware.**

## prebuilt/

`prebuilt/` holds the kernel, loader, UEFI pieces and our programs built from these repositories so the QEMU path needs no
toolchain. After rebuilding: `tools/update-prebuilt.sh`. `prebuilt/SHA256SUMS` lists them; `prebuilt/NOTICES.md` has third-party notices.
Prebuilt files lag the source: `prebuilt/mach.development.bcm2837` predates `tools/sleep-test`'s `sleep_test` boot-arg.

## Licensing of what is built

`xnu/` is a derivative of XNU (APSL 2.0); the kernel also contains ath11k (BSD-3-Clause-Clear) and FreeBSD LinuxKPI code (BSD-2-Clause);
libzbitmap (Corellium, MIT), libdeflate 1.24 (MIT). Apple's HFS+ is fetched at build time. See [../prebuilt/NOTICES.md](../prebuilt/NOTICES.md).
