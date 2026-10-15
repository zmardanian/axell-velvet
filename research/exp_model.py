"""Model experiments on the C++ features: logistic vs small MLP; soft (p^2-weighted) energy metrics = what the
de-esser does with the probability."""
import sys, numpy as np
sys.path.insert(0, ".")
import train_cpp as T
from data import SIBILANT, STOPS, FRICATIVE_OTHER
import os
cache = os.path.expanduser("~/axell-velvet-data/cppfeat/all.npz")
if not os.path.exists(cache):
    meta = np.load(f"{T.OUT}/meta.npy", allow_pickle=True); D, C, N, S, A, U = [], [], [], [], [], []
    for i, (spk, name, aug, fs, ph) in enumerate(meta):
        d = np.fromfile(f"{T.OUT}/{spk}_{name}.feat", np.float32).reshape(-1, T.NC); cls, near = T.label(d[:, 0].astype(float), ph)
        D.append(d); C.append(cls); N.append(near); S.append(np.full(len(d), spk)); A.append(np.full(len(d), aug)); U.append(np.full(len(d), i))
    np.savez(cache, D=np.concatenate(D), C=np.concatenate(C).astype(str), N=np.concatenate(N), S=np.concatenate(S), A=np.concatenate(A), U=np.concatenate(U))
z = np.load(cache); D, C, N, S, A, U = z["D"].astype(np.float64), z["C"], z["N"], z["S"], z["A"], z["U"]
X = D[:, T.COL["x"]]; y = np.isin(C, list(SIBILANT)).astype(float); use = ~N & (C != "")
e = D[:, T.COL["E"]].sum(1)
def soft(p, sel):
    w = p ** 2; out = {}
    for name, m in [("sib", np.isin(C, list(SIBILANT))), ("vowel", ~np.isin(C, list(SIBILANT | STOPS | FRICATIVE_OTHER | {"pau", ""}))),
                    ("stops", np.isin(C, list(STOPS))), ("f/th/hh", np.isin(C, list(FRICATIVE_OTHER)))]:
        mm = sel & m & ~N
        out[name] = (e[mm] * w[mm]).sum() / max(e[mm].sum(), 1e-30)
    return out
def mlp_fit(X, y, H=24, epochs=12, seed=0, l2=1e-4):
    rng = np.random.default_rng(seed); mu, sd = X.mean(0), X.std(0) + 1e-9; Z = (X - mu) / sd
    W1 = rng.standard_normal((Z.shape[1], H)) / np.sqrt(Z.shape[1]); b1 = np.zeros(H); W2 = rng.standard_normal(H) / np.sqrt(H); b2 = 0.0
    P = [W1, b1, W2, np.array([b2])]; M = [np.zeros_like(p) for p in P]; V = [np.zeros_like(p) for p in P]; t = 0; lr = 3e-3
    for ep in range(epochs):
        idx = rng.permutation(len(Z))
        for s in range(0, len(Z), 4096):
            b = idx[s:s + 4096]; z = Z[b]; h = np.tanh(z @ P[0] + P[1]); o = h @ P[2] + P[3][0]; p = 1 / (1 + np.exp(-np.clip(o, -30, 30)))
            g = (p - y[b]) / len(b); gW2 = h.T @ g; gb2 = np.array([g.sum()]); gh = np.outer(g, P[2]) * (1 - h * h)
            G = [z.T @ gh + l2 * P[0], gh.sum(0), gW2 + l2 * P[2], gb2]; t += 1
            for i in range(4):
                M[i] = 0.9 * M[i] + 0.1 * G[i]; V[i] = 0.999 * V[i] + 0.001 * G[i] ** 2
                P[i] -= lr * (M[i] / (1 - 0.9 ** t)) / (np.sqrt(V[i] / (1 - 0.999 ** t)) + 1e-8)
        lr *= 0.75
    return P, mu, sd
def mlp_prob(X, m):
    P, mu, sd = m; h = np.tanh(((X - mu) / sd) @ P[0] + P[1]); return 1 / (1 + np.exp(-np.clip(h @ P[2] + P[3][0], -30, 30)))
for kind in ("logistic", "mlp"):
    print(kind)
    for te in T.SPK:
        tr = (S != te) & use
        if kind == "logistic":
            m = T.fit(X[tr], y[tr]); pf = lambda Q: T.prob(Q, m)
        else:
            sub = np.where(tr)[0][::3]; m = mlp_fit(X[sub], y[sub]); pf = lambda Q: mlp_prob(Q, m)
        sel = S == te; p = np.zeros(len(X)); p[sel] = pf(X[sel])
        P_, R_, F1, fa = T.scores(p[sel] > 0.5, C[sel], N[sel])
        sm = soft(p, sel)
        print(f"  {te}: F1 {F1:.3f} | soft energy: sibilant {sm['sib']:.3f}, vowel {sm['vowel']:.4f}, stops {sm['stops']:.3f}, f/th/hh {sm['f/th/hh']:.3f}")
