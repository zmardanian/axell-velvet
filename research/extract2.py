import sys, numpy as np
sys.path.insert(0, ".")
from data import *
from features2 import *
import os
DATA = os.path.expanduser(os.environ.get("AXELL_VELVET_DATA", "~/axell-velvet-data"))
spk = sys.argv[1]
Fs, Ls, Cs = [], [], []
for name, w, l in utterances(spk):
    fs, x, egg, ph = load(spk, w, l)
    t, F = features(x, fs)
    cls = np.full(len(t), "", object)
    for a, b, p in ph:
        cls[(t >= a) & (t < b)] = p
    near = np.zeros(len(t), bool)
    for a, b, p in ph:
        if p in SIBILANT:
            near |= (np.abs(t - a) < 0.010) | (np.abs(t - b) < 0.010)
    Fs.append(F); Ls.append(cls); Cs.append(near)
np.savez_compressed(f"{DATA}/feat2_{spk}.npz", F=np.concatenate(Fs), cls=np.concatenate(Ls).astype(str), near=np.concatenate(Cs),
                    utt=np.concatenate([np.full(len(f), i) for i, f in enumerate(Fs)]))
print(spk, sum(len(f) for f in Fs), "frames")
