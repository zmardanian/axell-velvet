import numpy as np, sys
sys.path.insert(0, ".")
exec(open("exp_model.py").read().split("for kind in")[0])
from scipy.ndimage import uniform_filter1d
def past_mean(X, U, k):
    out = np.empty_like(X)
    starts = np.r_[0, np.where(np.diff(U) != 0)[0] + 1, len(U)]
    for a, b in zip(starts[:-1], starts[1:]):
        c = np.cumsum(np.vstack([np.zeros((1, X.shape[1])), X[a:b]]), 0)
        idx = np.arange(b - a); lo = np.maximum(0, idx - k + 1)
        out[a:b] = (c[idx + 1] - c[lo]) / (idx + 1 - lo)[:, None]
    return out
for k in (8, 16):
    Xc = np.column_stack([X, past_mean(X[:, [0, 1, 9, 10, 11]], U, k)])
    print("context past", k, "frames")
    for te in T.SPK:
        tr = (S != te) & use; sub = np.where(tr)[0][::3]; m = mlp_fit(Xc[sub], y[sub])
        sel = S == te; p = np.zeros(len(X)); p[sel] = mlp_prob(Xc[sel], m)
        F1 = T.scores(p[sel] > 0.5, C[sel], N[sel])[2]; sm = soft(p, sel)
        print(f"  {te}: F1 {F1:.3f} | soft energy: sibilant {sm['sib']:.3f}, vowel {sm['vowel']:.4f}, stops {sm['stops']:.3f}, f/th/hh {sm['f/th/hh']:.3f}")
