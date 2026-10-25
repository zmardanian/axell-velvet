"""Velvet 1.0 vs 3.0 (studio, LIVE) on the artist's own takes (the takes manifest: Romanian takes + the real takes; no
labels): diag_realtake.py's acoustic classes per 4 ms frame - cut on clear vowels (collateral), clear sibilants left
uncut, and of those the harsh ones (treble above the voiced body). AMOUNT 50 (the artist's natural setting) and 70.
usage: python3 realtakes3.py   (uses the benchmark tools in ~/axell-velvet-data/bench3/bin, *_vctk: no Romanian take,
and no take of the artist, was ever trained on)"""
import os, sys, subprocess, tempfile, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag_realtake as D, takes

BIN = os.path.expanduser("~/axell-velvet-data/bench3/bin")
VARIANTS = [("Velvet 1.0", "DeEss_v1_vctk", []), ("Velvet 3.0", "DeEss_v3_vctk", []), ("Velvet 3.0 LIVE", "DeEss_v3live_vctk", ["--live"])]

def files():
    out = [p for p, role in takes.romanian().values()]
    for p in takes.real_takes():
        if p not in out:
            out.append(p)
    return [p for p in out if os.path.exists(p)]

if __name__ == "__main__":
    fl = files()
    print(f"{len(fl)} takes")
    for amt in (50, 70):
        for name, tool, extra in VARIANTS:
            tot = dict(vow=0, vow1=0, vowcut=0.0, sib=0, unc=0, harsh=0)
            for f in fl:
                with tempfile.TemporaryDirectory(dir=os.path.expanduser("~/axell-velvet-data")) as td:
                    csv = f"{td}/f.csv"
                    subprocess.run([f"{BIN}/{tool}", f, f"{td}/o.wav", str(amt), "18", "0", "0", csv] + extra, check=True, stdout=subprocess.DEVNULL)
                    R = D.analyse(f, csv)
                t, p, gr, hf, edb, hfr, per = R.T
                ref = np.percentile(edb, 99); active = edb > ref - 45
                vowel = active & (per > 0.6) & (hfr < 0.05); sib = active & (per < 0.35) & (hfr > 0.6) & (edb > ref - 35)
                cut = gr > 0.5
                tot["vow"] += vowel.sum(); tot["vow1"] += np.sum(vowel & (gr > 1)); tot["vowcut"] += gr[vowel].sum()
                tot["sib"] += sib.sum(); tot["unc"] += np.sum(sib & ~cut); tot["harsh"] += np.sum(sib & ~cut & (hf > 0))
            print(f"AMOUNT {amt} {name:16s}: vowels mean cut {tot['vowcut'] / tot['vow']:.4f} dB, > 1 dB on {tot['vow1']} of {tot['vow']} | "
                  f"sibilants uncut {tot['unc']} of {tot['sib']} ({100 * tot['unc'] / tot['sib']:.1f}%), harsh uncut {tot['harsh']}", flush=True)
