"""Held-out detector folds for the 2.0 candidates (ARCTIC voice left out; the Romanian training takes included) ->
~/axell-velvet-data/models_v2/a<weight>/no_<spk>.h, for bench_cpp.py. usage: python3 make_folds_v2.py 0.2 0.5"""
import os, sys, numpy as np
alphas = [float(a) for a in sys.argv[1:]]
sys.argv = ["x", "0"]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_v2 as E, train_cpp as T, train_v2 as V
for a in alphas:
    d = os.path.expanduser(f"~/axell-velvet-data/models_v2/a{a}"); os.makedirs(d, exist_ok=True)
    for te in T.SPK:
        tr = np.where((E.S != te) & E.use)[0][::3]
        w = E.arctic_w(E.C[tr], E.E[tr]); Xs, ys, ws = [E.X[tr]], [E.y[tr]], [w]
        wr = np.where(E.trY == 1, np.clip(E.trE / np.median(E.trE[E.trY == 1]), 0.05, 5), 1.0)
        pos, neg = E.trY == 1, E.trY == 0
        wr[pos] *= a * w[E.y[tr] == 1].sum() / wr[pos].sum(); wr[neg] *= a * w[E.y[tr] == 0].sum() / wr[neg].sum()
        m = V.mlp_fit_w(np.concatenate(Xs + [E.trX]), np.concatenate(ys + [E.trY]), np.concatenate(ws + [wr]), H=32)
        T.write_header(f"{d}/no_{te}.h", m, len(tr), f"2.0 held-out fold without {te}, Romanian weight {a}")
        print("wrote", f"{d}/no_{te}.h", flush=True)
