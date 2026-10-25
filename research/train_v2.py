"""Velvet 2.0 detector: the same features and network as 1.0 (train_cpp.py), trained so that every sibilant class
counts - 1.0's training was dominated by "s" (4392 labelled frames in a test sample vs 137 "zh", 361 "jh", 694 "ch")
and detected only 28-49 % of ch / zh / jh frames, the sounds of Romanian ce/ci, ge/gi, j and (like t+s) ț.

Class balance: each sibilant phone (s z sh zh ch jh) gets the same total weight in the loss; the non-sibilant side
keeps its total weight, split so the confusable fricatives and stops (f th v dh hh, t d k p b g) are not drowned by
vowels. Evaluation: leave-one-speaker-out, per phone (share of frames with p > 0.5) and the 1.0 false-alarm groups,
clean and through random recording chains. usage: python3 train_v2.py [--hidden 32] [--write]"""
import os, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_cpp as T
from data import SIBILANT, STOPS, FRICATIVE_OTHER

arg = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
H = arg("--hidden", 32)
PHONES = ("s", "z", "sh", "zh", "ch", "jh", "f", "th", "v", "dh", "t", "d", "k", "p")

def mlp_fit_w(X, y, w, H=32, epochs=12, seed=0, l2=1e-4):
    """as train_cpp.mlp_fit, with per-frame loss weights (normalised per minibatch)"""
    rng = np.random.default_rng(seed); mu, sd = X.mean(0), X.std(0) + 1e-9; Z = (X - mu) / sd
    P = [rng.standard_normal((Z.shape[1], H)) / np.sqrt(Z.shape[1]), np.zeros(H), rng.standard_normal(H) / np.sqrt(H), np.zeros(1)]
    M = [np.zeros_like(q) for q in P]; V = [np.zeros_like(q) for q in P]; t = 0; lr = 3e-3
    for ep in range(epochs):
        idx = rng.permutation(len(Z))
        for s0 in range(0, len(Z), 4096):
            b = idx[s0:s0 + 4096]; z = Z[b]; h = np.tanh(z @ P[0] + P[1]); o = h @ P[2] + P[3][0]
            p = 1 / (1 + np.exp(-np.clip(o, -30, 30))); g = w[b] * (p - y[b]) / w[b].sum(); gh = np.outer(g, P[2]) * (1 - h * h)
            G = [z.T @ gh + l2 * P[0], gh.sum(0), h.T @ g + l2 * P[2], np.array([g.sum()])]; t += 1
            for i in range(4):
                M[i] = 0.9 * M[i] + 0.1 * G[i]; V[i] = 0.999 * V[i] + 0.001 * G[i] ** 2
                P[i] -= lr * (M[i] / (1 - 0.9 ** t)) / (np.sqrt(V[i] / (1 - 0.999 ** t)) + 1e-8)
        lr *= 0.75
    return P, mu, sd

def weights(C, balance):
    w = np.ones(len(C))
    if not balance:
        return w
    sib = np.isin(C, list(SIBILANT))
    for q in SIBILANT:                                    # each sibilant class: an equal share of the positive weight
        m = C == q
        if m.any():
            w[m] = sib.sum() / len(SIBILANT) / m.sum()
    conf = np.isin(C, list(FRICATIVE_OTHER | STOPS))      # confusable negatives: half of the negative weight
    rest = ~sib & ~conf
    neg = (~sib).sum()
    if conf.any():
        w[conf] = 0.5 * neg / conf.sum()
    if rest.any():
        w[rest] = 0.5 * neg / rest.sum()
    return w

def per_phone(p, C, N):
    keep = ~N & (C != "")
    return {q: float((p[keep & (C == q)] > 0.5).mean()) if (keep & (C == q)).any() else float("nan") for q in PHONES}

if __name__ == "__main__":
    meta = np.load(f"{T.OUT}/meta.npy", allow_pickle=True)
    D, C, N, S, A = [], [], [], [], []
    for spk, name, aug, fs, ph in meta:
        d = np.fromfile(f"{T.OUT}/{spk}_{name}.feat", np.float32).reshape(-1, T.NC).astype(np.float64)
        cls, near = T.label(d[:, 0], ph)
        D.append(d); C.append(cls); N.append(near); S.append(np.full(len(d), spk)); A.append(np.full(len(d), aug))
    D = np.concatenate(D); C = np.concatenate(C); N = np.concatenate(N); S = np.concatenate(S); A = np.concatenate(A)
    X = D[:, T.COL["x"]]; y = np.isin(C, list(SIBILANT)).astype(float); use = ~N & (C != "")
    print(f"{len(D)} frames; per class:", {q: int(np.sum(use & (C == q))) for q in sorted(SIBILANT)})
    for balance in (False, True):
        rows = {}
        for te in T.SPK:
            tr = np.where((S != te) & use)[0][::3]
            m = mlp_fit_w(X[tr], y[tr], weights(C[tr], balance), H=H)
            for aug in (False, True):
                sel = (S == te) & (A == aug)
                p = T.mlp_prob(X[sel], m)
                pp = per_phone(p, C[sel], N[sel]); _, _, F1, fa = T.scores(p > 0.5, C[sel], N[sel])
                for k, v in pp.items():
                    rows.setdefault(k, []).append(v)
                for k, v in fa.items():
                    rows.setdefault("FA " + k, []).append(v)
                rows.setdefault("F1", []).append(F1)
        name = "2.0 class-balanced" if balance else "1.0 recipe"
        print(f"=== {name} (H {H}), held-out voices, clean + chain averaged: share of frames with p > 0.5")
        print("   sibilants: " + "  ".join(f"{q} {100 * np.nanmean(rows[q]):5.1f}%" for q in ("s", "z", "sh", "zh", "ch", "jh")))
        print("   others:    " + "  ".join(f"{q} {100 * np.nanmean(rows[q]):4.1f}%" for q in ("f", "th", "v", "dh", "t", "d", "k", "p")))
        print("   false alarms: " + "  ".join(f"{k[3:]} {100 * np.mean(v):.2f}%" for k, v in rows.items() if k.startswith("FA ")) + f" | F1 {np.mean(rows['F1']):.3f}")
    if "--write" in sys.argv:
        tr = np.where(use)[0][::2]
        m = mlp_fit_w(X[tr], y[tr], weights(C[tr], True), H=H, epochs=14)
        hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source", "DetectorModel.h")
        T.write_header(hdr, m, len(tr), "all three voices, 2.0: every sibilant class weighted equally (train_v2.py)")
        print("wrote", os.path.abspath(hdr))
