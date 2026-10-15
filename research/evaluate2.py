import sys, numpy as np
sys.path.insert(0, ".")
from data import SIBILANT, STOPS, FRICATIVE_OTHER
from evaluate import load, scores, best_threshold, smooth
from scipy.ndimage import uniform_filter1d
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))

def adapt(F, tau_frames=1500):
    """speaker adaptation, causal: features relative to a running mean over voiced, loud frames (tau 3 s)"""
    G = F.copy()
    base = np.zeros(F.shape[1]); init = False
    a = 1.0 / tau_frames
    out = np.empty_like(F)
    for i in range(len(F)):
        if F[i, 3] > 0.6 and F[i, 4] > -20:          # voiced and loud: the voice body
            if not init: base = F[i].copy(); init = True
            else: base += a * (F[i] - base)
        out[i] = F[i] - base
    return out

def per_class(pred, cls, near):
    keep = ~near
    return {p: pred[keep & (cls == p)].mean() for p in ["s", "sh", "ch", "z", "jh"]}

def design(F, A):
    G = smooth(F); H = smooth(A)
    return np.column_stack([G[:, [1, 3]], H[:, [0, 2, 5, 6]], G[:, 0] * G[:, 1], np.ones(len(G))])

def fit(X, y):
    mu, sd = X[:, :-1].mean(0), X[:, :-1].std(0) + 1e-9
    Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd
    w = np.zeros(Z.shape[1])
    for it in range(200):
        p = 1 / (1 + np.exp(-Z @ w)); W = p * (1 - p) + 1e-6
        w += np.linalg.solve(Z.T @ (Z * W[:, None]) + 1e-3 * np.eye(len(w)), Z.T @ (y - p))
    return w, mu, sd
def prob(X, m):
    w, mu, sd = m; Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; return 1 / (1 + np.exp(-Z @ w))

D = {s: load(s) for s in ("slt", "bdl")}
A = {s: adapt(D[s][0]) for s in D}
for tr, te in (("slt", "bdl"), ("bdl", "slt")):
    F, c, n = D[tr]; X = design(F, A[tr]); use = ~n & (c != "")
    m = fit(X[use], np.isin(c[use], list(SIBILANT)).astype(float))
    ptr = prob(X, m); th = best_threshold(ptr, c, n)
    F2, c2, n2 = D[te]; pte = prob(design(F2, A[te]), m)
    for spk, p, cc, nn in ((tr + " fit", ptr, c, n), (te + " TEST", pte, c2, n2)):
        P, R, F1, fa = scores(p > th, cc, nn)
        pc = per_class(p > th, cc, nn)
        print(f"Axell v2 (adaptive) {spk:9s} P {P:.3f} R {R:.3f} F1 {F1:.3f} | recall " + " ".join(f"{k} {v:.2f}" for k, v in pc.items()) +
              " | FA " + ", ".join(f"{k} {100*v:.1f}%" for k, v in fa.items()))
    np.savez(f"{DATA}/model_v2_{tr}.npz", w=m[0], mu=m[1], sd=m[2], th=th)
# classic per-class for reference (fit slt)
F, c, n = D["slt"]; th = best_threshold(F[:, 6], c, n); F2, c2, n2 = D["bdl"]
print("classic on bdl recall", {k: round(v, 2) for k, v in per_class(F2[:, 6] > th, c2, n2).items()})
