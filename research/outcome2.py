"""Realistic benchmark: ONE setting per de-esser for every voice, test signals with slow level rides (+-8 dB over a
few seconds, like verse/chorus and singer movement). Measures how consistently each sibilant ends up relative to its
own voice (target), what happens to everything else away from sibilant edges, and over-cutting."""
import sys, numpy as np
sys.path.insert(0, ".")
from data import utterances, load, SIBILANT, STOPS, FRICATIVE_OTHER
from features2 import features
import detector as det
import deess_proto as dp
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))
SPK = ("slt", "bdl", "jmk")
D = {}
for s in SPK:
    d = np.load(f"{DATA}/feat2_{s}.npz", allow_pickle=True); D[s] = (d["F"], d["cls"], d["near"])
AMOUNT = -6.0
rng = np.random.default_rng(5)

def ride(n, fs):
    """smooth random level ride, +-8 dB, 2-6 s period"""
    t = np.arange(n) / fs; ph = rng.uniform(0, 2 * np.pi); per = rng.uniform(2, 6)
    return 10 ** ((8 * np.sin(2 * np.pi * t / per + ph)) / 20)

def bands(x, fs):
    f, t, Z, win, hop = dp.stft(x, fs); P = np.abs(Z) ** 2
    return 10 * np.log10(P[(f >= 2000) & (f < 16000)].sum(0) + 1e-14), 10 * np.log10(P[(f >= 100) & (f < 1500)].sum(0) + 1e-14)

# one model for the whole benchmark: fitted on slt + bdl; jmk is never seen; classic threshold tuned once on slt
X = np.concatenate([det.design(D[s][0], det.adapt(D[s][0])) for s in ("slt", "bdl")])
c = np.concatenate([D[s][1] for s in ("slt", "bdl")]); nn = np.concatenate([D[s][2] for s in ("slt", "bdl")])
use = ~nn & (c != ""); M = det.fit(X[use], np.isin(c[use], list(SIBILANT)).astype(float))

def run(spk, step, classic_th=None, variant={}):
    S_after, S_before, coll = [], [], {"vowels/voiced": [], "stops": [], "f/th/v": []}
    for name, w, l in list(utterances(spk))[::step]:
        fs, x, egg, ph = load(spk, w, l)
        x = x * ride(len(x), fs)
        if classic_th is not None:
            y = dp.classic(x, fs, classic_th)[0]
        else:
            t, F = features(x, fs); p = det.prob(det.design(F, det.adapt(F)), M)
            y = dp.axell(x, fs, p ** variant.get("pow", 1.0), AMOUNT, rel_ms=variant.get("rel", 30))[0]
        h0, b0 = bands(x, fs); h1, _ = bands(y, fs); n = min(len(h0), len(h1))
        tt = np.arange(n) * dp.HOP_S; cl = np.full(n, "", object); edge = np.zeros(n, bool)
        for a, b, q in ph:
            cl[(tt >= a) & (tt < b)] = q
            if q in SIBILANT: edge |= (np.abs(tt - a) < 0.04) | (np.abs(tt - b) < 0.04)
        # running voice-body level (same definition for both, from the input): the reference a sibilant is judged against
        sib = np.isin(cl, list(SIBILANT)) & (h0[:n] > np.percentile(h0[:n], 50))
        voiced = ~np.isin(cl, list(SIBILANT | STOPS | FRICATIVE_OTHER | {"pau", ""}))
        refb = np.interp(tt, tt[voiced], uniform_filter_med(b0[:n], voiced)) if voiced.any() else b0[:n]
        S_before.append((h0[:n] - refb)[sib]); S_after.append((h1[:n] - refb)[sib])
        d = np.abs(h1[:n] - h0[:n]); act = h0[:n] > np.percentile(h0[:n], 30)
        coll["vowels/voiced"].append(d[voiced & ~edge & act]); coll["stops"].append(d[np.isin(cl, list(STOPS)) & ~edge & act])
        coll["f/th/v"].append(d[np.isin(cl, list(FRICATIVE_OTHER)) & ~edge & act])
    return np.concatenate(S_before), np.concatenate(S_after), {k: np.concatenate(v) for k, v in coll.items()}

def uniform_filter_med(v, mask):
    from scipy.ndimage import uniform_filter1d
    return uniform_filter1d(v[mask], 75)                                      # 150 ms running mean of voiced frames

V2 = {"pow": 2.0, "rel": 12}
# classic: one threshold, tuned so its effect on slt's loud esses (90th percentile after) equals Axell's on slt
_, aa, _ = run("slt", 16, variant=V2); goal = np.percentile(aa, 90)
lo, hi = -80.0, 20.0
for it in range(14):
    th = 0.5 * (lo + hi); _, sa, _ = run("slt", 16, classic_th=th)
    if np.percentile(sa, 90) > goal: hi = th
    else: lo = th
print(f"classic threshold tuned once on slt: {th:.1f} dB (slt loud-ess goal {goal:+.1f} dB re voice body)")
HARSH = -3.0
for spk in SPK:
    for name, kw in (("Axell", dict(variant=V2)), ("classic", dict(classic_th=th))):
        b, a, coll = run(spk, 10, **kw)
        soft = b < -15
        print(f"  {spk} {name:7s}: loud esses (90th) {np.percentile(b,90):+.1f} -> {np.percentile(a,90):+.1f} dB | harsh esses (> {HARSH:+.0f} dB re voice) "
              f"{100*(b>HARSH).mean():.1f}% -> {100*(a>HARSH).mean():.1f}% | soft esses touched (were < -15): mean {np.mean(a[soft]-b[soft]):+.2f} dB | "
              f"collateral mean |dB|: " + ", ".join(f"{k} {v.mean():.2f}" for k, v in coll.items()))
