"""De-esser benchmark: the shipping C++ de-esser (held-out model: the test voice was never seen in training) against two
classic designs, each swept over its strength control, on unseen voices. Trade-off = how many harsh esses remain vs.
what happens to everything else; a better de-esser removes more harshness for less collateral damage.

Test material per voice: every 4th utterance at 48 kHz, peak-normalised (the user sets a de-esser for a track), with a
+-8 dB level ride (verse / chorus, singer movement); "chain" = through a random recording chain first (mic presence
peak, tilt, fast compressor). Classic designs (4 ms look-ahead like ours):
  CLASSIC-ABS: split-band compressor - 4-10 kHz side chain over a fixed threshold, ratio 4, everything above 3.5 kHz
               turned down (the classic hardware design).
  CLASSIC-REL: same, but the side chain is the 4-10 kHz level RELATIVE to the full-band level (auto-threshold designs):
               level independent, the strongest classic baseline.
Measures (2-16 kHz energy, 16 ms frames, relative to the running voice-body level of voiced frames):
  harsh      share of sibilant frames above -3 dB re voice (louder than the voice body: they cut through)
  soft cut   mean change of sibilant frames that were below -15 dB (already soft: cutting them = lisp)
  collateral mean |change| on vowels / voiced, stops, other fricatives away from sibilant edges (+-40 ms)"""
import os, sys, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
from scipy import signal as sg
from scipy.ndimage import uniform_filter1d
sys.path.insert(0, os.path.dirname(__file__))
from data import utterances, load, SIBILANT, STOPS, FRICATIVE_OTHER
import train_cpp as T
import deess_proto as dp

SCR = sys.argv[1] if len(sys.argv) > 1 else "."         # scratch dir with DeEss_no_<spk> tools
FS = 48000
SPK = ("slt", "bdl", "jmk")
HARSH = -3.0

def writewav(p, x, fs):
    import wave
    y = np.clip(x, -1, 1); w = wave.open(p, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs)
    w.writeframes((y * 32767).astype("<i2").tobytes()); w.close()

def readwav(p):
    from wav24 import read_wav
    return read_wav(p)[1][:, 0]

def test_signal(spk, name, w, l, chain):
    import zlib
    rng = np.random.default_rng(zlib.crc32(f"bench/{spk}/{name}/{chain}".encode()))
    fs0, x, egg, ph = load(spk, w, l)
    x = sg.resample_poly(x, 3, 2)
    if chain:
        b, a = T.peaking(FS, rng.uniform(3000, 12000), rng.uniform(0.7, 2.0), rng.uniform(0, 6)); x = sg.lfilter(b, a, x)
        b, a = T.shelf(FS, 5000, rng.uniform(-4, 4)); x = sg.lfilter(b, a, x)
        ka = np.exp(-1 / (0.0005 * FS)); env = sg.lfilter([1 - ka], [1, -ka], np.abs(x)); ldb = 20 * np.log10(env + 1e-9)
        thr = np.percentile(ldb[ldb > -80], 97) - rng.uniform(3, 12); gr = np.minimum(0, (thr - ldb) * (1 - 1 / rng.uniform(4, 12)))
        kr = np.exp(-1 / (0.08 * FS)); x = x * 10 ** (sg.lfilter([1 - kr], [1, -kr], gr) / 20)
    x = x / np.max(np.abs(x)) * 10 ** (-14 / 20)
    t = np.arange(len(x)) / FS
    x = x * 10 ** ((8 * np.sin(2 * np.pi * t / rng.uniform(2, 6) + rng.uniform(0, 6.3))) / 20)
    return x, ph

def classic(x, thr_db, relative):
    f, t, Z, win, hop = dp.stft(x, FS); P = np.abs(Z) ** 2
    sc = 10 * np.log10(P[(f >= 4000) & (f < 10000)].sum(0) + 1e-14)
    if relative:
        sc = sc - 10 * np.log10(P.sum(0) + 1e-14)
    g = -np.maximum(sc - thr_db, 0) * (1 - 1 / 4.0)
    g = dp.smooth_db(g[None], 1, 25, 2)[0]
    G = np.ones_like(P); G[f >= 3500] = 10 ** (g / 20)
    return dp.istft(Z * G, FS, win, hop, len(x))

