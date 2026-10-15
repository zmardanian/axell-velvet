"""Compare de-esser variants with the best classic design at equal harshness (interpolated along each classic sweep)."""
import numpy as np, os, sys
A = np.load(os.path.expanduser(sys.argv[1]), allow_pickle=True)
C = np.load(os.path.expanduser("~/axell-velvet-data/bench_rows.npy"), allow_pickle=True)
flt = eval(sys.argv[2]) if len(sys.argv) > 2 else (lambda k: True)
for spk in ("slt", "bdl", "jmk"):
    for ch in (False, True):
        cl = [r for r in C if r["spk"] == spk and r["chain"] == ch and r["k"][0] in ("abs", "rel")]
        print(f"\n{spk} {'chain' if ch else 'clean'}  (harsh before {cl[0]['harsh0']:.1f}%)")
        def classic_at(h, key):
            best = None
            for kind in ("abs", "rel"):
                rs = sorted([r for r in cl if r["k"][0] == kind], key=lambda r: r["harsh"])
                if h < rs[0]["harsh"]:
                    continue
                for a, b in zip(rs[:-1], rs[1:]):
                    if a["harsh"] <= h <= b["harsh"]:
                        t = (h - a["harsh"]) / max(b["harsh"] - a["harsh"], 1e-9); v = a[key] + t * (b[key] - a[key])
                        best = v if best is None or abs(v) < abs(best) else best
            return best
        for r in sorted(A, key=lambda r: str(r["k"])):
            if r["spk"] == spk and r["chain"] == ch and flt(r["k"]):
                cv, cs, cso = classic_at(r["harsh"], "vow"), classic_at(r["harsh"], "stops"), classic_at(r["harsh"], "soft")
                f = lambda v: "  n/a" if v is None else f"{v:5.2f}"
                print(f"  {str(r['k'][1:]):16s} harsh {r['harsh']:4.1f}% | vowels {r['vow']:.3f} (classic {f(cv)}) stops {r['stops']:.2f} (classic {f(cs)}) soft {r['soft']:+.2f} (classic {f(cso)})")
