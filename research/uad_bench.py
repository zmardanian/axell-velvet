"""HEAT benchmark against certified UAD plugins (the user's own licences, run through the same independent VST3 host as
the audit): UADx Oxide Tape, 610-A, 610-B, Century Tube Channel Strip vs Axell Velvet TAPE / CONSOLE / TUBE.

Every plugin at its default settings (Velvet: de-esser 0 %, low cut off, LEVEL 0, the model at DRIVE 5); the INPUT
level is swept, so each device is measured over its own saturation range:
  character   THD and H2..H5 at 1 kHz vs input level (-42 .. 0 dBFS)
  aliasing    at the input level where each device reaches 3 % THD at 1 kHz (equal saturation depth), a 7 kHz tone:
              worst in-band non-harmonic component re the fundamental (4-term Blackman-Harris, harmonics +-40 Hz out)
  colour      low-level (-40 dBFS) frequency response 30 Hz .. 18 kHz re 1 kHz (pink-ish multitone)
  noise       output with silence in (dBFS RMS, A-weighting not applied)
usage: python3 uad_bench.py <AuditHost> <Axell Velvet.vst3> <out dir>"""
import os, sys, subprocess, numpy as np

HOST, VELVET, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
FS = 48000
os.makedirs(OUT, exist_ok=True)
Y = os.path.expanduser("~/.vst3/yabridge")
# the Wine environment the DAW uses for the UAD plugins (a mismatched system Wine fails with "version mismatch")
_h = os.path.expanduser
WINE = _h(os.environ.get("UAD_WINE_HOME", "~/opt/wine-uad-dcomp11"))
ENV = dict(os.environ, WINEPREFIX=_h(os.environ.get("UAD_WINE_PREFIX", "~/.wine-uaconnect")), WINEARCH="win64",
           WINELOADER=WINE + "/bin/wine", WINESERVER=WINE + "/bin/wineserver",
           PATH=_h(os.environ.get("YABRIDGE_HOME", "~/.local/share/yabridge-wine10-embedding")) + ":" + os.environ["PATH"])
DEVICES = [
    ("UADx Oxide Tape (15 IPS, NAB)", f"{Y}/uaudio_oxide_tape.vst3", ["set Noise Reduct=Off"]),
    ("UADx 610-A (defaults)", f"{Y}/uaudio_610_a.vst3", []),
    ("UADx 610-B (defaults)", f"{Y}/uaudio_610_b.vst3", []),
    ("UADx Century Tube Strip (defaults)", f"{Y}/uaudio_century_channel_strip.vst3", []),
    ("Axell Velvet TAPE, drive 5", VELVET, ["set De-Ess Amount=0", "set Low Cut=Off", "set Heat Mode=Tape", "set Drive=5"]),
    ("Axell Velvet CONSOLE, drive 5", VELVET, ["set De-Ess Amount=0", "set Low Cut=Off", "set Heat Mode=Console", "set Drive=5"]),
    ("Axell Velvet TUBE, drive 5", VELVET, ["set De-Ess Amount=0", "set Low Cut=Off", "set Heat Mode=Tube", "set Drive=5"]),
]

def render(dev, x, tag):
    name, path, sets = dev
    inp, out = f"{OUT}/in_{tag}.f32", f"{OUT}/out_{tag}.f32"
    np.repeat(x[:, None], 2, 1).astype(np.float32).tofile(inp)
    job = f"{OUT}/job_{tag}.txt"
    open(job, "w").write("\n".join(["rate 48000", "channels 2", "blocks 512", "nonrealtime 1", "preroll 48000"] + sets
                                    + [f"input {inp}", f"output {out}", "run"]) + "\n")
    for attempt in range(3):  # the Wine bridge occasionally drops a run under load: retry
        try:
            r = subprocess.run([HOST, path, job], env=ENV, capture_output=True, text=True, timeout=900)
        except subprocess.TimeoutExpired:
            continue
        if os.path.exists(out) and os.path.getsize(out) >= len(x) * 8:
            break
    else:
        print(r.stdout[-2000:], r.stderr[-2000:]); raise SystemExit(f"render failed: {name}")
    y = np.fromfile(out, np.float32).reshape(-1, 2)[:, 0].astype(np.float64)
    os.remove(inp); os.remove(out); os.remove(job)
    return y

def bh(n):
    t = 2 * np.pi * np.arange(n) / n
    return 0.35875 - 0.48829 * np.cos(t) + 0.14128 * np.cos(2 * t) - 0.01168 * np.cos(3 * t)

