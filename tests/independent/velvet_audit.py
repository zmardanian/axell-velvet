"""Independent black-box audit of the built Axell Velvet VST3 through a real VST3 host (VelvetAuditHost, JUCE's VST3
hosting code), with test signals and analysis written here from textbook definitions (numpy/scipy) - nothing uses the
plugin's own code. Exit code = number of failures. Environment: AXELL_AUDIT_HOST, AXELL_VST3, AXELL_AUDIT_WORK."""
import os, sys, time
import numpy as np
from scipy import signal as sg

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
os.environ.setdefault("AXELL_AUDIT_HOST", os.path.join(ROOT, "build/VelvetAuditHost_artefacts/Release/VelvetAuditHost"))
os.environ.setdefault("AXELL_VST3", os.path.join(ROOT, "build/AxellVelvet_artefacts/Release/VST3/Axell Velvet.vst3"))
os.environ.setdefault("AXELL_AUDIT_WORK", os.path.expanduser("~/.cache/axell-velvet-audit"))
_vendored = os.path.join(ROOT, "external", "axell-core")
CORE = os.environ.get("AXELL_CORE", _vendored if os.path.isdir(_vendored) else os.path.join(os.path.dirname(ROOT), "axell-core"))
sys.path.insert(0, os.path.join(CORE, "tools"))
import lib  # noqa: E402
from lib import Run  # noqa: E402

def run_all(runs, inputs, **kw):
    """the plugin receives float32: compare against exactly what it received"""
    return lib.run_all(runs, {k: np.asarray(v, np.float32) for k, v in inputs.items()}, **kw)

def f32(x):
    return np.asarray(x, np.float32).astype(np.float64)

results = []
def report(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}", flush=True)

def db(a):
    return 20 * np.log10(max(float(a), 1e-30))

CLEAN = {"De-Ess Amount": "0", "Heat On": "Off", "Low Cut": "Off"}

def voice(fs, secs, gain_db=-14.0, seed=11):
    x, v, e = _voice(fs, secs, gain_db, seed)
    return f32(x), v, e

def _voice(fs, secs, gain_db=-14.0, seed=11):
    """independent synthetic voice: glottal-like pulse train (f0 110-170 Hz) through three formant resonators, alternating
    with sibilants (white noise through a 7 kHz or 3.5 kHz two-pole resonance, 120 ms, raised-sine envelope) and pauses.
    Returns the signal and the (start, end) samples of the vowel and sibilant centres."""
    rng = np.random.default_rng(seed)
    n = int(secs * fs); t = np.arange(n) / fs
    f0 = 140 + 30 * np.sin(2 * np.pi * 0.37 * t)
    ph = np.cumsum(f0 / fs)
    pulses = (np.diff(np.floor(ph), prepend=0) > 0).astype(float)
    src = sg.lfilter([1], [1, -0.95], pulses)
    vow = np.zeros(n)
    for fc, bw, g in ((700, 120, 1.0), (1200, 150, 0.6), (2600, 250, 0.25)):
        r = np.exp(-np.pi * bw / fs); w = 2 * np.pi * fc / fs
        vow += g * sg.lfilter([1 - r], [1, -2 * r * np.cos(w), r * r], src)
    noise = rng.standard_normal(n)
    def res(fc, bw):
        r = np.exp(-np.pi * bw / fs); w = 2 * np.pi * fc / fs
        return sg.lfilter([1 - r], [1, -2 * r * np.cos(w), r * r], noise)
    s_hi, s_lo = res(7000, 1500), res(3500, 1200)
    x = np.zeros(n); vowels, esses = [], []
    seg = 0.45
    for k in range(int(secs / seg)):
        a = int(k * seg * fs)
        if k % 5 == 4:
            continue                                                    # a pause
        v0, v1 = a, a + int(0.30 * fs)
        env = np.minimum(1, np.minimum(np.arange(v1 - v0), np.arange(v1 - v0)[::-1]) / (0.02 * fs))
        x[v0:v1] += vow[v0:v1] * env
        s0, s1 = v1, v1 + int(0.12 * fs)
        if s1 > n:
            break
        e = np.sin(np.pi * np.arange(s1 - s0) / (s1 - s0))
        x[s0:s1] += (s_hi if k % 2 == 0 else s_lo)[s0:s1] * e * (6.0 if k % 2 == 0 else 4.0)
        vowels.append((v0 + int(0.06 * fs), v1 - int(0.04 * fs))); esses.append((s0 + int(0.03 * fs), s1 - int(0.03 * fs)))
    x = x / np.max(np.abs(x)) * 10 ** (gain_db / 20)
    return np.stack([x, x], 1), vowels, esses

