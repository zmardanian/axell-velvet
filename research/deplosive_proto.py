"""Velvet 2.0 PLOSIVE - prototype of the de-plosive (the C++ code in Source/Plosive.h follows it sample for sample).

A plosive pop is a short burst of air on the capsule at a p / b: a low-frequency thump that, on the artist's takes,
stands 5-40 dB above the voice's own low end for 50-110 ms (research notes in DECISIONS). The voice's low end is itself
loud on a close vocal (chest voice, proximity effect), so a level threshold would also hit loud low notes. Instead:
  - two low bands: LOW (below 90 Hz) and MID (90-250 Hz), each against the voice BODY (250 Hz - 4 kHz);
  - the voice's own ratio of each band to the body is learned while it sings or raps (slow average over voiced
    frames, frozen during pops);
  - a pop is the part of a band that stands above that learned ratio + a margin; exactly that excess is removed:
    y = x - (1 - gLow) LP(x) - (1 - gMid) BP(x), with the gains taken ahead of time (look-ahead) and released in 30 ms.
With nothing to remove both gains are 1 and the output IS the input.
usage: python3 deplosive_proto.py take.wav [out.wav]"""
import numpy as np
from scipy import signal as sg

LOOK_MS, REL_MS, MARGIN_DB, FLOOR_DB, Q, ENV_MS = 8.0, 30.0, 8.0, -30.0, 0.8, 15.0

def env(v, fs, ms):
    a = np.exp(-1.0 / (ms * 1e-3 * fs))
    return np.sqrt(sg.lfilter([1 - a], [1, -a], v * v) + 1e-24)

HOP_MS = 1.0                  # control rate (the C++ code runs the same at 1 ms)

def deplosive(x, fs, amount=100.0, margin=MARGIN_DB, return_gains=False):
    lo = sg.sosfilt(sg.butter(2, 90, "low", fs=fs, output="sos"), x)
    mid = sg.sosfilt(sg.butter(2, [90, 250], "band", fs=fs, output="sos"), x)
    body = sg.sosfilt(sg.butter(2, [250, 4000], "band", fs=fs, output="sos"), x)
    el, em, eb = env(lo, fs, ENV_MS), env(mid, fs, ENV_MS), env(body, fs, ENV_MS)
    H = int(HOP_MS * 1e-3 * fs); idx = np.arange(0, len(x), H)
    L, M, B = 20 * np.log10(el[idx]), 20 * np.log10(em[idx]), 20 * np.log10(eb[idx])
    n = len(idx); gl = np.zeros(n); gm = np.zeros(n)
    up = 3.0 * HOP_MS / 1000.0; dn = up * (1 - Q) / Q   # the voice's band ratios: Q-quantile tracking, 3 dB/s up
    kpk = 1 - np.exp(-HOP_MS / 3000.0)       # body peak: instant up, 3 s down
    al = am = None; peak = -120.0
    for i in range(n):
        peak = B[i] if B[i] > peak else peak + kpk * (B[i] - peak)
        rl, rm = L[i] - B[i], M[i] - B[i]
        voiced = B[i] > peak - 35
        if voiced and al is None:
            al, am = rl, rm
        if al is None:
            continue
        if voiced:   # quantile tracking: a 60 ms pop moves it by 0.2 dB at most; a bassy voice is learned in a second or two
            al += up if rl > al else -dn; am += up if rm > am else -dn
        # a pop: the band stands above the voice's own ratio + margin, and is loud in absolute terms
        if L[i] > peak - 30:
            gl[i] = -max(0.0, rl - (al + margin))
        if M[i] > peak - 30:
            gm[i] = -max(0.0, rm - (am + margin))
    gl = np.maximum(gl * amount / 100, FLOOR_DB); gm = np.maximum(gm * amount / 100, FLOOR_DB)
    look = int(round(LOOK_MS / HOP_MS)); r = np.exp(-HOP_MS / REL_MS)
    for g in (gl, gm):
        g[:] = np.array([g[i:i + look + 1].min() for i in range(n)])      # look-ahead
        s = 0.0
        for i in range(n):                                                # release
            s = g[i] if g[i] < s else g[i] + r * (s - g[i]); g[i] = s
    t = np.arange(len(x))
    GL = np.interp(t, idx, gl); GM = np.interp(t, idx, gm)
    y = x - (1 - 10 ** (GL / 20)) * lo - (1 - 10 ** (GM / 20)) * mid
    return (y, GL, GM) if return_gains else y

