#!/usr/bin/env python3
"""mk_testpage.py DIR: a web page for testing Safari with no network (userland/src/hkhttpd.c serves it on the phone).

Everything in it is made here: the words, and PNGs computed from formulas. It has what makes WebKit do different work: long
text, a sticky header, gradients, shadows, rounded corners, opacity, a running CSS animation (a layer of its own), a 2D
canvas, inline SVG, and pictures. The colour bars at the top have known values, so a screenshot says whether the channels
are in order: red, green, blue, yellow, white, black, left to right.
"""
import math
import os
import struct
import sys
import zlib


def png(path, width, height, pixel):
    """An 8-bit RGB PNG of pixel(x, y) -> (r, g, b)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        f.write(chunk(b"IEND", b""))


def hue(h):
    h = (h % 1.0) * 6
    i = int(h)
    f = h - i
    return [(1, f, 0), (1 - f, 1, 0), (0, 1, f), (0, 1 - f, 1), (f, 0, 1), (1, 0, 1 - f)][i % 6]


def gradient(x, y):
    return (x * 255 // 959, y * 255 // 639, 128)


def checker(x, y):
    return (235, 235, 235) if (x // 40 + y // 40) % 2 else (40, 60, 120)


def wheel(x, y):
    dx, dy = x - 320, y - 320
    r = math.hypot(dx, dy) / 320
    if r > 1:
        return (250, 250, 250)
    c = hue(math.atan2(dy, dx) / (2 * math.pi))
    return tuple(int(255 * (1 - r + r * v)) for v in c)


def rings(x, y):
    v = int(127.5 + 127.5 * math.sin(math.hypot(x - 480, y - 320) / 9))
    return (v, 255 - v, 90)


PICTURES = [("gradient.png", 960, 640, gradient), ("checker.png", 960, 640, checker),
            ("wheel.png", 640, 640, wheel), ("rings.png", 960, 640, rings)]

PARAGRAPHS = [
    "This page was written for a test and is about the test. It asks one thing of the system that shows it: to draw "
    "ordinary web content, the kind a person reads every day, with a graphics driver that is being written alongside it.",
    "A browser's content does not reach the screen in one step. The web content process decides what goes where, the GPU "
    "process paints it into surfaces, and the compositor puts those surfaces on the display. Each of them has to agree on "
    "what a surface holds and when it has changed.",
    "Scrolling is the test that matters most. The surfaces already painted move under the finger with no painting at all, "
    "and new ones arrive from the edge as the page goes by. A compositor that keeps up makes this feel like paper, and one "
    "that falls behind shows it at once.",
    "Text is the other half. Small letters at many sizes, in black on white, show every mistake in filtering and every "
    "pixel that is out of place. If these words are sharp at the size you are reading them, sampling is doing its job.",
    "The pictures below are computed from formulas, so nothing about them belongs to anyone. A gradient shows banding, a "
    "checkerboard shows scaling, a colour wheel shows the order of the channels, and rings show moire when something "
    "is resampled badly.",
]

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HackiOS test page</title>
<style>
body { margin: 0; font: 17px/1.5 -apple-system, Helvetica, sans-serif; color: #1c1c1e; background: #fff; }
header { position: sticky; top: 0; z-index: 2; padding: 10px 16px; color: #fff; font-weight: 600;
  background: linear-gradient(90deg, #0a84ff, #bf5af2); box-shadow: 0 2px 10px rgba(0,0,0,.35); }
main { padding: 0 16px 40px; }
.bars { display: flex; height: 60px; margin: 16px 0; border-radius: 12px; overflow: hidden; }
.bars div { flex: 1; }
.card { margin: 18px 0; padding: 14px; border-radius: 14px; background: #f2f2f7; box-shadow: 0 3px 12px rgba(0,0,0,.18); }
.spin { width: 64px; height: 64px; margin: 18px auto; border-radius: 16px;
  background: conic-gradient(#ff3b30, #ffcc00, #34c759, #0a84ff, #ff3b30); animation: spin 3s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
.glass { opacity: .55; }
img, canvas, svg { display: block; width: 100%; height: auto; border-radius: 10px; }
table { width: 100%; border-collapse: collapse; font-size: 14px; }
td, th { padding: 6px; border-bottom: 1px solid #c7c7cc; text-align: left; }
</style></head><body>
<header>HackiOS test page</header>
<main>
<h1>Drawn over Metal, over Vulkan</h1>
<div class="bars"><div style="background:#f00"></div><div style="background:#0f0"></div><div style="background:#00f"></div>
<div style="background:#ff0"></div><div style="background:#fff"></div><div style="background:#000"></div></div>
<div class="spin"></div>
@SECTIONS@
<div class="card"><h2>A canvas</h2><canvas id="c" width="960" height="480"></canvas></div>
<div class="card"><h2>Vector drawing</h2>
<svg viewBox="0 0 300 120"><defs><linearGradient id="g"><stop offset="0" stop-color="#ff9500"/><stop offset="1" stop-color="#ff2d55"/></linearGradient></defs>
<rect x="4" y="4" width="292" height="112" rx="16" fill="url(#g)"/><circle cx="70" cy="60" r="38" fill="#fff" fill-opacity=".8"/>
<path d="M130 90 L170 30 L210 90 Z" fill="#5856d6"/><text x="222" y="68" font-size="20" fill="#fff">SVG</text></svg></div>
<div class="card glass"><h2>Half see-through</h2><p>This card is drawn at an opacity of 0.55.</p></div>
<div class="card"><h2>A table</h2><table><tr><th>Process</th><th>Its part</th></tr>
<tr><td>Web content</td><td>Layout and what is on each layer</td></tr><tr><td>GPU</td><td>Painting into surfaces</td></tr>
<tr><td>Compositor</td><td>Surfaces on the display</td></tr></table></div>
<p>The end of the page.</p>
</main>
<script>
var c = document.getElementById("c"), g = c.getContext("2d");
for (var i = 0; i < 48; i++) {
  g.fillStyle = "hsl(" + i * 7.5 + ",85%,55%)";
  g.fillRect(i * 20, 240 - 200 * Math.abs(Math.sin(i / 7)), 20, 400 * Math.abs(Math.sin(i / 7)));
}
g.fillStyle = "#000"; g.font = "48px sans-serif"; g.fillText("canvas 2D", 300, 256);
</script>
</body></html>
"""


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, width, height, pixel in PICTURES:
        png(os.path.join(out, name), width, height, pixel)
    sections = []
    for i, words in enumerate(PARAGRAPHS * 3):
        sections.append('<div class="card"><p>%s</p></div>' % words)
        name = PICTURES[i % len(PICTURES)][0]
        sections.append('<div class="card"><img src="%s" alt="a computed test picture"></div>' % name)
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(PAGE.replace("@SECTIONS@", "\n".join(sections)))


if __name__ == "__main__":
    main()
