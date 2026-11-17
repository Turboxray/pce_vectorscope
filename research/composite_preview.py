"""Render what the VCE composite model predicts a TV would show.

Usage:  python composite_preview.py [output_folder]

Encoder model (same assumptions as artifact_sim.py, none of them measured
directly): pixel-rate chroma codes modulated with no low-pass, three-level
carriers at 6 x fsc, pixel edges aligned with the B-Y carrier, 1365 master
clocks per line so the carrier flips 180 degrees every line.
Levels come from make_vce_palette.py (white 89 IRE, 2.14 IRE per B-Y count,
R-Y mixed 1.32 x stronger, pattern rotated -8.5 degrees).

TV model: ideal burst lock, chroma bandwidth about 0.5 MHz, luma taken either
through an LC trap at the subcarrier or through a two-line comb filter, then a
picture tube with a finite spot size (SPOT_SIGMA_NS). Frames are averaged as
light, after clipping at black, which is how the eye fuses 263-line mode.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import make_vce_palette as pal

OS = 4                        # samples per master clock tick
FM = 21.47727e6               # master clock
FSC = FM / 6
PAD = 40                      # blank pixels each side of a line
G = pal.RY_GAIN
K = pal.CHROMA_IRE_PER_COUNT
WHITE = pal.LUMA_SPAN_IRE
ROT = math.radians(pal.ROTATION_DEG)

# Notch-filter TV, luma path: an LC trap at the subcarrier followed by the
# video amplifier roll-off. Both are causal, so any ripple comes after an edge,
# never before it.
LUMA_BW = 3.0e6               # video amplifier, -3 dB
TRAP_Q = 2.0                  # subcarrier trap; lower = wider notch, softer luma

# Picture tube. The spot is a gaussian this many nanoseconds wide (sigma).
# 75 ns leaves about 25% contrast at 3.58 MHz and 45% on one-pixel stripes at
# 5.37 MHz, which is roughly a consumer set. About 30 for a studio monitor,
# 0 to see the decoder output with no tube at all.
SPOT_SIGMA_NS = 75.0
GAMMA = 2.2

# ---------------------------------------------------------------- test picture
def colour(r, g, b):
    return (r, g, b)

BLACK, WHITE_C = (0, 0, 0), (7, 7, 7)
RED, GREEN, BLUE = (7, 0, 0), (0, 7, 0), (0, 0, 7)
YELLOW, CYAN, MAGENTA = (7, 7, 0), (0, 7, 7), (7, 0, 7)
DKGREEN, ORANGE, GREY = (0, 5, 0), (7, 4, 0), (4, 4, 4)
SKY1, SKY2 = (0, 3, 7), (0, 5, 7)


def text_mask(s, width, height=12):
    """1-bit text, strokes one pixel wide."""
    im = Image.new("1", (width, height), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    try:
        font = ImageFont.load_default(size=10)
    except TypeError:
        font = ImageFont.load_default()
    d.text((2, 0), s, fill=1, font=font)
    return np.array(im, dtype=bool)


def build_picture():
    W = 256
    rows = []

    def band(h, fn):
        a = np.zeros((h, W, 3), dtype=np.int16)
        for y in range(h):
            for x in range(W):
                a[y, x] = fn(x, y)
        rows.append(a)

    def spacer(h=4):
        rows.append(np.zeros((h, W, 3), dtype=np.int16))

    bars = [WHITE_C, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK]
    band(14, lambda x, y: bars[x // 32])
    spacer()

    def dither(a, b):
        def fn(x, y):
            if x < 84:                       # one-pixel vertical stripes
                return a if x % 2 == 0 else b
            if x < 88:
                return BLACK
            if x < 172:                      # checkerboard
                return a if (x + y) % 2 == 0 else b
            if x < 176:
                return BLACK
            return a if y % 2 == 0 else b    # one-line horizontal stripes
        return fn

    for a, b in ((MAGENTA, DKGREEN), (RED, BLUE), (YELLOW, ORANGE), (SKY1, SKY2)):
        band(20, dither(a, b))
        spacer()

    def lines(fg, bg):
        return lambda x, y: fg if (x % 7 == 3) else bg
    line_sets = [(RED, BLACK), (RED, WHITE_C), (BLUE, BLACK), (GREEN, BLACK), (WHITE_C, BLACK)]
    def line_band(x, y):
        fg, bg = line_sets[min(x // 52, 4)]
        return fg if (x % 7 == 3 and x % 52 < 48) else (bg if x % 52 < 48 else BLACK)
    band(16, line_band)
    spacer()

    def text_band(s, fg, bg):
        m = text_mask(s, W)
        a = np.zeros((m.shape[0] + 2, W, 3), dtype=np.int16)
        a[:, :] = bg
        a[1:-1][m] = fg
        rows.append(a)
        spacer(3)

    text_start = sum(r.shape[0] for r in rows)
    text_band("PURE RED on black: PC Engine HuC6260 0123456789", RED, BLACK)
    text_band("PURE RED on white: PC Engine HuC6260 0123456789", RED, WHITE_C)
    text_band("PURE RED on grey:  PC Engine HuC6260 0123456789", RED, GREY)
    text_band("PURE RED on blue:  PC Engine HuC6260 0123456789", RED, BLUE)
    text_band("PURE BLUE on black: PC Engine HuC6260 012345678", BLUE, BLACK)
    text_band("WHITE on black (luma only, for reference) 01234", WHITE_C, BLACK)
    return np.concatenate(rows, axis=0), text_start


# ---------------------------------------------------------------- encoder
def rom_planes(pic):
    r, g, b = pic[..., 0].astype(float), pic[..., 1].astype(float), pic[..., 2].astype(float)
    y = np.floor(1.33 * r + 2.61 * g + 0.49 * b + 0.5) / 31.0 * WHITE
    return y, 2 * b - r - g, 2 * (r - g)


def timebase(height, nticks, frame):
    t = (np.arange(nticks * OS) + 0.5) / OS
    start = (3 * (np.arange(height) + frame)) % 6          # 1365 ticks per line -> 3 ticks slip
    return t[None, :] + start[:, None]


def gauss_lp(x, f3db):
    f = np.fft.rfftfreq(x.shape[1], 1 / (FM * OS))
    sigma = f3db / math.sqrt(math.log(2))
    return np.fft.irfft(np.fft.rfft(x, axis=1) * np.exp(-(f / sigma) ** 2 / 2), x.shape[1], axis=1)


def encode(pic, ticks_per_px, frame=0, band_limited=False):
    h, w, _ = pic.shape
    padded = np.zeros((h, w + 2 * PAD, 3), dtype=pic.dtype)
    padded[:, PAD:PAD + w] = pic
    y, by, ry = (np.repeat(p, ticks_per_px * OS, axis=1) for p in rom_planes(padded))
    t = timebase(h, (w + 2 * PAD) * ticks_per_px, frame)
    ang = 2 * np.pi * t / 6.0
    if band_limited:
        by, ry = gauss_lp(by, 1.3e6), gauss_lp(ry, 1.3e6)
        chroma = K * (by * np.sin(ang) - G * ry * np.cos(ang))
    else:
        f1 = 2 * math.sqrt(3) / math.pi
        def three(shift):
            ph = (t - shift) % 6.0
            return ((ph > 0.5) & (ph < 2.5)).astype(float) - ((ph > 3.5) & (ph < 5.5)).astype(float)
        chroma = (K / f1) * (by * three(0.0) + G * ry * three(1.5))
    return y + chroma, t


# ---------------------------------------------------------------- TV
def trap_luma_response(f):
    """LC trap at the subcarrier times a 3rd-order Bessel low-pass, with their
    low-frequency delay taken out again (the set's luma delay line does that)."""
    w = 1j * 1.7557 * f / LUMA_BW
    lowpass = 15 / (w ** 3 + 6 * w ** 2 + 15 * w + 15)
    x = f / FSC
    trap = (1 - x ** 2) / (1 - x ** 2 + 1j * x / TRAP_Q)
    delay = 1.7557 / (2 * np.pi * LUMA_BW) + 1 / (TRAP_Q * 2 * np.pi * FSC)
    return lowpass * trap * np.exp(2j * np.pi * f * delay)


def display(frames, spot_ns=None):
    """Picture tube and eye: clip at black and white, turn into light, blur by
    the spot, average the frames the eye fuses, and go back to signal values."""
    spot_ns = SPOT_SIGMA_NS if spot_ns is None else spot_ns
    light = sum(np.clip(fr, 0, 1) ** GAMMA for fr in frames) / len(frames)
    if spot_ns > 0:
        f3db = math.sqrt(math.log(2)) / (2 * np.pi * spot_ns * 1e-9)
        light = np.stack([gauss_lp(light[..., k], f3db) for k in range(3)], axis=-1)
    return np.clip(light, 0, 1) ** (1 / GAMMA)


def decode(s, t, comb=False):
    n = s.shape[1]
    f = np.fft.rfftfreq(n, 1 / (FM * OS))
    S = np.fft.rfft(s, axis=1)
    lum_lp = np.exp(-(f / (4.2e6 / math.sqrt(math.log(2)))) ** 2 / 2)
    if comb:
        wide = np.exp(-((f - FSC) / 1.2e6) ** 2 / 2)
        hp = np.fft.irfft(S * wide, n, axis=1)
        prev = np.vstack([hp[1:2], hp[:-1]])
        c = (hp - prev) / 2
        y = np.fft.irfft(S * lum_lp, n, axis=1) - gauss_lp(c, 4.2e6)
    else:
        bp = np.exp(-((f - FSC) / 0.6e6) ** 2 / 2)
        c = np.fft.irfft(S * bp, n, axis=1)
        y = np.fft.irfft(S * trap_luma_response(f), n, axis=1)
    ang = 2 * np.pi * t / 6.0
    u0 = gauss_lp(2 * c * np.sin(ang), 0.6e6)
    v0 = gauss_lp(-2 * c * np.cos(ang), 0.6e6)
    u = u0 * math.cos(ROT) - v0 * math.sin(ROT)
    v = u0 * math.sin(ROT) + v0 * math.cos(ROT)
    yy = y / WHITE
    b_y = (u / WHITE) / 0.492
    r_y = (v / WHITE) / 0.877
    g_y = -(0.299 * r_y + 0.114 * b_y) / 0.587
    rgb = np.stack([yy + r_y, yy + g_y, yy + b_y], axis=-1)
    return rgb


def to_image(rgb, ticks_per_px, vscale=4):
    """One output pixel per master tick horizontally, padding removed."""
    h, n, _ = rgb.shape
    per_tick = rgb.reshape(h, n // OS, OS, 3).mean(axis=2)
    a = per_tick[:, PAD * ticks_per_px: -PAD * ticks_per_px]
    a = np.clip(np.round(a * 255), 0, 255).astype(np.uint8)
    return Image.fromarray(np.repeat(a, vscale, axis=0))


def palette_image(pic, ticks_per_px, vscale=4):
    table = np.array(pal.build(), dtype=np.uint8)
    idx = (pic[..., 1] << 6) | (pic[..., 0] << 3) | pic[..., 2]
    a = table[idx]
    return Image.fromarray(np.repeat(np.repeat(a, ticks_per_px, axis=1), vscale, axis=0))


def sheet(panels):
    w = max(im.size[0] for _, im in panels)
    hh = sum(im.size[1] + 20 for _, im in panels)
    out = Image.new("RGB", (w, hh), (40, 40, 40))
    d = ImageDraw.Draw(out)
    y = 0
    for label, im in panels:
        d.text((4, y + 4), label, fill=(235, 235, 235))
        out.paste(im, (0, y + 20))
        y += im.size[1] + 20
    return out


def render(pic, ticks_per_px, out, tag, text_start=0, vscale=4):
    s0, t0 = encode(pic, ticks_per_px, frame=0)
    s1, t1 = encode(pic, ticks_per_px, frame=1)
    sf, tf = encode(pic, ticks_per_px, frame=0, band_limited=True)
    notch0 = decode(s0, t0)
    notch1 = decode(s1, t1)
    panels = [
        ("1. Palette only (what the emulator shows today)", palette_image(pic, ticks_per_px)),
        ("2. VCE model -> TV with notch filter, one frame (262-line mode looks like this all the time)", to_image(display([notch0]), ticks_per_px)),
        ("3. Same, two frames fused by the eye (263-line mode)", to_image(display([notch0, notch1]), ticks_per_px)),
        ("4. VCE model -> TV with two-line comb filter, one frame", to_image(display([decode(s0, t0, comb=True)]), ticks_per_px)),
        ("5. Band-limited encoder -> TV with notch filter (what a conventional encoder would give)", to_image(display([decode(sf, tf)]), ticks_per_px)),
        ("6. As 2 with no picture-tube blur (the decoder's electrical output)", to_image(display([notch0], spot_ns=0), ticks_per_px)),
    ]
    for i, (label, im) in enumerate(panels, 1):
        im.save(os.path.join(out, "%s_panel%d.png" % (tag, i)))
    sheet(panels).save(os.path.join(out, "%s_all_panels.png" % tag))
    # the text rows of every panel, enlarged 2x, for side-by-side reading
    y0 = text_start * vscale
    crops = []
    for label, im in panels:
        c = im.crop((0, y0, im.size[0], im.size[1]))
        crops.append((label, c.resize((c.size[0] * 2, c.size[1] * 2), Image.NEAREST)))
    sheet(crops).save(os.path.join(out, "%s_text_compare.png" % tag))
    return panels


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "preview"
    os.makedirs(out, exist_ok=True)
    pic, text_start = build_picture()
    print("test picture", pic.shape[1], "x", pic.shape[0])
    # check: flat colours through the whole encode/decode chain must land on the palette
    table = pal.build()
    worst = 0
    for c in (WHITE_C, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, ORANGE, GREY):
        flat = np.tile(np.array(c, dtype=np.int16), (4, 64, 1))
        s, t = encode(flat, 4)
        got = np.clip(np.round(decode(s, t)[1, s.shape[1] // 2 - 64: s.shape[1] // 2 + 64].mean(axis=0) * 255), 0, 255)
        want = table[c[1] << 6 | c[0] << 3 | c[2]]
        worst = max(worst, int(np.abs(got - np.array(want)).max()))
    print("flat colours vs palette file: worst channel difference", worst)
    for tpp, tag in ((4, "dot5.37MHz"), (3, "dot7.16MHz")):
        render(pic, tpp, out, tag, text_start)
        print("wrote", tag)
