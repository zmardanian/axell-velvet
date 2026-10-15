"""De-esser prototypes (STFT domain, 32/48 kHz, 2 ms hop), both with 4 ms look-ahead:
 classic(): split-band compressor - side-chain 4-10 kHz level over a fixed threshold, ratio, the whole band above
            3.5 kHz turned down (the SPL / allround-style design).
 axell():   sibilance probability x water-filling to a target relative to the voice body (tracks verse / chorus),
            per-band gains so only the sibilance's own peaks are flattened; range-limited; smoothed in dB."""
import numpy as np
from scipy import signal as sg
from scipy.ndimage import uniform_filter1d
WIN_S, HOP_S = 0.016, 0.002
EDGES = [2000, 2800, 3600, 4500, 5600, 7000, 8500, 10000, 12000, 14500, 17000]

def stft(x, fs):
    win = int(round(WIN_S * fs)); hop = int(round(HOP_S * fs))
    f, t, Z = sg.stft(x, fs, window="hann", nperseg=win, noverlap=win - hop, boundary="zeros", padded=True)
    return f, t, Z, win, hop

def istft(Z, fs, win, hop, n):
    _, y = sg.istft(Z, fs, window="hann", nperseg=win, noverlap=win - hop, boundary=True)
    return y[:n]

def smooth_db(g_db, att_frames, rel_frames, look):
    """gain smoothing in dB: instant-ish attack (look-ahead min), exponential release"""
    g = np.minimum.reduce([np.roll(g_db, -k, axis=-1) for k in range(look + 1)])   # look-ahead: react before it arrives
    out = np.empty_like(g); s = np.zeros(g.shape[:-1])
    ka, kr = 1 / max(att_frames, 1), 1 / max(rel_frames, 1)
    for i in range(g.shape[-1]):
        x = g[..., i]
        s = np.where(x < s, s + (x - s) * ka, s + (x - s) * kr)
        out[..., i] = s
    return out

def classic(x, fs, thr_db, ratio=4.0, lo=3500):
    f, t, Z, win, hop = stft(x, fs); P = np.abs(Z) ** 2
    sc = 10 * np.log10(P[(f >= 4000) & (f < 10000)].sum(0) + 1e-14)
    over = np.maximum(sc - thr_db, 0)
    g = -over * (1 - 1 / ratio)
    g = smooth_db(g[None], 1, 25, 2)[0]
    G = np.ones_like(P); G[f >= lo] = 10 ** (g / 20)
    return istft(Z * G, fs, win, hop, len(x)), g

def axell(x, fs, prob, amount_db, range_db=12.0, rel_ms=30.0):
    """prob: sibilance probability per frame (same hop). amount_db: allowed sibilant HF level relative to the voice body."""
    f, t, Z, win, hop = stft(x, fs); P = np.abs(Z) ** 2
    n = min(P.shape[1], len(prob)); prob = np.pad(prob[:n], (0, P.shape[1] - n))
    body = P[(f >= 100) & (f < 1500)].sum(0) + 1e-14
    # voice-body reference: running level of voiced, loud frames (tracks verse / chorus), tau 300 ms
    lv = 10 * np.log10(body); ref = np.empty_like(lv); r = lv[0]; k = 1 / (0.3 / HOP_S)
    loud = lv > np.percentile(lv, 90) - 20
    for i in range(len(lv)):
        if loud[i] and prob[i] < 0.3: r += (lv[i] - r) * k
        ref[i] = r
    B = len(EDGES) - 1
    E = np.stack([P[(f >= EDGES[b]) & (f < EDGES[b + 1])].sum(0) + 1e-14 for b in range(B)])
    target = 10 ** ((ref + amount_db) / 10)                 # allowed total HF energy
    tot = E.sum(0)
    g = np.zeros((B, P.shape[1]))
    for i in np.where(tot > target)[0]:
        e = np.sort(E[:, i])[::-1]
        # water-filling: cap C with sum(min(E, C)) = target
        cs = np.cumsum(e); C = None
        for k2 in range(B):
            c = (target[i] - (cs[-1] - cs[k2])) / (k2 + 1)
            if c >= (e[k2 + 1] if k2 + 1 < B else 0):
                C = c; break
        C = max(C, 1e-14)
        g[:, i] = np.minimum(0, 10 * np.log10(C / E[:, i]))
    g = np.maximum(g, -range_db) * prob[None, :]                # probability-weighted, range-limited
    g = smooth_db(g, 1, max(1, int(rel_ms / 1000 / HOP_S)), 2)
    G = np.ones_like(P)
    for b in range(B):
        m = (f >= EDGES[b]) & (f < EDGES[b + 1]); G[m] = 10 ** (g[b] / 20)[None, :]
    return istft(Z * G, fs, win, hop, len(x)), g
