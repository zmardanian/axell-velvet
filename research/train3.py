"""Velvet 3.0 detector training: the 1.0 recipe (train_cpp.py: the shipping C++ features, a 19 -> H tanh -> 1 network,
44.1 / 48 / 96 kHz, half of the utterances through a random recording chain) on many more voices - CMU ARCTIC
slt / bdl / jmk plus the VCTK voices (corpus3.py: MFA phone labels), with corpus3.TEST_VCTK never trained on.

Two detectors, each trained on its own features:
  studio  the 1.0 detector's features (centred frames, +-2 hop smoothing) - the label at the frame centre
  live    the LIVE detector's (DetectorConfig::live(): causal, past-only smoothing) - the label LEAD ms after the
          frame centre, because LIVE applies a frame's gains to the newest sample, win/2 after its centre
steps:
  python3 train3.py features <VelvetFeatures>        both feature sets for every utterance (~/axell-velvet-data/feat3)
  python3 train3.py train studio|live [--lead ms] [--hidden H] [--write]
     held-out evaluation (each ARCTIC voice by a model trained without it; TEST_VCTK by a model trained on all other
     voices), the held-out headers for the benchmark (~/axell-velvet-data/models3/<kind>_no_<voice>.h, <kind>_vctk.h),
     and with --write the final network on every voice -> Source/DetectorModel.h / DetectorModelLive.h"""
import os, sys, zlib, subprocess, numpy as np
from concurrent.futures import ProcessPoolExecutor
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus3 as C3
import train_cpp as T
from data import SIBILANT, STOPS, FRICATIVE_OTHER

F3 = os.path.expanduser("~/axell-velvet-data/feat3")
M3 = os.path.expanduser("~/axell-velvet-data/models3")
RATES = (44100, 48000, 96000)
arg = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d

