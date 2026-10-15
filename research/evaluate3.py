"""Leave-one-speaker-out evaluation of sibilance detectors (fit on two voices, test on the third)."""
import sys, numpy as np
sys.path.insert(0, ".")
from data import SIBILANT, STOPS, FRICATIVE_OTHER
from features2 import NAMES
from scipy.ndimage import uniform_filter1d
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))
SPK = ("slt", "bdl", "jmk")
D = {}
for s in SPK:
    d = np.load(f"{DATA}/feat2_{s}.npz", allow_pickle=True)
    D[s] = (d["F"], d["cls"], d["near"], d["utt"])

def adapt(F, utt, tau=1500):
    """causal speaker adaptation: band features minus their running mean over voiced, loud frames (tau 3 s);
    runs continuously across a speaker's utterances like a plugin across a song"""
    out = F.copy(); base = None; a = 1.0 / tau
    cols = slice(3, F.shape[1])
    for i in range(len(F)):
        if F[i, 0] < 0.2 and F[i, 2] > -25:
            base = F[i, cols].copy() if base is None else base + a * (F[i, cols] - base)
        if base is not None:
            out[i, cols] = F[i, cols] - base
    return out

def design(F, A):
    G = F.copy(); H = A.copy()
    for k in range(G.shape[1]):
        G[:, k] = uniform_filter1d(G[:, k], 7); H[:, k] = uniform_filter1d(H[:, k], 7)
    bands = H[:, 3:-1]
    peak = bands.max(1)                                        # strongest band's excess over the voice, wherever it is
    low_hi = np.maximum(bands[:, 1], bands[:, 2]) - bands[:, 0] # sh/ch region rising above 1.5-2.5 k (voiced formants)
    return np.column_stack([G[:, :3], H[:, 3:], peak, low_hi, G[:, 0] * peak, G[:, 1] * peak, np.ones(len(G))])

def fit(X, y, l2=1e-2):
    mu, sd = X[:, :-1].mean(0), X[:, :-1].std(0) + 1e-9
    Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; w = np.zeros(Z.shape[1])
    for it in range(60):
        p = 1 / (1 + np.exp(-Z @ w)); W = p * (1 - p) + 1e-6
        w += np.linalg.solve(Z.T @ (Z * W[:, None]) + l2 * np.eye(len(w)), Z.T @ (y - p))
    return w, mu, sd
def prob(X, m):
    w, mu, sd = m; Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; return 1 / (1 + np.exp(-Z @ w))

def scores(pred, cls, near):
    sib = np.isin(cls, list(SIBILANT)); keep = ~near & (cls != "")
    tp = (pred & sib & keep).sum(); fp = (pred & ~sib & keep).sum(); fn = (~pred & sib & keep).sum()
    P = tp / max(tp + fp, 1); R = tp / max(tp + fn, 1)
    fa = {}
    for name, g in [("stops", STOPS), ("f/th/v/dh/hh", FRICATIVE_OTHER), ("pause", {"pau"})]:
        fa[name] = pred[keep & np.isin(cls, list(g))].mean()
    fa["vowels/voiced"] = pred[keep & ~sib & ~np.isin(cls, list(STOPS | FRICATIVE_OTHER | {"pau"}))].mean()
    rc = {p: pred[keep & (cls == p)].mean() for p in ["s", "sh", "ch", "z", "jh"]}
    return P, R, 2 * P * R / max(P + R, 1e-9), fa, rc

def energy_recall(pred, cls, near, F):
    # share of the sibilant high-band energy (1.5-14 kHz re body, x level) that falls in detected frames: what a de-esser must catch
    sib = np.isin(cls, list(SIBILANT)) & ~near
    e = 10 ** ((F[:, 2] + F[:, -1]) / 10)
    return (e * (pred & sib)).sum() / (e * sib).sum()

def best_th(p, cls, near):
    best = (0, 0.5)
    for th in np.linspace(0.05, 0.95, 91):
        f1 = scores(p > th, cls, near)[2]
        if f1 > best[0]: best = (f1, th)
    return best[1]

A = {s: adapt(D[s][0], D[s][3]) for s in SPK}
X = {s: design(D[s][0], A[s]) for s in SPK}
res = []
for te in SPK:
    tr = [s for s in SPK if s != te]
    Xt = np.concatenate([X[s] for s in tr]); ct = np.concatenate([D[s][1] for s in tr]); nt = np.concatenate([D[s][2] for s in tr])
    use = ~nt & (ct != "")
    m = fit(Xt[use], np.isin(ct[use], list(SIBILANT)).astype(float))
    th = best_th(prob(Xt, m), ct, nt)
    pr = prob(X[te], m) > th
    P, R, F1, fa, rc = scores(pr, D[te][1], D[te][2])
    res.append(F1)
    print(f"TEST {te}: P {P:.3f} R {R:.3f} F1 {F1:.3f} energy-recall {energy_recall(pr, D[te][1], D[te][2], D[te][0]):.3f} | recall " + " ".join(f"{k} {v:.2f}" for k, v in rc.items()) +
          " | false alarms " + ", ".join(f"{k} {100*v:.1f}%" for k, v in fa.items()) + f" | th {th:.2f}")
print("mean held-out F1 %.3f" % np.mean(res))
# final model on all three voices (for the plugin)
Xa = np.concatenate([X[s] for s in SPK]); ca = np.concatenate([D[s][1] for s in SPK]); na = np.concatenate([D[s][2] for s in SPK])
use = ~na & (ca != ""); m = fit(Xa[use], np.isin(ca[use], list(SIBILANT)).astype(float)); th = best_th(prob(Xa, m), ca, na)
np.savez(f"{DATA}/model_v3.npz", w=m[0], mu=m[1], sd=m[2], th=th)
print("final weights:", dict(zip(NAMES + ["peak band", "sh rise", "ap*peak", "flat*peak"], np.round(m[0][:-1], 2))), "bias", round(m[0][-1], 2), "th", th)
# reference: the classic detector (HF level re loud, threshold fitted on the other two voices) - energy recall + FA
for te in SPK:
    tr = [s for s in SPK if s != te]
    hf = {s: D[s][0][:, 2] + D[s][0][:, -1] for s in SPK}
    v = np.concatenate([hf[s] for s in tr]); ct = np.concatenate([D[s][1] for s in tr]); nt = np.concatenate([D[s][2] for s in tr])
    best = (0, 0)
    for t in np.percentile(v, np.linspace(60, 99.5, 120)):
        f1 = scores(v > t, ct, nt)[2]
        if f1 > best[0]: best = (f1, t)
    pr = hf[te] > best[1]; P, R, F1, fa, rc = scores(pr, D[te][1], D[te][2])
    print(f"classic TEST {te}: F1 {F1:.3f} energy-recall {energy_recall(pr, D[te][1], D[te][2], D[te][0]):.3f} | false alarms " + ", ".join(f"{k} {100*v:.1f}%" for k, v in fa.items()))
