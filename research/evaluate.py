import sys, numpy as np
sys.path.insert(0, ".")
from data import SIBILANT, STOPS, FRICATIVE_OTHER
from features import NAMES
from scipy.ndimage import uniform_filter1d, maximum_filter1d
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))

def load(spk):
    d = np.load(f"{DATA}/feat_{spk}.npz", allow_pickle=True)
    return d["F"], d["cls"], d["near"]

def scores(pred, cls, near):
    sib = np.isin(cls, list(SIBILANT))
    keep = ~near & (cls != "")
    tp = (pred & sib & keep).sum(); fp = (pred & ~sib & keep).sum(); fn = (~pred & sib & keep).sum()
    P = tp / max(tp + fp, 1); R = tp / max(tp + fn, 1)
    fa = {}
    for name, group in [("stops", STOPS), ("f/th/v/dh/hh", FRICATIVE_OTHER), ("vowels+voiced", None), ("pause/breath", {"pau"})]:
        if group is None:
            m = keep & ~sib & ~np.isin(cls, list(STOPS | FRICATIVE_OTHER | {"pau"}))
        else:
            m = keep & np.isin(cls, list(group))
        fa[name] = pred[m].mean()
    return P, R, 2 * P * R / max(P + R, 1e-9), fa

def best_threshold(v, cls, near):
    best = (0, None)
    for th in np.percentile(v, np.linspace(50, 99.5, 200)):
        f1 = scores(v > th, cls, near)[2]
        if f1 > best[0]:
            best = (f1, th)
    return best[1]

def smooth(F):
    # +-6 ms context (the plugin has the look-ahead for it): mean of the ratio features, so 2 ms transients can't fire alone
    G = F.copy()
    for k in (0, 1, 5):
        G[:, k] = uniform_filter1d(F[:, k], 7)
    return G

Ftr, ctr, ntr = load("slt"); Fte, cte, nte = load("bdl")
def report(name, ptr, pte):
    for spk, pred, cls, near in (("slt (fit)", ptr, ctr, ntr), ("bdl (test)", pte, cte, nte)):
        P, R, F1, fa = scores(pred, cls, near)
        print(f"{name:42s} {spk:10s} precision {P:.3f} recall {R:.3f} F1 {F1:.3f} | false alarms: " + ", ".join(f"{k} {100*v:.1f}%" for k, v in fa.items()))

# 1. classic de-esser: high band level over a threshold (here even normalised per utterance - generous)
th = best_threshold(Ftr[:, 6], ctr, ntr); report("classic: HF level > threshold", Ftr[:, 6] > th, Fte[:, 6] > th)
# 2. split-band ratio (sibilance band vs voice body), like auto level tracking
th = best_threshold(Ftr[:, 0], ctr, ntr); report("relative: HF/body ratio > threshold", Ftr[:, 0] > th, Fte[:, 0] > th)
# 3. Axell: logistic model on all features with context, fitted on slt only
from numpy.linalg import lstsq
def design(F):
    G = smooth(F)
    return np.column_stack([G, G[:, 0] * G[:, 1], np.ones(len(G))])
sib = np.isin(ctr, list(SIBILANT)) & ~ntr & (ctr != "")
use = ~ntr & (ctr != "")
X, y = design(Ftr)[use], sib[use].astype(float)
mu, sd = X[:, :-1].mean(0), X[:, :-1].std(0) + 1e-9
def norm(X): Z = X.copy(); Z[:, :-1] = (Z[:, :-1] - mu) / sd; return Z
Z = norm(X); w = np.zeros(Z.shape[1])
for it in range(300):                      # Newton / IRLS logistic regression
    p = 1 / (1 + np.exp(-Z @ w)); W = p * (1 - p) + 1e-6
    w += np.linalg.solve(Z.T @ (Z * W[:, None]) + 1e-3 * np.eye(len(w)), Z.T @ (y - p))
ptr = 1 / (1 + np.exp(-norm(design(Ftr)) @ w)); pte = 1 / (1 + np.exp(-norm(design(Fte)) @ w))
th = best_threshold(ptr, ctr, ntr)
report("Axell features + context (logistic)", ptr > th, pte > th)
np.savez(f"{DATA}/model_v1.npz", w=w, mu=mu, sd=sd, th=th)
print("weights", dict(zip(NAMES + ["sib*flat"], np.round(w[:-1], 2))), "bias", round(w[-1], 2), "threshold", round(th, 3))
