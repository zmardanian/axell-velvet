"""YIN voicing / pitch (de Cheveigne & Kawahara, JASA 111(4), 2002): cumulative-mean-normalised difference function.
Returns per frame (hop) the aperiodicity d' at the best lag (0 = perfectly periodic, ~1 = noise) and f0."""
import numpy as np
from scipy import signal as sg

def yin(x, fs, hop, fmin=60.0, fmax=500.0, win_s=0.025, thresh=0.15):
    x = sg.sosfilt(sg.butter(4, 1500, fs=fs, output="sos"), x)   # periodicity lives below ~1.5 kHz; HF noise would mask it
    W = int(win_s * fs); tmax = int(fs / fmin) + 1; tmin = int(fs / fmax)
    n = (len(x) - W - tmax) // hop
    ap = np.ones(max(n, 0)); f0 = np.zeros(max(n, 0))
    for i in range(n):
        s = x[i * hop:i * hop + W + tmax]
        a = s[:W]
        e0 = np.dot(a, a)
        if e0 < 1e-12:
            continue
        # difference function via energies and cross-correlation
        c = np.correlate(s[:W + tmax], a, "valid")[:tmax]           # sum a[j] * s[j + tau]
        cs = np.cumsum(np.concatenate([[0.0], s * s]))
        et = cs[np.arange(tmax) + W] - cs[np.arange(tmax)]
        d = e0 + et - 2 * c
        d[0] = 0
        cm = np.cumsum(d[1:]) / np.arange(1, tmax)
        dn = np.ones(tmax); dn[1:] = d[1:] / np.maximum(cm, 1e-20)
        seg = dn[tmin:tmax]
        below = np.where(seg < thresh)[0]
        if len(below):
            k = below[0]
            while k + 1 < len(seg) and seg[k + 1] < seg[k]:
                k += 1
        else:
            k = np.argmin(seg)
        tau = k + tmin
        if 1 <= tau < tmax - 1:                                       # parabolic refinement
            y0, y1, y2 = dn[tau - 1], dn[tau], dn[tau + 1]
            den = y0 - 2 * y1 + y2
            tau = tau + (float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5)) if den > 0 else 0.0)
        ap[i] = min(1.0, dn[int(round(tau))])
        f0[i] = fs / tau
    return ap, f0
