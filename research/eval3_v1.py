"""Velvet 1.0's detector on the 3.0 held-out sets, for comparison with train3.py's numbers: TEST_VCTK with the shipping
1.0 network (it never saw VCTK) and each ARCTIC voice with 1.0's own held-out network (models/no_<voice>.h).
usage: python3 eval3_v1.py"""
import os, re, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.argv = [sys.argv[0], "train", "studio"]
import train3 as T3, train_cpp as T, corpus3 as C3

def header(p):
    s = open(p).read(); g = lambda n: np.array([float(v.strip().rstrip("f")) for v in re.search(n + r"\[[^\]]*\] = \{([^}]*)\}", s).group(1).split(",")])
    H = int(re.search(r"kHidden = (\d+)", s).group(1)); I = int(re.search(r"kInputs = (\d+)", s).group(1))
    b2 = float(re.search(r"kB2 = ([^f;]+)f", s).group(1))
    return [g("kW1").reshape(I, H), g("kB1"), g("kW2"), np.array([b2])], g("kMu"), g("kSd")

if __name__ == "__main__":
    X, E, Cl, N, V, A = T3.load("studio", 0.0)
    test = np.isin(V, C3.TEST_VCTK)
    m = header(os.path.expanduser("~/axell-velvet-data/models/v1_full.h"))
    print("Velvet 1.0 detector")
    for aug in (False, True):
        sel = test & (A == aug)
        T3.report(f"TEST_VCTK {'chain' if aug else 'clean'}", T.mlp_prob(X[sel], m), Cl[sel], N[sel], E[sel])
    for te in ("slt", "bdl", "jmk"):
        m = header(os.path.expanduser(f"~/axell-velvet-data/models/no_{te}.h"))
        for aug in (False, True):
            sel = (V == te) & (A == aug)
            T3.report(f"{te} {'chain' if aug else 'clean'}", T.mlp_prob(X[sel], m), Cl[sel], N[sel], E[sel])
