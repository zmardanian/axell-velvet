"""Velvet 3.0 vs 1.0 detector on the artist's own Romanian takes (romanian_data.py: acoustic labels where the sound
leaves no doubt; the shipping C++ features). Energy-weighted share of unvoiced / voiced sibilant frames with p > 0.5 and
the share of clearly vowel frames with p > 0.5, per take (train and test takes alike: no 3.0 model saw any of them).
usage: python3 eval3_romanian.py <studio header> [more headers...]"""
import os, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ARGS = sys.argv[1:]
from eval3_v1 import header
import train_cpp as T

R = np.load(os.path.expanduser("~/axell-velvet-data/romanian.npz"), allow_pickle=True)
takes = sorted({k.split("|")[0] for k in R.files})
heads = [os.path.expanduser("~/axell-velvet-data/models/v1_full.h")] + ARGS
for h in heads:
    m = header(h); rows = []
    for k in takes:
        X = R[f"{k}|X"].astype(np.float64); E = R[f"{k}|E"].astype(np.float64); lab = R[f"{k}|lab"]; kind = R[f"{k}|kind"]
        p = T.mlp_prob(X, m) > 0.5
        u = (lab == 1) & (kind == "u"); v = (lab == 1) & (kind == "v")
        rows.append(((E[u] * p[u]).sum() / max(E[u].sum(), 1e-30), (E[v] * p[v]).sum() / max(E[v].sum(), 1e-30), p[lab == 0].mean(), u.sum(), v.sum()))
    a = np.array(rows)
    print(f"{os.path.basename(h):22s} unvoiced sibilant energy caught {100 * np.mean(a[:, 0]):5.1f}%  voiced {100 * np.mean(a[:, 1]):5.1f}%  "
          f"vowel frames flagged {100 * np.mean(a[:, 2]):.3f}% (mean over {len(takes)} takes; worst take {100 * np.max(a[:, 2]):.3f}%)")
