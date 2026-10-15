import sys, os, subprocess, numpy as np
sys.path.insert(0, ".")
import bench_cpp as B
from data import utterances, SIBILANT
import deess_proto as dp
from scipy.ndimage import uniform_filter1d
SCR = sys.argv[1]
ps, gr, hfs, cls_ = [], [], [], []
for spk in ("jmk", "bdl"):
    for i, (n, w, l) in enumerate(list(utterances(spk))[::16]):
        x, ph = B.test_signal(spk, n, w, l, True)
        b = f"{SCR}/d_{spk}_{n}"; B.writewav(b + ".wav", x, B.FS); x = B.readwav(b + ".wav")
        subprocess.run([f"{SCR}/DeEss_no_{spk}", b + ".wav", b + "_o.wav", "90", "18", "0", "0", b + ".csv"], check=True, stdout=subprocess.DEVNULL)
        y = B.readwav(b + "_o.wav"); fr = np.loadtxt(b + ".csv", delimiter=",")
        f, t, Z, win, hop = dp.stft(x, B.FS); P0 = np.abs(Z) ** 2; f, t, Z1, _, _ = dp.stft(y, B.FS); P1 = np.abs(Z1) ** 2
        h0 = 10 * np.log10(P0[(f >= 2000) & (f < 16000)].sum(0) + 1e-14); h1 = 10 * np.log10(P1[(f >= 2000) & (f < 16000)].sum(0) + 1e-14)
        b0 = 10 * np.log10(P0[(f >= 100) & (f < 1500)].sum(0) + 1e-14)
        nn = min(len(h0), len(h1)); tt = np.arange(nn) * dp.HOP_S; cl = np.full(nn, "", object)
        for a, bb, q in ph: cl[(tt >= a) & (tt < bb)] = q
        voiced = ~np.isin(cl, list(SIBILANT | {"p","t","k","b","d","g","f","v","th","dh","hh","pau",""}))
        refb = np.interp(tt, tt[voiced], uniform_filter1d(b0[:nn][voiced], 75))
        sib = np.isin(cl, list(SIBILANT)) & (h0[:nn] > np.percentile(h0[:nn], 50))
        harsh = sib & (h1[:nn] - refb > -3)
        p = np.interp(tt, fr[:, 0], fr[:, 1])
        ps.append(p[harsh]); gr.append((h0 - h1)[:nn][harsh]); hfs.append((h0[:nn] - refb)[harsh]); cls_.append(cl[harsh])
        for s in (".wav", "_o.wav", ".csv"): os.remove(b + s)
p = np.concatenate(ps); g = np.concatenate(gr); h = np.concatenate(hfs); c = np.concatenate(cls_)
print("remaining harsh frames:", len(p))
print("p quantiles (10/25/50/75/90):", np.round(np.percentile(p, [10, 25, 50, 75, 90]), 2))
print("cut applied dB quantiles:", np.round(np.percentile(g, [10, 25, 50, 75, 90]), 1))
print("level before re voice:", np.round(np.percentile(h, [10, 50, 90]), 1))
import collections; print("phones:", collections.Counter(c).most_common())
