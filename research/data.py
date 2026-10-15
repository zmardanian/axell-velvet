"""CMU ARCTIC (Carnegie Mellon University, free for any use): 32 kHz original recordings (ch0 speech, ch1 EGG) with the
release's automatic phone labels, aligned per file (align.py)."""
import os, wave, numpy as np
ROOT = os.path.expanduser("~/axell-velvet-data/arctic")
# per-file offsets measured by align.py (cross-correlation with the 16 kHz release files); files whose correlation is
# below 0.9 do not match their labels and are skipped
OFFSETS = {}
for _l in open(os.path.join(ROOT, "offsets.txt")):
    _s, _f, _o, _c = _l.split()
    if float(_c) >= 0.9:
        OFFSETS[(_s, _f)] = float(_o)
SIBILANT = {"s", "z", "sh", "zh", "ch", "jh"}
FRICATIVE_OTHER = {"f", "v", "th", "dh", "hh"}
STOPS = {"p", "t", "k", "b", "d", "g"}

def utterances(spk):
    d = os.path.join(ROOT, f"cmu_us_{spk}_arctic")
    for f in sorted(os.listdir(os.path.join(d, "orig"))):
        lab = os.path.join(d, "lab", f[:-4] + ".lab")
        if os.path.exists(lab) and (spk, f[:-4]) in OFFSETS:
            yield f[:-4], os.path.join(d, "orig", f), lab

def load(spk, wavp, labp):
    w = wave.open(wavp)
    fs = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64).reshape(-1, w.getnchannels()) / 32768.0
    phones, t0 = [], 0.0
    for line in open(labp):
        p = line.split()
        if len(p) == 3 and p[0][0].isdigit():
            t1 = float(p[0]) + OFFSETS[(spk, os.path.basename(labp)[:-4])]
            phones.append((t0, t1, p[2]))
            t0 = t1
    return fs, x[:, 0], x[:, 1], phones

def frame_labels(phones, n_frames, hop_s, cls):
    lab = np.zeros(n_frames, bool)
    for a, b, p in phones:
        if p in cls:
            lab[int(np.ceil(a / hop_s)):int(b / hop_s)] = True
    return lab
