"""Velvet 2.0 detector candidates vs 1.0 (the shipping model's p, column 'p' of the feature files):
- held-out CMU ARCTIC voices (leave-one-speaker-out): energy-weighted share caught per phone, false alarms;
- the held-out Romanian takes (role "test" in the takes manifest; acoustic labels from romanian_data.py): energy-weighted
  share of unvoiced and voiced sibilant frames caught, and false alarms on vowel frames.
usage: python3 eval_v2.py [romanian weight ...]"""
import os, sys, numpy as np
alphas = [float(a) for a in sys.argv[1:]] or [0.0, 0.2, 0.5]
sys.argv = ["x"]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_cpp as T, train_v2 as V
from data import SIBILANT
from romanian_data import TAKES, OUT as ROUT, stem

z = np.load(os.path.expanduser("~/axell-velvet-data/v2cache.npz"), allow_pickle=True)
X = z["X"].astype(np.float64); y = z["y"]; C = z["C"]; N = z["N"]; S = z["S"]; E = z["E"].astype(np.float64)
use = ~N & (C != "")
R = np.load(os.path.expanduser("~/axell-velvet-data/romanian.npz"), allow_pickle=True)
ro = {k: {f: R[f"{k}|{f}"] for f in ("X", "E", "lab", "kind", "role")} for k in TAKES}
for k in ro:   # 1.0's own probability on the Romanian frames, from the shipping C++ model
    d = np.fromfile(f"{ROUT}/{stem(k)}.feat", np.float32).reshape(-1, T.NC)
    ro[k]["p10"] = d[:, T.COL["p"]].astype(np.float64)

def arctic_w(c, e):
    w = np.ones(len(c)); sib = np.isin(c, list(SIBILANT))
    for q in SIBILANT:
        m = c == q
        if m.any():
            w[m] = np.clip(e[m] / np.median(e[m]), 0.05, 5)
            w[m] *= sib.sum() / len(SIBILANT) / w[m].sum()
    return w

def ro_report(pfun):
    out = {}
    for k, r in ro.items():
        if r["role"] != "test":
            continue
        p = pfun(r); lab, kind, e = r["lab"], r["kind"], r["E"]
        for nm, m in (("unvoiced", (lab == 1) & (kind == "u")), ("voiced", (lab == 1) & (kind == "v"))):
            out.setdefault(nm, []).append((e[m] * (p[m] > 0.5)).sum() / max(e[m].sum(), 1e-30))
        out.setdefault("vowel FA", []).append(np.mean(p[lab == 0] > 0.5))
    return out

def arctic_report(fit):
    acc = {}
    for te in T.SPK:
        m = fit(te); sel = (S == te) & use; p = T.mlp_prob(X[sel], m); c = C[sel]; e = E[sel]
        for q in ("s", "z", "sh", "zh", "ch", "jh", "f", "th", "t"):
            mq = c == q
            acc.setdefault(q, []).append((e[mq] * (p[mq] > 0.5)).sum() / e[mq].sum())
        _, _, F1, fa = T.scores(p > 0.5, c, N[sel]); acc.setdefault("vowel FA", []).append(fa["vowels/voiced"])
    return acc

def show(name, a, r):
    print(f"--- {name}")
    if a:
        print("   ARCTIC held out: " + "  ".join(f"{q} {100 * np.mean(v):.1f}%" for q, v in a.items()))
    print("   Romanian takes held out: " + "  ".join(f"{q} " + "/".join(f"{100 * x:.1f}" for x in v) + "%" for q, v in r.items()))

trX = np.concatenate([ro[k]["X"][ro[k]["lab"] >= 0] for k in ro if ro[k]["role"] == "train"])
trY = np.concatenate([(ro[k]["lab"][ro[k]["lab"] >= 0] == 1).astype(float) for k in ro if ro[k]["role"] == "train"])
trE = np.concatenate([ro[k]["E"][ro[k]["lab"] >= 0] for k in ro if ro[k]["role"] == "train"])
if __name__ == "__main__":
    show("1.0 shipping model", None, ro_report(lambda r: r["p10"]))
    models = {}
    for a in alphas:
        def fit(te=None, a=a):
            tr = np.where(((S != te) if te else np.ones(len(S), bool)) & use)[0][::3]
            w = arctic_w(C[tr], E[tr]); Xs, ys, ws = [X[tr]], [y[tr]], [w]
            if a > 0:   # Romanian frames: their positives get a share a of the positive weight, negatives likewise
                wr = np.where(trY == 1, np.clip(trE / np.median(trE[trY == 1]), 0.05, 5), 1.0)
                pos, neg = trY == 1, trY == 0
                wr[pos] *= a * w[y[tr] == 1].sum() / wr[pos].sum(); wr[neg] *= a * w[y[tr] == 0].sum() / wr[neg].sum()
                Xs.append(trX); ys.append(trY); ws.append(wr)
            return V.mlp_fit_w(np.concatenate(Xs), np.concatenate(ys), np.concatenate(ws), H=32)
        full = fit(None); models[a] = full
        show(f"2.0 candidate, Romanian weight {a}", arctic_report(fit), ro_report(lambda r, m=full: T.mlp_prob(r["X"], m)))
    np.save(os.path.expanduser("~/axell-velvet-data/v2models.npy"), np.array([models], dtype=object), allow_pickle=True)
