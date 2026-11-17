"""Predict encoder-side artifacts of pixel-rate chroma modulated with no low-pass (VCE model).

Assumptions, none of them measured: no filtering at all; pixel edges line up
with the B-Y carrier edges (as Epson FIG. 2(c) draws it); 1365 master clocks
per line, so the carrier slips 180 degrees against the pixel grid every line.
Edit A and B (R, G, B, each 0-7) to try other colour pairs.
"""
import numpy as np
OS = 16                                  # samples per master tick (21.47727 MHz)
FM = 21.47727e6
W = 89.0                                 # white, IRE
def rom(r, g, b):
    return int(np.floor(1.33*r + 2.61*g + 0.49*b + 0.5)), 2*b - r - g, 2*(r - g)
def carriers(nticks, start_tick, three_level):
    t = (np.arange(nticks * OS) + 0.5) / OS + start_tick          # time in ticks
    def car(shift):
        ph = (t - shift) % 6.0
        if three_level:                   # + over 0.5-2.5, 0, - over 3.5-5.5, 0
            return np.where((ph > 0.5) & (ph < 2.5), 1.0, 0.0) - np.where((ph > 3.5) & (ph < 5.5), 1.0, 0.0)
        return np.where(ph < 3.0, 1.0, -1.0)
    return t, car(0.0), car(1.5)          # B-Y carrier, R-Y carrier 1.5 ticks later
def line(colours, ticks_per_px, start_tick, three_level, chroma=True):
    n = len(colours) * ticks_per_px
    t, cb, cr = carriers(n, start_tick, three_level)
    f1 = (2*np.sqrt(3)/np.pi) if three_level else 4/np.pi
    step = 2.14 / f1                      # IRE per count so that the fundamental is 2.14 IRE per count
    y = np.repeat([rom(*c)[0] / 31 * W for c in colours], ticks_per_px * OS)
    by = np.repeat([rom(*c)[1] for c in colours], ticks_per_px * OS)
    ry = np.repeat([rom(*c)[2] for c in colours], ticks_per_px * OS)
    s = y + (step * (by * cb + 1.3226 * ry * cr) if chroma else 0)
    return s
def lowpass(s, fc):
    S_ = np.fft.rfft(s); f = np.fft.rfftfreq(len(s), 1 / (FM * OS))
    S_[f > fc] = 0
    return np.fft.irfft(S_, len(s))
def comp(s, f0):
    S_ = np.fft.rfft(s) / len(s) * 2; f = np.fft.rfftfreq(len(s), 1 / (FM * OS))
    k = np.argmin(np.abs(f - f0)); return abs(S_[k]), f[k]

A, B = (7, 0, 7), (0, 5, 0)              # magenta and a dark green: same luma code, very different chroma
print("A", A, rom(*A), " B", B, rom(*B))
for three in (True, False):
    print("\n=== %s carrier ===" % ("three-level" if three else "plain square"))
    # 5.37 MHz: alternating single pixels, 240 px (multiple of 6 px so the window holds whole beat periods)
    for lineno in (0, 1):
        s = line([A, B] * 120, 4, 3 * lineno, three)
        a, f = comp(s, FM / 24)
        lp = lowpass(s, 2.0e6)
        mid = lp[len(lp)//4: 3*len(lp)//4]
        print("5.37 MHz A/B stripes, line %d: component at %.3f MHz = %.1f IRE peak ; luma band (<2 MHz) ripple %.1f IRE p-p, mean %.1f (flat-colour luma %.1f)" % (lineno, f/1e6, a, mid.max() - mid.min(), mid.mean(), rom(*A)[0]/31*W))
    # phase of the beat on two successive lines
    ph = []
    for lineno in (0, 1):
        s = line([A, B] * 120, 4, 3 * lineno, three)
        S_ = np.fft.rfft(s); f = np.fft.rfftfreq(len(s), 1 / (FM * OS)); k = np.argmin(np.abs(f - FM / 24)); ph.append(np.degrees(np.angle(S_[k])))
    print("   beat phase line 0 -> line 1: %.0f deg apart (stripes);" % ((ph[1] - ph[0]) % 360), end=" ")
    ph2 = []
    for lineno in (0, 1):
        cols = ([A, B] * 120) if lineno == 0 else ([B, A] * 120)
        s = line(cols, 4, 3 * lineno, three)
        S_ = np.fft.rfft(s); f = np.fft.rfftfreq(len(s), 1 / (FM * OS)); k = np.argmin(np.abs(f - FM / 24)); ph2.append(np.degrees(np.angle(S_[k])))
    print("%.0f deg apart (checkerboard)" % ((ph2[1] - ph2[0]) % 360))
    # 7.16 MHz: alternating single pixels
    for lineno in (0, 1):
        s = line([A, B] * 120, 3, 3 * lineno, three)
        print("7.16 MHz A/B stripes, line %d: mean level %.1f IRE (flat colour %.1f)" % (lineno, s.mean(), rom(*A)[0]/31*W))
    # single-axis versions to see which carrier leaks at 7.16
    for name, P, Q in (("B-Y only", (0, 0, 7), (7, 7, 0)), ("R-Y only (R vs G, equal B-Y)", (7, 0, 0), (0, 7, 0))):
        s0 = line([P, Q] * 120, 3, 0, three); s0n = line([P, Q] * 120, 3, 0, three, chroma=False)
        print("   7.16 MHz %s: chroma-caused mean shift %.1f IRE" % (name, s0.mean() - s0n.mean()))
    # 10.74 MHz alternating
    s = line([A, B] * 120, 2, 0, three)
    S_ = np.abs(np.fft.rfft(s)) / len(s) * 2; fr = np.fft.rfftfreq(len(s), 1 / (FM * OS))
    band = (fr > 0.05e6) & (fr < 2.5e6); k = np.argmax(np.where(band, S_, 0))
    print("10.74 MHz A/B stripes: strongest component below 2.5 MHz: %.1f IRE at %.3f MHz" % (S_[k], fr[k] / 1e6))
    # isolated 1-pixel line of A on a field of B at 5.37 MHz: luma-band blip vs x position
    out = []
    for x in range(6):
        cols = [B] * 60; cols[30 + x] = A
        s = line(cols, 4, 0, three); ref = line([B] * 60, 4, 0, three)
        d = lowpass(s - ref, 2.0e6)
        out.append("%+.1f" % (d[np.argmax(np.abs(d))]))
    print("5.37 MHz single A pixel on B, luma-band blip (IRE) for x = 0..5:", " ".join(out))
