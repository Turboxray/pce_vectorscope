"""Build a 512-colour composite-video palette for the HuC6260 (VCE).

Usage:  python make_vce_palette.py [output.pal]

Output is 1536 bytes: 512 entries of R,G,B, indexed the way the VCE colour
table is (index = G<<6 | R<<3 | B), which is the layout Mesen 2 loads.

Where each number comes from
  ROM rules ............ verified 512/512 against the dumped lookup table
                         (HuC6260_RGB_Lookups.xlsx)
  RY_GAIN .............. service-manual mixing resistors, B-Y 8.2k / R-Y 6.2k.
                         The vectorscope photo gives 1.29-1.32 independently.
  CHROMA_IRE_PER_COUNT . vectorscope photo (IMG_2902.jpg), scaled to
                         burst = 20 IRE peak. Readings span 2.11-2.18.
  ROTATION_DEG ......... vectorscope photo, pattern vs burst (+-1.5 deg)
  LUMA_SPAN_IRE ........ waveform-monitor photo (IMG_2232.jpg) with sync set
                         to -40 IRE: white bar at 89 IRE, black at 0 (no
                         7.5 IRE pedestal). The six colour bars sit within
                         1.7 IRE of Y code / 31 * 89, so luma is linear in
                         the code.
  SATURATION ........... taste only. 1.0 = as measured.

Chroma is scaled to burst and luma to sync, i.e. a set with both AGC and
automatic chroma control. On that photo burst reads 37-40 IRE p-p with sync
at 40, so a set without chroma control would show up to 7% less colour.
"""
import math
import sys

LUMA_SPAN_IRE = 89.0           # measured, see above
CHROMA_IRE_PER_COUNT = 2.14    # one B-Y count, as fundamental amplitude
RY_GAIN = 8.2 / 6.2            # R-Y is mixed in this much stronger than B-Y
ROTATION_DEG = -8.5            # whole chroma pattern relative to burst
SATURATION = 1.0


def rom(r, g, b):
    """The VCE matrix ROM: 5-bit luma, signed B-Y and R-Y counts."""
    y = int(math.floor(1.33 * r + 2.61 * g + 0.49 * b + 0.5))
    return y, 2 * b - r - g, 2 * (r - g)


def decode(r, g, b):
    y_code, by, ry = rom(r, g, b)
    # chroma on the chip's own axes, in IRE
    u0 = SATURATION * CHROMA_IRE_PER_COUNT * by
    v0 = SATURATION * CHROMA_IRE_PER_COUNT * RY_GAIN * ry
    t = math.radians(ROTATION_DEG)
    u = u0 * math.cos(t) - v0 * math.sin(t)
    v = u0 * math.sin(t) + v0 * math.cos(t)
    # standard NTSC decode, black -> 0 and white -> 1
    y = y_code / 31.0
    b_y = (u / LUMA_SPAN_IRE) / 0.492
    r_y = (v / LUMA_SPAN_IRE) / 0.877
    g_y = -(0.299 * r_y + 0.114 * b_y) / 0.587
    return tuple(min(255, max(0, int(round(255 * (y + d))))) for d in (r_y, g_y, b_y))


def build():
    pal = []
    for i in range(512):
        g, r, b = (i >> 6) & 7, (i >> 3) & 7, i & 7
        pal.append(decode(r, g, b))
    return pal


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "vce_composite_firstpass.pal"
    pal = build()
    with open(out, "wb") as f:
        for rgb in pal:
            f.write(bytes(rgb))
    print("wrote", out, "(%d colours)" % len(pal))
