"""What the de-esser does on a real take (no labels): every 4 ms frame of tools/VelvetDeEss's log (detector p, gain
reduction, HF re voiced body) is classified independently from the audio:
  vowel     - clearly periodic (normalised autocorrelation > 0.6 at 60-400 Hz) with < 5 % of the energy above 3 kHz
  sibilant  - clearly aperiodic (< 0.35) with > 60 % of the energy above 3 kHz, within 35 dB of the take's peak
and reports the cut on vowels (collateral), sibilants left uncut (misses) and what else gets cut.
usage: python3 diag_realtake.py take.wav frames.csv"""
import sys, wave, numpy as np

def read(p):
    w = wave.open(p); n = w.getnframes(); ch = w.getnchannels(); sw = w.getsampwidth(); raw = w.readframes(n)
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, np.int32) / 2.0 ** 31
    else:
        x = np.frombuffer(raw, np.int16) / 32768.0
    return w.getframerate(), x.reshape(-1, ch).mean(1)

def analyse(take, csv):
    fs, x = read(take)
    t, p, gr, hf = np.loadtxt(csv, delimiter=",").T
    N, W = int(0.012 * fs), int(0.015 * fs); lo, hi = int(fs / 400), int(fs / 60)
    rows = []
    for k in range(len(t)):
        i = int(t[k] * fs)
        if i - W < 0 or i + W > len(x):
            continue
        seg = x[i - N // 2:i + N // 2]
        S = np.abs(np.fft.rfft(seg * np.hanning(N))) ** 2; f = np.fft.rfftfreq(N, 1 / fs)
        w = x[i - W:i + W] - x[i - W:i + W].mean(); ac = np.correlate(w, w, "full")[len(w) - 1:]
        rows.append((t[k], p[k], gr[k], hf[k], 10 * np.log10(np.mean(seg ** 2) + 1e-20), S[f >= 3000].sum() / (S.sum() + 1e-30),
                     ac[lo:hi].max() / (ac[0] + 1e-20)))
    return np.array(rows)

if __name__ == "__main__":
    R = analyse(sys.argv[1], sys.argv[2])
    t, p, gr, hf, edb, hfr, per = R.T
    ref = np.percentile(edb, 99); active = edb > ref - 45
    vowel = active & (per > 0.6) & (hfr < 0.05)
    sib = active & (per < 0.35) & (hfr > 0.6) & (edb > ref - 35)
    cut = gr > 0.5
    print(f"frames {len(R)}: vowel {vowel.sum()}, sibilant {sib.sum()}")
    print(f"cut > 0.5 dB: {cut.sum()} frames | vowel {np.sum(cut & vowel)}, sibilant {np.sum(cut & sib)}, other {np.sum(cut & ~vowel & ~sib)}")
    print(f"vowels: mean cut {gr[vowel].mean():.3f} dB, max {gr[vowel].max():.2f} dB, frames > 1 dB {np.sum(vowel & (gr > 1))}")
    print(f"sibilants: mean cut {gr[sib].mean():.2f} dB, median {np.median(gr[sib]):.2f}; left uncut (< 0.5 dB) {np.sum(sib & ~cut)} of {sib.sum()}")
    unc = sib & ~cut
    if unc.any():
        print("HF re voiced body (dB), uncut sibilants p50/p90:", np.percentile(hf[unc], [50, 90]).round(1),
              "| cut sibilants:", np.percentile(hf[sib & cut], [50, 90]).round(1) if (sib & cut).any() else "-")
        print("uncut sibilant frames brighter than the voiced body (harsh):", np.sum(unc & (hf > 0)))
    print(f"detector p: sibilants median {np.median(p[sib]):.2f} (p10 {np.percentile(p[sib], 10):.2f}) | vowels p99 {np.percentile(p[vowel], 99):.3f}")
    o = cut & ~vowel & ~sib
    if o.any():
        print(f"other cut frames: {o.sum()}, HF ratio median {np.median(hfr[o]):.2f}, periodicity median {np.median(per[o]):.2f}, "
              f"cut median {np.median(gr[o]):.2f} dB, level re peak median {np.median(edb[o] - ref):.1f} dB")
