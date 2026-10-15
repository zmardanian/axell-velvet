import sys, numpy as np
sys.path.insert(0, ".")
from data import *
from features import *
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))
spk = sys.argv[1]
Fs, Ls, Cs = [], [], []
for name, w, l in utterances(spk):
    fs, x, egg, ph = load(spk, w, l)
    t, F = features(x, fs)
    hop = HOP / fs
    # frame centres: the STFT frame i covers [i*hop, i*hop + win]; its centre is i*hop + win/2
    centre = t
    cls = np.full(len(t), "", object)
    for a, b, p in ph:
        cls[(centre >= a) & (centre < b)] = p
    # boundary tolerance: frames within 10 ms of a sibilant boundary are excluded from scoring
    near = np.zeros(len(t), bool)
    for a, b, p in ph:
        if p in SIBILANT:
            near |= (np.abs(centre - a) < 0.010) | (np.abs(centre - b) < 0.010)
    Fs.append(F); Ls.append(cls); Cs.append(near)
np.savez_compressed(f"{DATA}/feat_{spk}.npz", F=np.concatenate(Fs), cls=np.concatenate(Ls).astype(str), near=np.concatenate(Cs))
print(spk, sum(len(f) for f in Fs), "frames")
