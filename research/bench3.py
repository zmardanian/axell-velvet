"""Velvet 3.0 de-esser benchmark on voices no tested model was trained on: Velvet 1.0, Velvet 3.0 (studio and LIVE),
the two classic designs of bench_cpp.py and a third-party sibilance-aware de-esser (nClear Sibalance 1.1.0, a VST3
the user has installed, 20 ms look-ahead; rendered black-box through the suite's independent VST3 host and aligned by
its reported latency), each swept over its strength control (Sibalance: Sibilance Gain at Sensitivity 50 and 80).

Voices: CMU ARCTIC slt / bdl / jmk (every 8th utterance; Velvet models trained without that voice) and the 12
corpus3.TEST_VCTK voices (all 60 utterances; models trained without any of them; 1.0 never saw VCTK). Per voice and
condition takes of 20 utterances joined (as a vocal take, ~1 min), peak at -14 dBFS, a +-8 dB level ride; "chain" = through a
random recording chain first (as in bench_cpp.py). Measures as bench_cpp.measure (harsh share of sibilant frames
above -3 dB re the voice body; soft cut; collateral on vowels / stops / other fricatives away from sibilant edges).
Comparison at equal harshness: every processor's sweep is interpolated at the same remaining-harsh share.
usage: python3 bench3.py render <scratch dir> <AuditHost> ; python3 bench3.py report"""
import os, re, sys, zlib, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_argv = sys.argv; sys.argv = ["x", "."]
import bench_cpp as B
sys.argv = _argv
import corpus3 as C3, train_cpp as T

FS = 48000
OUT = os.path.expanduser("~/axell-velvet-data/bench3")
SIB = os.path.expanduser("~/.vst3/Sibalance.vst3")
AMOUNTS = (25, 40, 50, 60, 75, 90)
SIB_GAIN = (-3, -6, -9, -12, -15, -20, -30)
SIB_SENS = (50, 80)          # its default sensitivity, and a higher one (catches more esses)
VARIANTS = ("v1", "v3", "v3live")

PART = 20   # utterances per take (~1 min): a whole voice in one take needs several GB in the STFT-based measures

def parts(voice):
    n = len(C3.utts(voice)[::8] if voice in ("slt", "bdl", "jmk") else C3.utts(voice))
    return (n + PART - 1) // PART