def resample(x, fs0, fs):
    from math import gcd
    g = gcd(int(fs0), int(fs)); return sg.resample_poly(x, fs // g, fs0 // g)

def prep(job):
    voice, k, name, tmp = job
    rng = np.random.default_rng(zlib.crc32(f"{voice}/{name}".encode()))
    fs0, x, ph = C3.load_utt(voice, name)
    fs = RATES[k % 3]
    x = resample(x, fs0, fs)
    aug = k % 2 == 1
    x = T.chain(x, fs, rng) if aug else x * 10 ** (rng.uniform(-20, 0) / 20) / (np.max(np.abs(x)) + 1e-9) * 0.5
    p = f"{tmp}/{voice}_{name}.f32"
    x.astype(np.float32).tofile(p)
    return p, fs, (voice, name, aug, fs, ph)

def features(tool):
    os.makedirs(F3 + "/studio", exist_ok=True); os.makedirs(F3 + "/live", exist_ok=True)
    tmp = os.path.expanduser("~/axell-velvet-data/feat3_tmp"); os.makedirs(tmp, exist_ok=True)
    meta = []
    vs = C3.voices()
    for vi, voice in enumerate(sorted(vs)):
        U = C3.utts(voice)
        jobs = [(voice, k, n, tmp) for k, (n, ld) in enumerate(U)]
        with ProcessPoolExecutor(8) as ex:
            res = list(ex.map(prep, jobs, chunksize=8))
        for kind, flag in (("studio", []), ("live", ["--live"])):
            mans = []
            for i in range(8):
                part = res[i::8]
                if not part:
                    continue
                m = f"{tmp}/man_{kind}_{i}.txt"
                open(m, "w").writelines(f"{p} {fs} {F3}/{kind}/{os.path.basename(p)[:-4]}.feat\n" for p, fs, _ in part)
                mans.append(m)
            procs = [subprocess.Popen([tool, m] + flag, stdout=subprocess.DEVNULL) for m in mans]
            for pr in procs:
                assert pr.wait() == 0
        for p, fs, mt in res:
            os.remove(p); meta.append(mt)
        print(f"{vi + 1}/{len(vs)} {voice}: {len(U)} utterances", flush=True)
    np.save(f"{F3}/meta.npy", np.array(meta, dtype=object), allow_pickle=True)

def label_at(t, ph):
    """class at each time t, and 'near' = within 10 ms of a sibilant boundary (ignored, as in 1.0)"""
    return T.label(t, ph)

def load(kind, lead_ms):
    meta = np.load(f"{F3}/meta.npy", allow_pickle=True)
    X, E, Cl, N, V, A = [], [], [], [], [], []
    for voice, name, aug, fs, ph in meta:
        f = f"{F3}/{kind}/{voice}_{name}.feat"
        if not os.path.exists(f):
            continue
        d = np.fromfile(f, np.float32).reshape(-1, T.NC)
        cls, near = label_at(d[:, 0].astype(np.float64) + lead_ms * 1e-3, ph)
        X.append(d[:, T.COL["x"]]); E.append(d[:, T.COL["E"]].sum(1)); Cl.append(cls); N.append(near)
        V.append(np.full(len(d), voice, object)); A.append(np.full(len(d), aug))
    return (np.concatenate(X), np.concatenate(E), np.concatenate(Cl), np.concatenate(N), np.concatenate(V), np.concatenate(A))

def report(tag, p, cls, near, e):
    keep = ~near & (cls != ""); sib = np.isin(cls, list(SIBILANT))
    pred = p > 0.5
    tp = (pred & sib & keep).sum(); fp = (pred & ~sib & keep).sum(); fn = (~pred & sib & keep).sum()
    P = tp / max(tp + fp, 1); R = tp / max(tp + fn, 1); F1 = 2 * P * R / max(P + R, 1e-9)
    er = (e * (pred & sib & keep)).sum() / max((e * (sib & keep)).sum(), 1e-30)
    vow = keep & ~sib & ~np.isin(cls, list(STOPS | FRICATIVE_OTHER | {"pau"}))
    per = {q: (e[keep & (cls == q)] * pred[keep & (cls == q)]).sum() / max(e[keep & (cls == q)].sum(), 1e-30) for q in ("s", "z", "sh", "zh", "ch", "jh")}
    print(f"  {tag:28s} F1 {F1:.3f} | sibilant energy caught {100 * er:5.1f}% (" + " ".join(f"{q} {100 * v:.0f}" for q, v in per.items())
          + f") | false alarms: vowels {100 * pred[vow].mean():.2f}% stops {100 * pred[keep & np.isin(cls, list(STOPS))].mean():.2f}%"
          + f" f/th/v {100 * pred[keep & np.isin(cls, list(FRICATIVE_OTHER))].mean():.2f}%", flush=True)
    return dict(F1=F1, er=er, vow=pred[vow].mean())

if __name__ == "__main__":
    if sys.argv[1] == "features":
        features(sys.argv[2]); sys.exit()
    kind = sys.argv[2]; lead = arg("--lead", 0.0); H = arg("--hidden", 32); os.makedirs(M3, exist_ok=True)
    X, E, Cl, N, V, A = load(kind, lead)
    y = np.isin(Cl, list(SIBILANT)).astype(float); use = ~N & (Cl != "")
    test = np.isin(V, C3.TEST_VCTK)
    print(f"{kind} (lead {lead} ms, hidden {H}): {len(X)} frames, {len(set(V))} voices, test voices {len(C3.TEST_VCTK)}", flush=True)
    tag = f"{kind}" + (f"_lead{lead:g}" if lead else "") + (f"_h{H}" if H != 32 else "")
    # held-out VCTK voices: one model on every other voice
    tr = np.where(use & ~test)[0][::3]
    m = T.mlp_fit(X[tr], y[tr], H=H); T.write_header(f"{M3}/{tag}_vctk.h", m, len(tr), "held out: TEST_VCTK")
    for aug in (False, True):
        sel = test & (A == aug)
        report(f"TEST_VCTK {'chain' if aug else 'clean'}", T.mlp_prob(X[sel], m), Cl[sel], N[sel], E[sel])
    # held-out ARCTIC voices (also without TEST_VCTK), comparable with 1.0's leave-one-out numbers (--quick: skipped)
    for te in (() if "--quick" in sys.argv else ("slt", "bdl", "jmk")):
        tr = np.where(use & ~test & (V != te))[0][::3]
        m = T.mlp_fit(X[tr], y[tr], H=H); T.write_header(f"{M3}/{tag}_no_{te}.h", m, len(tr), f"held out: {te} and TEST_VCTK")
        for aug in (False, True):
            sel = (V == te) & (A == aug)
            report(f"{te} {'chain' if aug else 'clean'}", T.mlp_prob(X[sel], m), Cl[sel], N[sel], E[sel])
    if "--write" in sys.argv:
        tr = np.where(use)[0][::2]
        m = T.mlp_fit(X[tr], y[tr], H=H, epochs=14)
        hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source", "DetectorModel.h" if kind == "studio" else "DetectorModelLive.h")
        T.write_header(hdr, m, len(tr), f"Velvet 3.0 {kind}: {len(set(V))} voices (CMU ARCTIC + VCTK), lead {lead:g} ms")
        s = open(hdr).read()
        a, b = s.index("// Axell Velvet"), s.index("// p = sigmoid")
        s = s[:a] + (f"// Axell Velvet 3.0 - {kind.upper()} sibilance network weights. GENERATED by research/train3.py from features computed by\n"
                     f"// the shipping C++ code (tools/VelvetFeatures.cpp{' --live' if kind == 'live' else ''}) on {len(set(V))} voices - CMU ARCTIC slt / bdl / jmk and the\n"
                     f"// CSTR VCTK Corpus 0.92 (CC BY 4.0, phone labels by the Montreal Forced Aligner) - at 44.1 / 48 / 96 kHz, half of\n"
                     f"// them through random recording chains ({len(tr)} labelled frames). Do not edit by hand.\n") + s[b:]
        if kind == "live":
            s = s.replace("namespace axv::model\n", "namespace axv::model_live\n").replace("} // namespace axv::model\n", "} // namespace axv::model_live\n")
        open(hdr, "w").write(s)
        print("wrote", os.path.abspath(hdr))
