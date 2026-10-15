"""Per-file offset between the 32 kHz originals and the 16 kHz release files the labels were made on (cross-correlation
of the original resampled to 16 kHz). Writes ~/axell-velvet-data/arctic/offsets.txt: speaker file offset_s corr."""
import os, wave, numpy as np
from scipy import signal as sg
from concurrent.futures import ProcessPoolExecutor
R = os.path.expanduser("~/axell-velvet-data")
def rd(p):
    w = wave.open(p); x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float)
    return x.reshape(-1, w.getnchannels())[:, 0]
def one(a):
    s, f = a
    try:
        r = rd(f"{R}/tmp16/cmu_us_{s}_arctic/wav/{f}.wav"); o = sg.resample_poly(rd(f"{R}/arctic/cmu_us_{s}_arctic/orig/{f}.wav"), 1, 2)
    except Exception:
        return None
    c = sg.fftconvolve(r, o[::-1], mode="full"); k = np.argmax(np.abs(c))
    corr = np.abs(c[k]) / np.sqrt((r * r).sum() * (o * o).sum())
    return s, f, -(k - (len(o) - 1)) / 16000.0, corr
if __name__ == "__main__":
    jobs = [(s, f[:-4]) for s in ("slt", "bdl", "jmk") for f in sorted(os.listdir(f"{R}/arctic/cmu_us_{s}_arctic/orig"))]
    with ProcessPoolExecutor() as ex:
        res = [r for r in ex.map(one, jobs, chunksize=20) if r]
    with open(f"{R}/arctic/offsets.txt", "w") as fo:
        for s, f, off, c in res:
            fo.write(f"{s} {f} {off:.5f} {c:.4f}\n")
    import collections
    for s in ("slt", "bdl", "jmk"):
        o = np.array([r[2] for r in res if r[0] == s]); c = np.array([r[3] for r in res if r[0] == s])
        vals = collections.Counter(np.round(o, 3)).most_common(4)
        print(s, len(o), "files; offsets", vals, "; min corr %.3f, files with corr < 0.9: %d" % (c.min(), (c < 0.9).sum()))

