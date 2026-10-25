"""Candidate plosive pops in the artist's takes, for labelling by eye: every place where the 20-150 Hz envelope rises by
8 dB or more within 15 ms and comes within 35 dB of the voice's peak. Each candidate gets a spectrogram tile (-150 ms
.. +250 ms: 0-600 Hz in detail above, 0-6 kHz context below), numbered, on contact sheets.
usage: python3 pop_candidates.py -> ~/axell-velvet-data/pops/candidates.json + sheet_*.png"""
import os, sys, glob, json, numpy as np
from scipy import signal as sg
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag_realtake as D
OUT = os.path.expanduser("~/axell-velvet-data/pops")
import takes
FILES = takes.real_takes()

def env(v, fs, ms):
    a = np.exp(-1.0 / (ms * 1e-3 * fs)); return np.sqrt(sg.lfilter([1 - a], [1, -a], v * v) + 1e-24)

cands = []
for fi, f in enumerate(FILES):
    fs, x = D.read(f); H = int(0.001 * fs)
    L = 20 * np.log10(env(sg.sosfilt(sg.butter(2, [20, 150], "band", fs=fs, output="sos"), x), fs, 5)[::H])
    B = 20 * np.log10(env(sg.sosfilt(sg.butter(2, [250, 4000], "band", fs=fs, output="sos"), x), fs, 10)[::H])
    top = np.percentile(B, 99)
    rise = L - np.concatenate([np.full(15, L[0]), L[:-15]])
    hit = np.flatnonzero((rise >= 8) & (L > top - 35)); last = -10 ** 9
    for k in hit:
        if k - last > 200:
            cands.append(dict(file=f, fi=fi, t=float(k / 1000.0)))
        last = k
json.dump(cands, open(f"{OUT}/candidates.json", "w"), indent=1)
print(len(cands), "candidates")
per = 24
for s0 in range(0, len(cands), per):
    fig, axs = plt.subplots(4, 6, figsize=(24, 13))
    for j, ax in enumerate(axs.flat):
        n = s0 + j
        if n >= len(cands):
            ax.axis("off"); continue
        c = cands[n]; fs, x = D.read(c["file"]); a = int((c["t"] - 0.15) * fs); b = int((c["t"] + 0.25) * fs)
        seg = x[max(0, a):b]
        f_, t_, S = sg.spectrogram(seg, fs, nperseg=2048, noverlap=2048 - 96)
        Sd = 10 * np.log10(S + 1e-14); vmax = Sd.max()
        m = f_ <= 600
        ax.pcolormesh(t_ * 1000 - 150, f_[m], Sd[m], vmin=vmax - 60, vmax=vmax, shading="auto", cmap="magma")
        ax.axvline(0, color="c", lw=0.8)
        ax.set_title(f"#{n}  {os.path.basename(c['file'])[:22]} {c['t']:.2f}s", fontsize=9)
        ax.tick_params(labelsize=7)
    fig.tight_layout(); fig.savefig(f"{OUT}/sheet_{s0 // per:02d}.png", dpi=55); plt.close(fig)
print("sheets in", OUT)