def take(voice, chain, part=0):
    rng = np.random.default_rng(zlib.crc32(f"bench3/{voice}/{chain}/{part}".encode()))
    U = C3.utts(voice)
    U = U[::8] if voice in ("slt", "bdl", "jmk") else U
    U = U[part * PART:(part + 1) * PART]
    xs, ph, t0 = [], [], 0.0
    for name, ld in U:
        fs0, x, p = C3.load_utt(voice, name)
        from math import gcd
        g = gcd(int(fs0), FS); x = sg.resample_poly(x, FS // g, int(fs0) // g)
        x = x / (np.max(np.abs(x)) + 1e-9)
        gap = np.zeros(int(rng.uniform(0.05, 0.3) * FS))
        xs += [x, gap]; ph += [(a + t0, b + t0, q) for a, b, q in p]; t0 += (len(x) + len(gap)) / FS
    x = np.concatenate(xs)
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

def tool(variant, voice):
    held = f"no_{voice}" if voice in ("slt", "bdl", "jmk") else "vctk"
    return f"{OUT}/bin/DeEss_{variant}_{held}"

def job(a):
    voice, chain, part, scr, host = a
    x, ph = take(voice, chain, part)
    base = f"{scr}/{voice}_{int(chain)}_{part}"
    B.writewav(base + ".wav", x, FS); x = B.readwav(base + ".wav")
    rows = []
    def add(key, y):
        m = B.measure(x, y, ph)
        if m is not None:
            rows.append((voice, chain, key, m))
    for v in VARIANTS:
        for amt in AMOUNTS:
            subprocess.run([tool(v, voice), base + ".wav", base + "_o.wav", str(amt), "18", "0", "0", "-"] + (["--live"] if v.endswith("live") else []),
                           check=True, stdout=subprocess.DEVNULL)
            add((v, amt), B.readwav(base + "_o.wav"))
    for th in B.ABS:
        add(("abs", th), B.classic(x, th, False))
    for th in B.REL:
        add(("rel", th), B.classic(x, th, True))
    # the third-party de-esser: stereo float32 through the independent host (latency 0 reported; checked below)
    pad = FS // 10   # room for the plugin's reported latency (Sibalance: 960 samples at 48 kHz), removed below
    np.repeat(np.concatenate([x, np.zeros(pad)])[:, None], 2, 1).astype(np.float32).tofile(base + ".f32")
    for sens in SIB_SENS:
        for gdb in SIB_GAIN:
            jf = base + f"_sib{sens}_{gdb}.txt"
            open(jf, "w").write("\n".join(["rate 48000", "channels 2", "blocks 512", "nonrealtime 1", "preroll 0",
                                           f"set Sensitivity={sens}", f"set Sibilance Gain={gdb:.1f}", f"input {base}.f32", f"output {base}_s.f32", "run"]) + "\n")
            r = subprocess.run([host, SIB, jf], check=True, capture_output=True, text=True)
            os.remove(jf)
            lat = int(re.search(r"latency (\d+)", r.stdout).group(1))
            assert lat <= pad
            y = np.fromfile(base + "_s.f32", np.float32).reshape(-1, 2)[:, 0].astype(np.float64)[lat:lat + len(x)]
            add((f"sibalance{sens}", gdb), y)
    for s in (".wav", "_o.wav", ".f32", "_s.f32"):
        if os.path.exists(base + s):
            os.remove(base + s)
    return rows

def summarize(rows):
    acc = {}
    for voice, chain, key, m in rows:
        grp = "ARCTIC" if voice in ("slt", "bdl", "jmk") else "VCTK"
        for g in (grp, "ALL"):
            a = acc.setdefault((g, chain, key), {k: [] for k in m}); [a[k].append(v) for k, v in m.items()]
    out = {}
    for (g, chain, key), a in acc.items():
        b = np.concatenate(a["before"]); af = np.concatenate(a["after"]); soft = b < -15
        out[(g, chain, key)] = dict(harsh0=100 * (b > B.HARSH).mean(), harsh=100 * (af > B.HARSH).mean(),
                                    soft=np.mean(af[soft] - b[soft]), vow=np.concatenate(a["vow"]).mean(),
                                    stops=np.concatenate(a["stops"]).mean(), fric=np.concatenate(a["fric"]).mean())
    return out

def at_harsh(S, g, chain, kind, h, key):
    rs = sorted([v for (gg, cc, k), v in S.items() if gg == g and cc == chain and k[0] == kind], key=lambda r: r["harsh"])
    if not rs or h < rs[0]["harsh"] or h > rs[-1]["harsh"]:
        return None
    for a, b in zip(rs[:-1], rs[1:]):
        if a["harsh"] <= h <= b["harsh"]:
            t = (h - a["harsh"]) / max(b["harsh"] - a["harsh"], 1e-9); return a[key] + t * (b[key] - a[key])
    return rs[0][key] if abs(rs[0]["harsh"] - h) < 1e-9 else None

if __name__ == "__main__":
    if sys.argv[1] == "render":
        scr, host = sys.argv[2], sys.argv[3]; os.makedirs(scr, exist_ok=True)
        voices = ["slt", "bdl", "jmk"] + C3.TEST_VCTK
        jobs = [(v, c, k, scr, host) for v in voices for c in (False, True) for k in range(parts(v))]
        print(len(jobs), "takes", flush=True)
        # 3 workers: each holds a take (40-90 MB) and plugin instances; 8 ran the 15 GB machine out of memory
        with ProcessPoolExecutor(int(os.environ.get("BENCH3_WORKERS", "3"))) as ex:
            rows = [r for rs in ex.map(job, jobs) for r in rs]
        np.save(f"{OUT}/rows.npy", np.array(rows, dtype=object), allow_pickle=True)
    rows = list(np.load(f"{OUT}/rows.npy", allow_pickle=True))
    S = summarize(rows)
    kinds = [("v1", "Velvet 1.0"), ("v3", "Velvet 3.0"), ("v3live", "Velvet 3.0 LIVE"), ("sibalance50", "Sibalance sens 50"), ("sibalance80", "Sibalance sens 80"), ("abs", "classic abs"), ("rel", "classic rel")]
    for g in ("ARCTIC", "VCTK", "ALL"):
        for chain in (False, True):
            h0 = [v for (gg, cc, k), v in S.items() if gg == g and cc == chain][0]["harsh0"]
            print(f"\n== {g} {'chain' if chain else 'clean'}: harsh esses before {h0:.1f}%")
            print("   sweeps (harsh left %): " + " | ".join(f"{nm} {min(v['harsh'] for (gg, cc, k), v in S.items() if gg == g and cc == chain and k[0] == kd):.1f}-"
                                                       f"{max(v['harsh'] for (gg, cc, k), v in S.items() if gg == g and cc == chain and k[0] == kd):.1f}" for kd, nm in kinds))
            for frac in (0.5, 0.3, 0.15):
                h = h0 * frac
                cells = []
                for kd, nm in kinds:
                    v = at_harsh(S, g, chain, kd, h, "vow"); st = at_harsh(S, g, chain, kd, h, "stops"); so = at_harsh(S, g, chain, kd, h, "soft")
                    cells.append(f"{nm}: " + ("  -  " if v is None else f"vow {v:.3f} stop {st:.2f} soft {so:+.1f}"))
                print(f"   harsh {h:4.1f}% | " + " | ".join(cells))
