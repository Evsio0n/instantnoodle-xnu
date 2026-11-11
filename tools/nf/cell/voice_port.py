#!/usr/bin/env python3
"""voice_port.py: the sound of a call for the cellular kernel. The OnePlus 8 Pro port (dekefake/hackios-op8pro @ e5631cd, APSL 2.0) added it to the same
audio drivers this tree has (hk_adsp.c, hk_mic.c, hk_sound.c, hk_speaker.c, instantnoodle_speaker.c, sys/hk_audio.h): voice-base/ holds this tree's files as
the cellular kernel's objects were built from them (the container's nf-tree), ref-pro/audio/ the Pro's (fetch-ref.sh). This makes the overlay's copies: ours, with the
Pro's call additions and nothing else of the Pro's (its other differences are its board's: the DSP's memory, the firmware's place, an older sound path).
Names of the modem driver are the cellular driver's (hk_cell_*). It stops if a place it changes is not found exactly once. Run, then build.py."""
import re
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE, PRO, OUT = HERE / 'voice-base', HERE / 'ref-pro/audio', HERE / 'overlay/xnu/files/bsd'
NAMES = (('hk_modem_dma_sync', 'hk_cell_dma_sync'), ('hk_modem_generation', 'hk_cell_generation'), ('hk_modem_map_contig', 'hk_cell_map_contig'),
         ('hk_modem_qrtr_from_dsp', 'hk_cell_qrtr_from_dsp'), ('hk_modem_sat_from_dsp', 'hk_cell_sat_from_dsp'))


def once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit('voice_port.py: %d of %r' % (text.count(old), old[:70]))
    return text.replace(old, new)


def between(text, start, end):
    """The Pro's text from `start` up to, not with, `end`."""
    if text.count(start) != 1:
        raise SystemExit('voice_port.py: %d of %r in the Pro\'s' % (text.count(start), start[:70]))
    a = text.index(start)
    return text[a:text.index(end, a)]


