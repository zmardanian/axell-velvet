"""Outcome benchmark on held-out voices: Axell vs classic split-band de-esser at EQUAL average reduction of the
sibilants' high band. Measures what else each one changes (collateral) and over-reduction (lisp)."""
import sys, numpy as np
sys.path.insert(0, ".")
from data import utterances, load, SIBILANT, STOPS, FRICATIVE_OTHER
from features2 import features
import detector as det
import deess_proto as dp
from scipy import signal as sg
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))
SPK = ("slt", "bdl", "jmk")
D = {}
for s in SPK:
    d = np.load(f"{DATA}/feat2_{s}.npz", allow_pickle=True); D[s] = (d["F"], d["cls"], d["near"])
AMOUNT = -6.0   # allowed sibilance HF level re the voice body (dB)

def hf_db(x, fs):
    f, t, Z, win, hop = dp.stft(x, fs); P = np.abs(Z) ** 2
    return 10 * np.log10(P[(f >= 2000) & (f < 16000)].sum(0) + 1e-14)

for te in SPK:
    tr = [s for s in SPK if s != te]
    X = np.concatenate([det.design(D[s][0], det.adapt(D[s][0])) for s in tr]); c = np.concatenate([D[s][1] for s in tr]); nn = np.concatenate([D[s][2] for s in tr])
    use = ~nn & (c != ""); m = det.fit(X[use], np.isin(c[use], list(SIBILANT)).astype(float))
    utts = list(utterances(te))[::8]                                  # ~140 utterances per voice
    rec = []
    for name, w, l in utts:
        fs, x, egg, ph = load(te, w, l)
        t, F = features(x, fs)
        p = det.prob(det.design(F, det.adapt(F)), m)
        ya, _ = dp.axell(x, fs, p, AMOUNT)
        rec.append((fs, x, ph, ya))
    # labels per STFT frame of the de-esser analysis
    def frame_cls(ph, n, fs):
        tt = np.arange(n) * dp.HOP_S; cl = np.full(n, "", object)
        for a, b, q in ph: cl[(tt >= a) & (tt < b)] = q
        return cl
    def collect(outs):
        S, O = [], []
        for (fs, x, ph, _), y in zip(rec, outs):
            h0, h1 = hf_db(x, fs), hf_db(y, fs); n = min(len(h0), len(h1)); cl = frame_cls(ph, n, fs)
            lvl = h0[:n] > np.percentile(h0[:n], 50)                         # frames with real HF content
            S.append((h1 - h0)[:n][np.isin(cl, list(SIBILANT)) & lvl]); O.append(((h1 - h0)[:n], cl, lvl))
        return np.concatenate(S), O
    sa, Oa = collect([r[3] for r in rec])
    target = sa.mean()
    # classic: find the threshold giving the same mean sibilant reduction (bisection on threshold)
    lo, hi = -80.0, 0.0
    for it in range(14):
        th = 0.5 * (lo + hi)
        outs = [dp.classic(x, fs, th)[0] for fs, x, ph, _ in rec]
        sc, Oc = collect(outs)
        if sc.mean() < target: lo = th
        else: hi = th
    def collateral(O, group):
        v = []
        for d, cl, lvl in O:
            if group == "vowels/voiced":
                msk = ~np.isin(cl, list(SIBILANT | STOPS | FRICATIVE_OTHER | {"pau", ""}))
            else:
                msk = np.isin(cl, list(group))
            v.append(d[msk & lvl])
        v = np.concatenate(v); return np.mean(np.abs(v)), np.percentile(np.abs(v), 99)
    print(f"== held-out voice {te}: both de-essers take the sibilants' 2-16 kHz band down by {-target:.1f} dB on average")
    for name, s_, O in (("Axell  ", sa, Oa), ("classic", sc, Oc)):
        cv = collateral(O, "vowels/voiced"); cs = collateral(O, STOPS); cf = collateral(O, FRICATIVE_OTHER)
        print(f"  {name}: sibilants {s_.mean():+.1f} dB (spread sd {s_.std():.1f}, deepest 1% {np.percentile(s_, 1):+.1f}) | collateral |dB| mean / 99th: "
              f"vowels+voiced {cv[0]:.2f} / {cv[1]:.1f}, stops {cs[0]:.2f} / {cs[1]:.1f}, f/th/v {cf[0]:.2f} / {cf[1]:.1f}")