def latency(dev):
    x = np.zeros(FS); x[FS // 2] = 0.1
    y = render(dev, x, "lat")
    return int(np.argmax(np.abs(y))) - FS // 2

def harmonics(y, f0, lat, secs=1.0):
    seg = y[lat + int(0.5 * FS): lat + int(0.5 * FS) + int(secs * FS)]
    w = bh(len(seg)); Y = np.abs(np.fft.rfft(seg * w)); fr = np.fft.rfftfreq(len(seg), 1 / FS)
    amp = lambda f: Y[np.abs(fr - f) < 3].max()
    h = [amp(k * f0) for k in range(1, 10) if k * f0 < 0.45 * FS]
    return h, Y, fr

def tone(f, db, secs=2.0):
    t = np.arange(int(secs * FS)) / FS
    return 10 ** (db / 20) * np.sin(2 * np.pi * f * t)

if __name__ == "__main__":
    levels = np.arange(-42, 1, 3.0)
    report = []
    import json
    cache = f"{OUT}/cache.json"
    done = json.load(open(cache)) if os.path.exists(cache) else {}
    for dev in DEVICES:
        name = dev[0]
        if name in done:
            report.append(done[name]); r = done[name]
            print(f"\n{name}: latency {r['latency']} smp | 3 % THD at {r['op']:+.1f} dBFS in | alias at that depth (7 kHz) {r['alias']:.1f} dBc | "
                  f"response 30 Hz..18 kHz {min(r['resp']):+.2f} .. {max(r['resp']):+.2f} dB re 1 kHz | noise (silence in) {r['noise']:.1f} dBFS")
            continue
        lat = latency(dev)
        thd, comp, gains = [], [], []
        for L in levels:
            y = render(dev, tone(1000.0, L), f"t{int(L)}")
            h, _, _ = harmonics(y, 1000.0, lat)
            thd.append(float(20 * np.log10(np.sqrt(np.sum(np.array(h[1:]) ** 2)) / h[0])))
            comp.append([float(20 * np.log10(v / h[0] + 1e-12)) for v in h[1:5]])
            gains.append(float(20 * np.log10(h[0] / (np.sum(bh(FS)) / 2) / 10 ** (L / 20))))
        thd = np.array(thd)
        # input level for -30 dB THD (3.2 %): interpolate on the rising part
        idx = np.where(thd >= -30.0)[0]
        if len(idx) and idx[0] > 0:
            i = idx[0]; Lop = levels[i - 1] + (levels[i] - levels[i - 1]) * (-30.0 - thd[i - 1]) / (thd[i] - thd[i - 1])
        else:
            Lop = levels[-1] if not len(idx) else levels[0]
        # aliasing at that level, 7 kHz
        f0 = 6997.0
        y = render(dev, tone(f0, Lop, 2.5), "alias")
        seg = y[lat + FS: lat + 2 * FS]
        Ys = np.abs(np.fft.rfft(seg * bh(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / FS)
        harm = np.zeros(len(fr), bool)
        for k in range(1, 20):
            harm |= np.abs(fr - k * f0) < 40
        band = (fr > 20) & (fr < 20000) & ~harm
        alias = 20 * np.log10(Ys[band].max() / Ys[np.abs(fr - f0) < 3].max())
        # colour: multitone at -40 dBFS total
        rng = np.random.default_rng(1); fl = np.geomspace(30, 18000, 40); t = np.arange(3 * FS) / FS
        mt = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) / np.sqrt(f / 1000) for f in fl)
        mt = mt / np.max(np.abs(mt)) * 10 ** (-40 / 20)
        y = render(dev, mt, "mt")
        seg_in = mt[FS: 2 * FS]; seg_out = y[lat + FS: lat + 2 * FS]
        Xi = np.fft.rfft(seg_in * bh(FS)); Xo = np.fft.rfft(seg_out * bh(FS)); fr = np.fft.rfftfreq(FS, 1 / FS)
        resp = [20 * np.log10(np.abs(Xo[np.argmin(np.abs(fr - f))]) / np.abs(Xi[np.argmin(np.abs(fr - f))])) for f in fl]
        ref = np.interp(1000, fl, resp); resp = np.array(resp) - ref
        # noise with silence in
        y = render(dev, np.zeros(3 * FS), "sil")
        noise = 20 * np.log10(np.sqrt(np.mean(y[FS:] ** 2)) + 1e-12)
        r = dict(name=name, latency=int(lat), levels=levels.tolist(), thd=thd.tolist(), comp=comp, gain=gains, op=float(Lop), alias=float(alias),
                 resp=resp.tolist(), fl=fl.tolist(), noise=float(noise))
        report.append(r)
        done[name] = r
        json.dump(done, open(cache, "w"))
        print(f"\n{name}: latency {lat} smp | 3 % THD at {Lop:+.1f} dBFS in | alias at that depth (7 kHz) {alias:.1f} dBc | "
              f"response 30 Hz..18 kHz {resp.min():+.2f} .. {resp.max():+.2f} dB re 1 kHz | noise (silence in) {noise:.1f} dBFS")
        print("   THD (dB) vs input:", " ".join(f"{L:+.0f}:{v:.0f}" for L, v in zip(levels[::2], thd[::2])))
        i = int(np.argmin(np.abs(levels - Lop)))
        print("   at ~3 %% THD: H2 %.1f  H3 %.1f  H4 %.1f  H5 %.1f dBc" % tuple(comp[i]))
    np.save(f"{OUT}/uad_bench.npy", np.array(report, dtype=object), allow_pickle=True)
