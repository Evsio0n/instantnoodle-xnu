#!/bin/bash
# fetch-ref.sh: the OnePlus 8 Pro port's files port.py and voice_port.py read, fetched from its repository at the commit the port was
# made from (github.com/dekefake/hackios-op8pro, e5631cd2523a8a7a897618a3535fe2a08b1672dc, Apple Public Source License 2.0) into ref-pro/
# under the names the two scripts expect. ref-pro/ is not in git: the Pro's files are its authors', and what is built from them here
# (instantnoodle_cell.c, hk_mhi.*, cell_net.*, the call additions in overlay/) names them at its head.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REV=e5631cd2523a8a7a897618a3535fe2a08b1672dc
RAW=https://raw.githubusercontent.com/dekefake/hackios-op8pro/$REV
mkdir -p "$HERE/ref-pro/audio"
get() { curl -fsSL "$RAW/$1" -o "$HERE/ref-pro/$2"; echo "$2"; }
get xnu/files/bsd/dev/instantnoodle_modem.c   pro_modem.c
get xnu/files/bsd/dev/hk_mhi.c                pro_hk_mhi.c
get xnu/files/bsd/sys/hk_mhi.h                pro_hk_mhi.h
get xnu/files/bsd/dev/hk_net.c                pro_hk_net.c
get xnu/files/bsd/sys/hk_net.h                pro_hk_net.h
get docs/research/modem-sdx55.md              pro-modem.md
get xnu/files/bsd/dev/hk_adsp.c               audio/pro_hk_adsp.c
get xnu/files/bsd/dev/hk_audio.c              audio/pro_hk_audio.c
get xnu/files/bsd/sys/hk_audio.h              audio/pro_hk_audio.h
get xnu/files/bsd/dev/hk_mic.c                audio/pro_hk_mic.c
get xnu/files/bsd/dev/hk_sound.c              audio/pro_hk_sound.c
get xnu/files/bsd/dev/hk_speaker.c            audio/pro_hk_speaker.c
get xnu/files/bsd/dev/instantnoodle_speaker.c audio/pro_instantnoodle_speaker.c
get docs/subsystems/audio.md                  audio/pro_audio.md
