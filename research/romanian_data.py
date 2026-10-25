"""Romanian training and test data from the artist's own takes (Velvet 2.0). The audio is only read.

There are no phone labels, so frames are labelled only where the acoustics leave no doubt (everything else stays
unlabelled and is not used):
  sibilant, unvoiced (s, ș, the s of ț and ce/ci): aperiodic (normalised autocorrelation < 0.35 at 60-400 Hz), > 60 % of
      the energy above 3 kHz, spectral centroid above 2 kHz at 2.5-10 kHz, within 35 dB of the take's peak
  sibilant, voiced (z, j, ge/gi): partly periodic (0.35-0.7), > 35 % of the energy above 3 kHz, centroid 2.5-9 kHz,
      within 30 dB of the peak
  not sibilant: clearly periodic (> 0.6) with < 5 % of the energy above 3 kHz (vowels, voiced consonants), or quiet
      (> 50 dB under the peak: pauses)
Features come from the shipping C++ detector (tools/VelvetFeatures), exactly as for CMU ARCTIC.
usage: python3 romanian_data.py <VelvetFeatures binary>  -> ~/axell-velvet-data/romanian.npz"""
import os, sys, subprocess, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag_realtake as D
import train_cpp as T
import takes

TAKES = takes.romanian()   # name: (path, role) - "train" or "test" (held out: never seen in training)
OUT = os.path.expanduser("~/axell-velvet-data/romanian")
stem = lambda name: "".join(c if c.isalnum() else "_" for c in name)   # the feature tool's manifest is space-separated

def acoustic_labels(x, fs, t):
    """per feature frame (centre times t): 1 sibilant, 0 not, -1 unlabelled; and the sibilant kind (u / v)"""
    N, W = int(0.012 * fs), int(0.015 * fs); lo, hi = int(fs / 400), int(fs / 60)
    e = np.full(len(t), -200.0); hfr = np.zeros(len(t)); per = np.zeros(len(t)); cen = np.zeros(len(t))
    f = np.fft.rfftfreq(N, 1 / fs); hiband = f >= 3000; cb = f >= 2000
    for k, tc in enumerate(t):
        i = int(tc * fs)
        if i - W < 0 or i + W > len(x):
            continue
        seg = x[i - N // 2:i + N // 2]; S = np.abs(np.fft.rfft(seg * np.hanning(N))) ** 2
        w = x[i - W:i + W] - x[i - W:i + W].mean(); ac = np.correlate(w, w, "full")[len(w) - 1:]
        e[k] = 10 * np.log10(np.mean(seg ** 2) + 1e-20); hfr[k] = S[hiband].sum() / (S.sum() + 1e-30)
        per[k] = ac[lo:hi].max() / (ac[0] + 1e-20); cen[k] = np.sum(f[cb] * S[cb]) / (np.sum(S[cb]) + 1e-30)
    ref = np.percentile(e[e > -199], 99)
    unv = (e > ref - 35) & (per < 0.35) & (hfr > 0.6) & (cen > 2500) & (cen < 10000)
    vcd = (e > ref - 30) & (per >= 0.35) & (per < 0.7) & (hfr > 0.35) & (cen > 2500) & (cen < 9000)
    neg = ((e > ref - 45) & (per > 0.6) & (hfr < 0.05)) | ((e > -199) & (e < ref - 50))
    lab = np.full(len(t), -1); lab[neg] = 0; lab[unv | vcd] = 1
    kind = np.where(unv, "u", np.where(vcd, "v", ""))
    return lab, kind

if __name__ == "__main__":
    tool = sys.argv[1]; os.makedirs(OUT, exist_ok=True)
    lines = []
    for name, (p, role) in TAKES.items():
        fs, x = D.read(p)
        x.astype(np.float32).tofile(f"{OUT}/{stem(name)}.f32")
        lines.append(f"{OUT}/{stem(name)}.f32 {fs} {OUT}/{stem(name)}.feat\n")
    open(f"{OUT}/manifest.txt", "w").writelines(lines)
    subprocess.run([tool, f"{OUT}/manifest.txt"], check=True, stdout=subprocess.DEVNULL)
    rows = {}
    for name, (p, role) in TAKES.items():
        fs, x = D.read(p)
        d = np.fromfile(f"{OUT}/{stem(name)}.feat", np.float32).reshape(-1, T.NC).astype(np.float64)
        lab, kind = acoustic_labels(x, fs, d[:, 0])
        rows[name] = dict(X=d[:, T.COL["x"]], E=d[:, T.COL["E"]].sum(1), lab=lab, kind=kind, role=role)
        os.remove(f"{OUT}/{stem(name)}.f32")
        print(f"{name:36s} {role:5s} frames {len(d)}: sibilant {np.sum(lab == 1)} (voiced {np.sum(kind == 'v')}), "
              f"not {np.sum(lab == 0)}, unlabelled {np.sum(lab == -1)}")
    np.savez_compressed(os.path.expanduser("~/axell-velvet-data/romanian.npz"),
                        **{f"{k}|{f}": v for k, r in rows.items() for f, v in r.items()})