def measure(x, y, ph):
    def bands(s):
        f, t, Z, win, hop = dp.stft(s, FS); P = np.abs(Z) ** 2
        return 10 * np.log10(P[(f >= 2000) & (f < 16000)].sum(0) + 1e-14), 10 * np.log10(P[(f >= 100) & (f < 1500)].sum(0) + 1e-14)
    h0, b0 = bands(x); h1, _ = bands(y); n = min(len(h0), len(h1)); h0, b0, h1 = h0[:n], b0[:n], h1[:n]
    tt = np.arange(n) * dp.HOP_S; cl = np.full(n, "", object); edge = np.zeros(n, bool)
    for a, b, q in ph:
        cl[(tt >= a) & (tt < b)] = q
        if q in SIBILANT:
            edge |= (np.abs(tt - a) < 0.04) | (np.abs(tt - b) < 0.04)
    sib = np.isin(cl, list(SIBILANT)) & (h0 > np.percentile(h0, 50))
    voiced = ~np.isin(cl, list(SIBILANT | STOPS | FRICATIVE_OTHER | {"pau", ""}))
    if voiced.sum() < 10:
        return None
    refb = np.interp(tt, tt[voiced], uniform_filter1d(b0[voiced], 75))
    d = h1 - h0; act = h0 > np.percentile(h0, 30)
    return dict(before=(h0 - refb)[sib], after=(h1 - refb)[sib],
                vow=np.abs(d[voiced & ~edge & act]), stops=np.abs(d[np.isin(cl, list(STOPS)) & ~edge & act]),
                fric=np.abs(d[np.isin(cl, list(FRICATIVE_OTHER)) & ~edge & act]))

AXELL = eval(os.environ.get("BENCH_AXELL", "[(a, r, 6.0) for r in (12, 18) for a in (25, 40, 50, 60, 75, 90)]"))
ONLY_AXELL = os.environ.get("BENCH_ONLY_AXELL") == "1"
ABS = [-62, -58, -54, -50, -46, -42, -38]
REL = [-26, -22, -18, -14, -10, -6]

def one(job):
    spk, name, w, l, chain, idx = job
    x, ph = test_signal(spk, name, w, l, chain)
    base = f"{SCR}/b_{spk}_{name}_{int(chain)}"
    writewav(base + ".wav", x, FS)
    x = readwav(base + ".wav")
    out = {}
    for a, r, net in AXELL:
        subprocess.run([f"{SCR}/DeEss_no_{spk}", base + ".wav", base + "_o.wav", str(a), str(r), "0", "0", "-", str(net)], check=True, stdout=subprocess.DEVNULL)
        out[("axell", a, r, net)] = measure(x, readwav(base + "_o.wav"), ph)
    for th in ([] if ONLY_AXELL else ABS):
        out[("abs", th)] = measure(x, classic(x, th, False), ph)
    for th in ([] if ONLY_AXELL else REL):
        out[("rel", th)] = measure(x, classic(x, th, True), ph)
    for s in (".wav", "_o.wav"):
        os.remove(base + s)
    return spk, chain, out

if __name__ == "__main__":
    jobs = [(s, n, w, l, c, i) for s in SPK for i, (n, w, l) in enumerate(list(utterances(s))[::4]) for c in (False, True)]
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(one, jobs, chunksize=4))
    acc = {}
    for spk, chain, out in res:
        for k, m in out.items():
            if m is None:
                continue
            a = acc.setdefault((spk, chain, k), {kk: [] for kk in m}); [a[kk].append(v) for kk, v in m.items()]
    rows = []
    for (spk, chain, k), a in sorted(acc.items(), key=lambda z: (z[0][0], z[0][1], str(z[0][2]))):
        b = np.concatenate(a["before"]); af = np.concatenate(a["after"]); soft = b < -15
        r = dict(spk=spk, chain=chain, k=k, harsh0=100 * (b > HARSH).mean(), harsh=100 * (af > HARSH).mean(), loud90=np.percentile(af, 90),
                 soft=np.mean(af[soft] - b[soft]), vow=np.concatenate(a["vow"]).mean(), stops=np.concatenate(a["stops"]).mean(),
                 fric=np.concatenate(a["fric"]).mean())
        rows.append(r)
    np.save(os.path.expanduser(os.environ.get("BENCH_OUT", "~/axell-velvet-data/bench_rows.npy")), np.array(rows, dtype=object), allow_pickle=True)
    for spk in SPK:
        for chain in (False, True):
            print(f"\n{spk} {'chain' if chain else 'clean'} (harsh esses before: {[r for r in rows if r['spk'] == spk and r['chain'] == chain][0]['harsh0']:.1f}%)")
            for r in rows:
                if r["spk"] == spk and r["chain"] == chain:
                    print(f"  {str(r['k']):22s} harsh {r['harsh']:5.1f}%  loud90 {r['loud90']:+6.1f}  soft cut {r['soft']:+6.2f} dB  collateral: vowels {r['vow']:.3f}  stops {r['stops']:.3f}  fric {r['fric']:.3f} dB")