def hunks(base, theirs, keep):
    """`base` with those hunks of diff -u base theirs applied that `keep` says yes to."""
    d = subprocess.run(['diff', '-u', str(base), str(theirs)], capture_output=True, text=True).stdout
    parts = re.split(r'(?m)^(?=@@ )', d)
    chosen = [h for h in parts[1:] if keep(h)]
    with tempfile.TemporaryDirectory() as t:
        patch = Path(t) / 'p'
        patch.write_text(parts[0] + ''.join(chosen))
        out = Path(t) / 'out'
        r = subprocess.run(['patch', '-s', '-o', str(out), str(base), str(patch)], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit('voice_port.py: patch: ' + r.stdout + r.stderr)
        return out.read_text(), len(chosen), len(parts) - 1


def names(text):
    for old, new in NAMES:
        text = text.replace(old, new)
    return text


# sys/hk_audio.h: the Pro's declarations added (the two lines of ours it has not got stay)
h, n, of = hunks(BASE / 'hk_audio.h', PRO / 'pro_hk_audio.h', lambda x: not any(l.startswith('-') for l in x.split('\n')[1:]))
assert n == 3, n
# (the amplifiers' two new functions: in the Pro's tree sys/instantnoodle.h declares them, a header too many units of this kernel were built with to change here)
h = once(h, 'void hk_audio_dsp_faults(void);\n', 'void hk_audio_dsp_faults(void);\nbool instantnoodle_speaker_route(uint32_t route);\nbool instantnoodle_speaker_earpiece_gain(uint32_t gain);\n')
(OUT / 'sys').mkdir(parents=True, exist_ok=True)
(OUT / 'sys/hk_audio.h').write_text(h)

# dev/hk_adsp.c: the satellite's GLINK channel, the bridge's functions (unused: voice_bridge stays off), a session's handle in an answer, the voice services' answers
VOICE = ('CHANNEL_SAT', 'hk_modem_sat_from_dsp', 'hk_modem_qrtr_from_dsp', '0x4005u', 'uint16_t port;', 'The voice services (MVM 9', 'APR not for us', 'apr_answer.port', 'answer->port')
a, n, of = hunks(BASE / 'hk_adsp.c', PRO / 'pro_hk_adsp.c', lambda x: any(w in x for w in VOICE) and 'adsp_memory = 0x8fe00000u' not in x)
assert n == 11, (n, of)
a = names(a)
# (the last hunk of the Pro's mixes its board's memory with the channel's switch: the switch alone. The channel is opened for voice=3 and above, or as voice_sat says)
a = once(a, '\t\tquiet = !option("adsp_loud");', '''\t\t{
\t\t\t/* the DSP's own road to the modem (a call's sound): with voice=3 and above (the default with cell=1), or as voice_sat=0/1 says; without it the channel has no name and is never opened */
\t\t\tuint32_t voice = 0, sat = 0;
\t\t\tbool said = PE_parse_boot_argn("voice_sat", &sat, sizeof(sat));

\t\t\tif (!PE_parse_boot_argn("voice", &voice, sizeof(voice))) {
\t\t\t\tuint32_t cell = 0;

\t\t\t\tvoice = PE_parse_boot_argn("cell", &cell, sizeof(cell)) && cell != 0 ? 3 : 0;       /* (as hk_mic.c's voice_steps()) */
\t\t\t}
\t\t\tif (!(said ? sat != 0 : voice >= 3)) {
\t\t\t\tglink.channel[CHANNEL_SAT].name = "";
\t\t\t}
\t\t}
\t\tquiet = !option("adsp_loud");''')
(OUT / 'dev').mkdir(parents=True, exist_ok=True)
(OUT / 'dev/hk_adsp.c').write_text(a)

# dev/hk_sound.c: ours (its queue and its parking are newer than the Pro's), with the Pro's four functions and their places in the watcher
p = (PRO / 'pro_hk_sound.c').read_text()
s = (BASE / 'hk_sound.c').read_text()
s = once(s, 'void\nhk_audio_dsp_memory_put(void)\n', between(p, "/*\n * Memory of somebody else's, in one piece, shown to the DSP for good", 'void\nhk_audio_dsp_memory_put(void)\n') + 'void\nhk_audio_dsp_memory_put(void)\n')
WATCH = '/*\n * The amplifiers on while there is sound, at the volume iOS has set'
s = once(s, WATCH, between(p, "/* A call's sound (hk_mic.c, voice=3)", WATCH) + WATCH)
s = once(s, '\tuint32_t onsets_said = 0;\n\n\twhile (!stopping) {\n', '\tuint32_t onsets_said = 0;\n\tuint32_t route_now = HK_SPEAKER_BOTH;\n\n\twhile (!stopping) {\n'
         + between(p, '\t\tif (voice_hold) {\n\t\t\tlast_sound_us = now_us();', '\t\tuint64_t now = now_us(), sounded = last_sound_us;'))
(OUT / 'dev/hk_sound.c').write_text(s)

# dev/hk_mic.c, dev/hk_speaker.c, dev/instantnoodle_speaker.c: the Pro's are ours with the call's part added (checked here: nothing of ours is missing from them
# but the amplifiers' two lines the Pro made over, set_field() and the start's condition)
for name, gone in (('hk_mic.c', 0), ('hk_speaker.c', 0), ('instantnoodle_speaker.c', 2)):
    d = subprocess.run(['diff', str(BASE / name), str(PRO / ('pro_' + name))], capture_output=True, text=True).stdout
    lost = sum(1 for l in d.split('\n') if l.startswith('<'))
    if lost != gone:
        raise SystemExit('voice_port.py: %s: %d lines of ours are not in the Pro\'s (%d expected)' % (name, lost, gone))
    text = names((PRO / ('pro_' + name)).read_text())
    if name == 'hk_mic.c':
        # voice=N: not given, it is 3 with the cellular path (cell=1) and 0 without: the loader's line has no room for one more argument in the daily
        # system's, and a call without its sound is of no use. voice=0 turns it off.
        text = once(text, 'static void\nvoice_thread(__unused void *parameter, __unused wait_result_t result)\n', '''/* voice=N; not given: 3 with the cellular path (cell=1), 0 without */
static uint32_t
voice_steps(void)
{
	uint32_t voice = 0, cell = 0;

	if (!PE_parse_boot_argn("voice", &voice, sizeof(voice))) {
		voice = PE_parse_boot_argn("cell", &cell, sizeof(cell)) && cell != 0 ? 3 : 0;
	}
	return voice;
}

static void
voice_thread(__unused void *parameter, __unused wait_result_t result)
''')
        text = once(text, '\tPE_parse_boot_argn("voice", &voice, sizeof(voice));\n\tbzero(&answer, sizeof(answer));\n', '\tvoice = voice_steps();\n\tbzero(&answer, sizeof(answer));\n')
        text = once(text, '\t\tuint32_t voice = 0;\n\n\t\tif (PE_parse_boot_argn("voice", &voice, sizeof(voice)) && voice != 0 && kernel_thread_start(', '\t\tuint32_t voice = voice_steps();\n\n\t\tif (voice != 0 && kernel_thread_start(')
    (OUT / 'dev' / name).write_text(text)
print('voice: overlay/xnu/files/bsd: sys/hk_audio.h, dev/hk_adsp.c, hk_sound.c, hk_mic.c, hk_speaker.c, instantnoodle_speaker.c')