def aligned(y, L, n):
    return y[L:L + n]

def band_energy(x, fs, lo, hi, segs):
    sos = sg.butter(4, [lo, hi], "bandpass", fs=fs, output="sos")
    z = sg.sosfiltfilt(sos, x)
    return 10 * np.log10(sum(np.sum(z[a:b] ** 2) for a, b in segs) + 1e-30)

def bh(n):
    return sg.windows.blackmanharris(n)

START = time.time()
# ---------------------------------------------------------------- 1. silence, exact delay, latency, rates
runs, ins = [], {}
for fs in (44100, 48000, 96000):
    runs.append(Run(f"sil{fs}", fs, sets={"Drive": "10", "Level": "+36", "Tone": "Bright 100"})); ins[f"sil{fs}"] = np.zeros((fs, 2))
    runs.append(Run(f"tr{fs}", fs, sets=CLEAN)); ins[f"tr{fs}"] = f32(np.random.default_rng(fs).uniform(-0.3, 0.3, (int(1.5 * fs), 2)))
    imp = np.zeros((fs, 2)); imp[fs // 2] = 0.5
    runs.append(Run(f"imp{fs}", fs, sets={"De-Ess Amount": "0", "Low Cut": "Off", "Drive": "0", "Level": "-24"})); ins[f"imp{fs}"] = imp
out = run_all(runs, ins, jobname="a1")
for fs in (44100, 48000, 96000):
    report(f"silence in, full drive and level -> exactly silence out @ {fs}", np.abs(out[f"sil{fs}"]).max() == 0.0, f"max |y| {np.abs(out[f'sil{fs}']).max():.3g}")
    r = [r for r in runs if r.name == f"tr{fs}"][0]; x = ins[r.name]; y = out[r.name]; L = r.latency
    e = np.abs(y[L:] - x[:len(x) - L]).max()
    report(f"de-ess 0 %, heat off, low cut off: output = input delayed by the reported latency ({L}) @ {fs}", e == 0.0, f"max error {e:.3g}")
    r = [r for r in runs if r.name == f"imp{fs}"][0]; y = out[r.name]
    pk = int(np.argmax(np.abs(y[:, 0])))
    report(f"impulse comes out at the reported latency (heat on, drive 0) @ {fs}", pk - fs // 2 == r.latency, f"peak at +{pk - fs // 2}, reported {r.latency}")

# ---------------------------------------------------------------- 2. HEAT: unity small-signal gain, aliasing at full drive
fs = 48000
runs, ins = [], {}
t = np.arange(int(2.0 * fs)) / fs
for m in ("Tape", "Console", "Tube"):
    runs.append(Run(f"g{m}", fs, ch=1, sets={"De-Ess Amount": "0", "Low Cut": "Off", "Heat Mode": m, "Drive": "5"}))
    ins[f"g{m}"] = (10 ** (-60 / 20) * np.sin(2 * np.pi * 1000 * t))[:, None]
    runs.append(Run(f"a{m}", fs, ch=1, sets={"De-Ess Amount": "0", "Low Cut": "Off", "Heat Mode": m, "Drive": "10", "S-Guard": "0"}))
    ins[f"a{m}"] = (0.5 * np.sin(2 * np.pi * 6997 * t))[:, None]
out = run_all(runs, ins, jobname="heat")
for m in ("Tape", "Console", "Tube"):
    r = [r for r in runs if r.name == f"g{m}"][0]; y = out[r.name][r.latency:, 0]; x = ins[r.name][:, 0]
    seg = slice(fs // 2, fs // 2 + fs)
    gi = np.abs(np.fft.rfft(x[seg] * bh(fs)))[1000]; go = np.abs(np.fft.rfft(y[seg] * bh(fs)))[1000]
    report(f"HEAT {m} drive 5: small-signal gain at 1 kHz (auto gain)", abs(db(go / gi)) < 0.3, f"{db(go / gi):+.3f} dB")
    r = [r for r in runs if r.name == f"a{m}"][0]; y = out[r.name][r.latency + fs // 2: r.latency + fs // 2 + fs, 0]
    Y = np.abs(np.fft.rfft(y * bh(fs))); f = np.fft.rfftfreq(fs, 1 / fs)
    harm = np.zeros_like(f, bool)
    for h in range(1, 10):
        harm |= np.abs(f - 6997 * h) < 40
    a = db(Y[(f > 20) & (f < 20000) & ~harm].max() / Y[np.abs(f - 6997) < 3].max())
    report(f"HEAT {m} 16x: worst alias below 20 kHz, 7 kHz at -6 dBFS, full drive", a < -95, f"{a:.1f} dBc (limit -95)")

# ---------------------------------------------------------------- 3. DE-ESS on an independent synthetic voice
for fs in (44100, 48000, 96000):
    x, vowels, esses = voice(fs, 7.0)
    xq, _, _ = voice(fs, 7.0, gain_db=-44.0)
    skip = int(1.5 * fs)
    es = [s for s in esses if s[0] > skip]; vs = [s for s in vowels if s[0] > skip]
    runs = [Run("d", fs, sets={"Heat On": "Off", "Low Cut": "Off", "De-Ess Amount": "75"}),
            Run("l", fs, sets={"Heat On": "Off", "Low Cut": "Off", "De-Ess Amount": "75", "De-Ess Listen": "On"}),
            Run("q", fs, sets={"Heat On": "Off", "Low Cut": "Off", "De-Ess Amount": "75"})]
    out = run_all(runs, {"d": x, "l": x, "q": xq}, jobname=f"de{fs}")
    n = len(x)
    y = aligned(out["d"], runs[0].latency, n)[:, 0]; rem = aligned(out["l"], runs[1].latency, n)[:, 0]
    yq = aligned(out["q"], runs[2].latency, n)[:, 0]
    red = band_energy(x[:len(y), 0], fs, 2500, 12000, es) - band_energy(y, fs, 2500, 12000, es)
    report(f"DE-ESS 75 %: sibilant 2.5-12 kHz energy reduced @ {fs}", red > 6.0, f"{red:.1f} dB (limit > 6)")
    worst = max(10 * np.log10(np.sum((y[a:b] - x[a:b, 0]) ** 2) / np.sum(x[a:b, 0] ** 2) + 1e-30) for a, b in vs)
    report(f"DE-ESS 75 %: worst change on a vowel @ {fs}", worst < -50, f"{worst:.1f} dB re the vowel (limit -50)")
    e = np.abs(y + rem - x[:len(y), 0]).max()
    report(f"DE-ESS LISTEN: what is removed + what remains = the input @ {fs}", e < 1e-6, f"max error {e:.2g}")
    redq = band_energy(xq[:len(yq), 0], fs, 2500, 12000, es) - band_energy(yq, fs, 2500, 12000, es)
    report(f"DE-ESS level independence: reduction at -44 dB vs -14 dB @ {fs}", abs(redq - red) < 0.5, f"{redq:.2f} vs {red:.2f} dB")

# ---------------------------------------------------------------- 4. S-GUARD, MIX, OUTPUT, LOW CUT, bypass, blocks, mono
fs = 48000
x, vowels, esses = voice(fs, 5.0)
es = [s for s in esses if s[0] > int(1.5 * fs)]
runs = [Run("g0", fs, sets={"De-Ess Amount": "0", "Drive": "8", "S-Guard": "0"}),
        Run("g1", fs, sets={"De-Ess Amount": "0", "Drive": "8", "S-Guard": "100"}),
        Run("lin", fs, sets={"De-Ess Amount": "0", "Heat On": "Off"}),
        Run("mix0", fs, sets={"Drive": "9", "Heat Mix": "0"}),
        Run("hoff", fs, sets={"Drive": "9", "Heat On": "Off"}),
        Run("out6", fs, sets={"Drive": "9", "Heat On": "Off", "Output": "+6.0"}),
        Run("byp", fs, sets={"Drive": "9", "Bypass": "On"}),
        Run("blk", fs, sets={"Drive": "6"}, blocks="random 1 4096 7"),
        Run("b512", fs, sets={"Drive": "6"})]
out = run_all(runs, {r.name: x for r in runs}, jobname="misc")
A = {r.name: aligned(out[r.name], r.latency, len(x))[:, 0] for r in runs}
e0 = sum(np.sum((A["g0"][a:b] - A["lin"][a:b]) ** 2) for a, b in es); e1 = sum(np.sum((A["g1"][a:b] - A["lin"][a:b]) ** 2) for a, b in es)
report("S-GUARD 100 % vs 0 %: heat added to the esses", 10 * np.log10(e0 / e1) > 10, f"{10 * np.log10(e0 / e1):.1f} dB less (limit > 10)")
report("HEAT MIX 0 % = HEAT off", np.abs(A["mix0"] - A["hoff"]).max() == 0.0, f"max diff {np.abs(A['mix0'] - A['hoff']).max():.2g}")
g = np.abs(A["out6"][fs:] / np.where(np.abs(A["hoff"][fs:]) > 1e-4, A["hoff"][fs:], np.nan))
report("OUTPUT +6.0 dB", abs(db(np.nanmedian(g)) - 6.0) < 0.01, f"{db(np.nanmedian(g)):+.3f} dB")
e = np.abs(A["byp"][fs:] - x[fs:len(A['byp']), 0]).max()
report("host bypass: input delayed by the reported latency", e < 1e-6, f"max error {e:.2g}")
e = np.abs(A["blk"] - A["b512"]).max()
report("random host block sizes 1-4096 = 512-sample blocks", e < 1e-6, f"max diff {e:.2g}")
runs, ins = [], {}
t = np.arange(int(2.5 * fs)) / fs
for f in (45.0, 90.0, 180.0):
    runs.append(Run(f"lc{int(f)}", fs, ch=1, sets=dict(CLEAN, **{"Low Cut": "90 Hz"}))); ins[f"lc{int(f)}"] = (0.1 * np.sin(2 * np.pi * f * t))[:, None]
runs.append(Run("mono", fs, ch=1, sets={"Drive": "6"})); ins["mono"] = x[:, :1]
out = run_all(runs, ins, jobname="lc")
for f, lo, hi in ((45.0, -26, -22), (90.0, -3.3, -2.7), (180.0, -0.35, 0.0)):
    r = [r for r in runs if r.name == f"lc{int(f)}"][0]; y = out[r.name][r.latency + fs: r.latency + 2 * fs, 0]
    gdb = db(np.std(y) / (0.1 / np.sqrt(2)))
    report(f"LOW CUT 90 Hz: gain at {int(f)} Hz", lo <= gdb <= hi, f"{gdb:+.2f} dB [{lo}, {hi}]")
ym = out["mono"][:, 0]
report("mono (1 in / 1 out) processes", np.isfinite(ym).all() and 0.01 < np.abs(ym).max() < 1.5, f"peak {np.abs(ym).max():.3f}")

# ---------------------------------------------------------------- 5. every factory preset on the voice: finite, level kept
names = ["Init", "De-Ess Only", "Gentle Polish", "Heat Only", "Rap Lead - Upfront", "Rap Lead - Warm Tape", "Trap Vocal - Bright & Smooth",
         "Boom Bap Grit", "Ad-Libs Thick", "Doubles & Stacks", "Hype Shouts", "R&B Silk", "Pop Vocal Air", "Soul Tube Bloom",
         "Intimate & Close", "Belting Control", "Podcast & Spoken Word", "Harsh Mic Rescue", "Lo-Fi Saturated", "Parallel Crunch"]
runs = [Run(f"p{i}", fs, sets={"Program": n}) for i, n in enumerate(names)]
out = run_all(runs, {r.name: x for r in runs}, jobname="presets")
vs = [s for s in vowels if s[0] > int(1.5 * fs)]
lev_in = 10 * np.log10(sum(np.sum(x[a:b, 0] ** 2) for a, b in vs))
bad = []
for r, n in zip(runs, names):
    y = aligned(out[r.name], r.latency, len(x))[:, 0]
    d = 10 * np.log10(sum(np.sum(y[a:b] ** 2) for a, b in vs)) - lev_in
    if not np.isfinite(y).all() or not -1.5 < d < 1.0:
        bad.append(f"{n} ({d:+.1f} dB)")
report("all 20 factory presets: finite output, vowel level within -1.5 .. +1 dB of the input (auto gain; the esses are "
       "meant to come down)", not bad, ", ".join(bad) if bad else "ok")

fails = sum(1 for _, ok, _ in results if not ok)
print(f"\n{'ALL PASS' if fails == 0 else 'FAILURES'}: {len(results) - fails} of {len(results)} independent checks passed ({time.time() - START:.0f} s)")
sys.exit(fails)