def pops(x, fs):
    """the plosive pops of a take (as counted for DECISIONS): short (< 110 ms at -10 dB) 20-90 Hz bursts above the voice
    body that coincide with a word start (the voice rises > 6 dB across them)"""
    bp = lambda lo, hi: sg.sosfiltfilt(sg.butter(4, [lo, hi], "band", fs=fs, output="sos"), x)
    lf, body = bp(20, 90), bp(150, 4000)
    el, eb = env(lf, fs, 5), env(body, fs, 20)
    top = np.percentile(20 * np.log10(eb), 99); rel = 20 * np.log10(el / eb); lvl = 20 * np.log10(el)
    cand = np.flatnonzero((rel > 0) & (lvl > top - 25)); ev = []; last = -10 ** 9
    for k in cand:
        if k - last > int(0.2 * fs):
            ev.append(k)
        last = k
    out = []
    for k in ev:
        seg = el[max(0, k - int(0.02 * fs)):k + int(0.3 * fs)]; pk = seg.argmax(); d = np.sum(seg > seg[pk] * 10 ** (-0.5)) / fs * 1000
        b0 = 20 * np.log10(eb[max(0, k - int(0.12 * fs)):max(1, k - int(0.02 * fs))].mean()) - top
        b1 = 20 * np.log10(eb[k:k + int(0.15 * fs)].mean()) - top
        if d < 110 and b1 - b0 > 6:
            out.append(k)
    return out

def report(x, y, fs):
    """pop excess removed (20-250 Hz energy in the 120 ms after each pop, before vs after) and the low-end change
    everywhere else on voiced material (the price: thinning)"""
    lowb = lambda v: sg.sosfiltfilt(sg.butter(4, [20, 250], "band", fs=fs, output="sos"), v)
    lx, ly = lowb(x), lowb(y)
    P = pops(x, fs); mask = np.zeros(len(x), bool); red = []
    for k in P:
        a, b = max(0, k - int(0.01 * fs)), k + int(0.12 * fs); mask[max(0, k - int(0.05 * fs)):k + int(0.2 * fs)] = True
        red.append(10 * np.log10(np.sum(ly[a:b] ** 2) / np.sum(lx[a:b] ** 2)))
    body = sg.sosfiltfilt(sg.butter(4, [150, 4000], "band", fs=fs, output="sos"), x); eb = env(body, fs, 20)
    voiced = (~mask) & (20 * np.log10(eb) > np.percentile(20 * np.log10(eb), 99) - 30)
    thin = 10 * np.log10(np.sum(ly[voiced] ** 2) / np.sum(lx[voiced] ** 2))
    return len(P), red, thin

if __name__ == "__main__":
    import sys, os, glob
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import diag_realtake as D
    import takes
    files = sys.argv[1:] or takes.real_takes()
    allred, allthin = [], []
    for f in files:
        fs, x = D.read(f)
        y, gl, gm = deplosive(x, fs, return_gains=True)
        n, red, thin = report(x, y, fs)
        allred += red; allthin.append(thin)
        print(f"{os.path.basename(f)[:38]:38s} pops {n:2d}: low band (20-250 Hz) after the pop {np.median(red) if red else 0:+6.1f} dB median"
              f" (least {max(red) if red else 0:+5.1f}) | elsewhere on the voice {thin:+.2f} dB | cut > 1 dB {100 * np.mean((gl < -1) | (gm < -1)):.1f} % of the time")
    print(f"ALL: {len(allred)} pops, median {np.median(allred):+.1f} dB, the least-treated {max(allred):+.1f} dB | voice low end elsewhere: mean {np.mean(allthin):+.2f} dB")
