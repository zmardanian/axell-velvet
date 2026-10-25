"""LIVE detector variants on the 12 held-out VCTK voices (bench3.py's takes and measures; Velvet only): how much harsh
sibilance each can remove at all (its sweep's floor) and the collateral at equal harshness.
usage: python3 bench3_live.py <scratch> <name>=<tool>[:live] ...   (rows -> ~/axell-velvet-data/bench3/live_rows.npy)"""
import os, sys, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
ARGS = sys.argv[1:]
sys.argv = ["x", "."]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench3 as B3, bench_cpp as B, corpus3 as C3

AMTS = (40, 60, 75, 90, 100)

def job(a):
    voice, chain, part, scr, tools = a
    x, ph = B3.take(voice, chain, part)
    base = f"{scr}/L_{voice}_{int(chain)}_{part}"
    B.writewav(base + ".wav", x, B3.FS); x = B.readwav(base + ".wav")
    rows = []
    for name, tool, live in tools:
        for amt in AMTS:
            subprocess.run([tool, base + ".wav", base + "_o.wav", str(amt), "18", "0", "0", "-"] + (["--live"] if live else []), check=True, stdout=subprocess.DEVNULL)
            m = B.measure(x, B.readwav(base + "_o.wav"), ph)
            if m is not None:
                rows.append((voice, chain, (name, amt), m))
    for s in (".wav", "_o.wav"):
        os.remove(base + s)
    return rows

if __name__ == "__main__":
    scr = ARGS[0]; os.makedirs(scr, exist_ok=True)
    tools = []
    for t in ARGS[1:]:
        name, rest = t.split("=", 1); live = rest.endswith(":live"); tools.append((name, rest[:-5] if live else rest, live))
    jobs = [(v, c, k, scr, tools) for v in C3.TEST_VCTK for c in (False, True) for k in range(B3.parts(v))]
    with ProcessPoolExecutor(3) as ex:
        rows = [r for rs in ex.map(job, jobs) for r in rs]
    np.save(os.path.expanduser("~/axell-velvet-data/bench3/live_rows.npy"), np.array(rows, dtype=object), allow_pickle=True)
    S = B3.summarize(rows)
    for chain in (False, True):
        h0 = [v for (g, c, k), v in S.items() if g == "VCTK" and c == chain][0]["harsh0"]
        print(f"\nVCTK {'chain' if chain else 'clean'}: harsh before {h0:.1f}%")
        for name, _, _ in tools:
            rs = sorted([(k[1], v) for (g, c, k), v in S.items() if g == "VCTK" and c == chain and k[0] == name])
            print(f"  {name:14s} " + " | ".join(f"{a}%: harsh {v['harsh']:.1f} vow {v['vow']:.3f} stop {v['stops']:.2f} soft {v['soft']:+.1f}" for a, v in rs))
        for frac in (0.5, 0.3):
            h = h0 * frac
            print(f"  at harsh {h:.1f}%: " + " | ".join(f"{n} vow {B3.at_harsh(S, 'VCTK', chain, n, h, 'vow') or float('nan'):.3f} stop {B3.at_harsh(S, 'VCTK', chain, n, h, 'stops') or float('nan'):.2f}" for n, _, _ in tools))
