"""Train the sibilance model on the features of the SHIPPING C++ detector (tools/VelvetFeatures.cpp).

CMU ARCTIC slt / bdl / jmk (32 kHz originals, phone labels aligned per file) are resampled to 44.1 / 48 / 96 kHz and,
for half of the utterances, passed through a random recording chain - a microphone presence peak (+0..6 dB at
3-12 kHz), a high-shelf tilt, a fast FET-style compressor (3-12 dB of gain reduction), a level ride, a level offset and
a noise floor - so the model learns sibilance, not one microphone or one level. Leave-one-speaker-out evaluation
(clean and chain-processed test sets), then the final model on all three voices -> Source/DetectorModel.h.

usage: python3 train_cpp.py <VelvetFeatures binary> [--eval-only]"""
import os, sys, subprocess, zlib, numpy as np
from concurrent.futures import ProcessPoolExecutor
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(__file__))
from data import utterances, load, SIBILANT, STOPS, FRICATIVE_OTHER

TOOL = sys.argv[1] if __name__ == "__main__" else None
OUT = os.path.expanduser("~/axell-velvet-data/cppfeat")
SPK = ("slt", "bdl", "jmk")
RATES = (44100, 48000, 96000)
NC = 55  # columns written by VelvetFeatures
COL = {"t": 0, "feat": slice(1, 11), "adapted": slice(11, 21), "x": slice(21, 40), "E": slice(40, 50),
       "body": 50, "level": 51, "f0": 52, "ap": 53, "p": 54}

def peaking(fs, f0, q, db):
    A = 10 ** (db / 40); w = 2 * np.pi * f0 / fs; al = np.sin(w) / (2 * q); c = np.cos(w)
    b = np.array([1 + al * A, -2 * c, 1 - al * A]); a = np.array([1 + al / A, -2 * c, 1 - al / A]); return b / a[0], a / a[0]

def shelf(fs, f0, db):
    A = 10 ** (db / 40); w = 2 * np.pi * f0 / fs; c = np.cos(w); al = np.sin(w) / 2 * np.sqrt(2); s = 2 * np.sqrt(A) * al
    b = np.array([A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s)])
    a = np.array([(A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s]); return b / a[0], a / a[0]

def chain(x, fs, rng):
    """a random vocal recording chain (mic colour, compressor, level ride, level, noise)"""
    b, a = peaking(fs, rng.uniform(3000, 12000), rng.uniform(0.7, 2.0), rng.uniform(0, 6)); x = sg.lfilter(b, a, x)
    b, a = shelf(fs, 5000, rng.uniform(-4, 4)); x = sg.lfilter(b, a, x)
    if rng.random() < 0.7:  # fast compressor: peak envelope (attack ~0.5 ms) -> static curve -> gain release ~80 ms
        ka = np.exp(-1 / (rng.uniform(0.0002, 0.001) * fs))
        env = sg.lfilter([1 - ka], [1, -ka], np.abs(x))
        ldb = 20 * np.log10(env + 1e-9); thr = np.percentile(ldb[ldb > -80], 97) - rng.uniform(3, 12); ratio = rng.uniform(4, 12)
        gr = np.minimum(0, (thr - ldb) * (1 - 1 / ratio))
        kr = np.exp(-1 / (rng.uniform(0.04, 0.15) * fs))
        g = 10 ** (sg.lfilter([1 - kr], [1, -kr], gr) / 20); x = x * g
    t = np.arange(len(x)) / fs
    x = x * 10 ** ((rng.uniform(0, 8) * np.sin(2 * np.pi * t / rng.uniform(2, 6) + rng.uniform(0, 6.3))) / 20)
    x = x / (np.max(np.abs(x)) + 1e-9) * 10 ** (rng.uniform(-30, -1) / 20)
    x = x + rng.standard_normal(len(x)) * np.max(np.abs(x)) * 10 ** (rng.uniform(-75, -55) / 20)
    return x

def prepare(job):
    spk, k, name, w, l = job
    rng = np.random.default_rng(zlib.crc32(f"{spk}/{name}".encode()))
    fs0, x, egg, ph = load(spk, w, l)
    fs = RATES[k % 3]
    x = sg.resample_poly(x, {44100: 441, 48000: 3, 96000: 3}[fs], {44100: 320, 48000: 2, 96000: 1}[fs])
    aug = k % 2 == 1
    if aug:
        x = chain(x, fs, rng)
    else:
        x = x * 10 ** (rng.uniform(-20, 0) / 20)
    p = f"{OUT}/{spk}_{name}.f32"
    x.astype(np.float32).tofile(p)
    return f"{p} {fs} {OUT}/{spk}_{name}.feat\n", (spk, name, aug, fs, ph)

def label(t, ph):
    cls = np.full(len(t), "", object); near = np.zeros(len(t), bool)
    for a, b, q in ph:
        cls[(t >= a) & (t < b)] = q
        if q in SIBILANT:
            near |= (np.abs(t - a) < 0.010) | (np.abs(t - b) < 0.010)
    return cls, near

def fit(X, y, l2=1e-2):
    mu, sd = X.mean(0), X.std(0) + 1e-9
    Z = np.column_stack([(X - mu) / sd, np.ones(len(X))]); w = np.zeros(Z.shape[1])
    for it in range(40):
        p = 1 / (1 + np.exp(-np.clip(Z @ w, -30, 30))); W = p * (1 - p) + 1e-6
        step = np.linalg.solve(Z.T @ (Z * W[:, None]) + l2 * np.eye(len(w)), Z.T @ (y - p) - l2 * w)
        w += step
        if np.max(np.abs(step)) < 1e-7:
            break
    return w, mu, sd

def prob(X, m):
    w, mu, sd = m; return 1 / (1 + np.exp(-np.clip(((X - mu) / sd) @ w[:-1] + w[-1], -30, 30)))

def scores(pred, cls, near):
    sib = np.isin(cls, list(SIBILANT)); keep = ~near & (cls != "")
    tp = (pred & sib & keep).sum(); fp = (pred & ~sib & keep).sum(); fn = (~pred & sib & keep).sum()
    P = tp / max(tp + fp, 1); R = tp / max(tp + fn, 1)
    vow = keep & ~sib & ~np.isin(cls, list(STOPS | FRICATIVE_OTHER | {"pau"}))
    fa = {"vowels/voiced": pred[vow].mean(), "stops": pred[keep & np.isin(cls, list(STOPS))].mean(),
          "f/th/v/hh": pred[keep & np.isin(cls, list(FRICATIVE_OTHER))].mean(), "pause": pred[keep & (cls == "pau")].mean()}
    return P, R, 2 * P * R / max(P + R, 1e-9), fa

def energy_recall(pred, cls, near, D):
    sib = np.isin(cls, list(SIBILANT)) & ~near; e = D[:, COL["E"]].sum(1)
    return (e * (pred & sib)).sum() / max((e * sib).sum(), 1e-30)


def mlp_fit(X, y, H=32, epochs=12, seed=0, l2=1e-4):
    """19 -> H tanh -> 1 logistic network, Adam, minibatches of 4096, learning rate decay 0.75 per epoch"""
    rng = np.random.default_rng(seed); mu, sd = X.mean(0), X.std(0) + 1e-9; Z = (X - mu) / sd
    P = [rng.standard_normal((Z.shape[1], H)) / np.sqrt(Z.shape[1]), np.zeros(H), rng.standard_normal(H) / np.sqrt(H), np.zeros(1)]
    M = [np.zeros_like(q) for q in P]; V = [np.zeros_like(q) for q in P]; t = 0; lr = 3e-3
    for ep in range(epochs):
        idx = rng.permutation(len(Z))
        for s0 in range(0, len(Z), 4096):
            b = idx[s0:s0 + 4096]; z = Z[b]; h = np.tanh(z @ P[0] + P[1]); o = h @ P[2] + P[3][0]
            p = 1 / (1 + np.exp(-np.clip(o, -30, 30))); g = (p - y[b]) / len(b); gh = np.outer(g, P[2]) * (1 - h * h)
            G = [z.T @ gh + l2 * P[0], gh.sum(0), h.T @ g + l2 * P[2], np.array([g.sum()])]; t += 1
            for i in range(4):
                M[i] = 0.9 * M[i] + 0.1 * G[i]; V[i] = 0.999 * V[i] + 0.001 * G[i] ** 2
                P[i] -= lr * (M[i] / (1 - 0.9 ** t)) / (np.sqrt(V[i] / (1 - 0.999 ** t)) + 1e-8)
        lr *= 0.75
    return P, mu, sd

def mlp_prob(X, m):
    P, mu, sd = m; h = np.tanh(((X - mu) / sd) @ P[0] + P[1]); return 1 / (1 + np.exp(-np.clip(h @ P[2] + P[3][0], -30, 30)))

def soft(p, C, N, D):
    """share of each class's 2-17 kHz energy the de-esser treats (weight p^2, as in the plugin)"""
    e = D[:, COL["E"]].sum(1); w = p ** 2; out = {}
    for name, m in [("sibilants", np.isin(C, list(SIBILANT))), ("vowels/voiced", ~np.isin(C, list(SIBILANT | STOPS | FRICATIVE_OTHER | {"pau", ""}))),
                    ("stops", np.isin(C, list(STOPS))), ("f/th/v/hh", np.isin(C, list(FRICATIVE_OTHER)))]:
        mm = m & ~N; out[name] = (e[mm] * w[mm]).sum() / max(e[mm].sum(), 1e-30)
    return out

def write_header(hdr, m, nframes, what):
    P, mu, sd = m
    X1 = P[0].shape[0]
    f = lambda a: ", ".join(f"{v:.7g}f" for v in np.ravel(a))
    open(hdr, "w").write(f"""#pragma once
// Axell Velvet - sibilance network weights. GENERATED by research/train_cpp.py from features computed by the shipping
// C++ code (tools/VelvetFeatures.cpp) on CMU ARCTIC slt / bdl / jmk at 44.1 / 48 / 96 kHz, half of them through
// random recording chains ({nframes} labelled frames; {what}). Do not edit by hand.
// p = sigmoid(kB2 + sum_j kW2[j] tanh(kB1[j] + sum_i z[i] kW1[i * kHidden + j])), z = (x - kMu) / kSd
namespace axv::model
{{
constexpr int kInputs = {X1}, kHidden = {len(P[1])};
constexpr float kMu[kInputs] = {{ {f(mu)} }};
constexpr float kSd[kInputs] = {{ {f(sd)} }};
constexpr float kW1[kInputs * kHidden] = {{ {f(P[0])} }};
constexpr float kB1[kHidden] = {{ {f(P[1])} }};
constexpr float kW2[kHidden] = {{ {f(P[2])} }};
constexpr float kB2 = {P[3][0]:.7g}f;
}} // namespace axv::model
""")

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    jobs = [(s, k, n, w, l) for s in SPK for k, (n, w, l) in enumerate(utterances(s))]
    meta = {}
    if "--eval-only" not in sys.argv:
        with ProcessPoolExecutor() as ex:
            res = list(ex.map(prepare, jobs, chunksize=16))
        lines = [r[0] for r in res]
        mans = []
        for i in range(16):
            mp = f"{OUT}/manifest{i}.txt"; open(mp, "w").writelines(lines[i::16]); mans.append(mp)
        procs = [subprocess.Popen([TOOL, m], stdout=subprocess.DEVNULL) for m in mans]
        assert all(p.wait() == 0 for p in procs)
        for r in res:
            os.remove(r[0].split()[0])
        np.save(f"{OUT}/meta.npy", np.array([r[1] for r in res], dtype=object), allow_pickle=True)
    meta = np.load(f"{OUT}/meta.npy", allow_pickle=True)
    D, C, N, S, A = [], [], [], [], []
    for spk, name, aug, fs, ph in meta:
        d = np.fromfile(f"{OUT}/{spk}_{name}.feat", np.float32).reshape(-1, NC).astype(np.float64)
        cls, near = label(d[:, 0], ph)
        D.append(d); C.append(cls); N.append(near); S.append(np.full(len(d), spk)); A.append(np.full(len(d), aug))
    D = np.concatenate(D); C = np.concatenate(C); N = np.concatenate(N); S = np.concatenate(S); A = np.concatenate(A)
    X = D[:, COL["x"]]
    y = np.isin(C, list(SIBILANT)).astype(float)
    use = ~N & (C != "")
    print(f"{len(D)} frames, {int(y[use].sum())} sibilant")
    os.makedirs(os.path.expanduser("~/axell-velvet-data/models"), exist_ok=True)
    for te in SPK:
        tr = np.where((S != te) & use)[0][::3]
        m = mlp_fit(X[tr], y[tr])
        write_header(os.path.expanduser(f"~/axell-velvet-data/models/no_{te}.h"), m, len(tr), f"held-out fold: trained without {te}")
        for aug in (False, True):
            sel = (S == te) & (A == aug)
            p = mlp_prob(X[sel], m)
            P, R, F1, fa = scores(p > 0.5, C[sel], N[sel])
            sm = soft(p, C[sel], N[sel], D[sel])
            print(f"TEST {te} {'chain' if aug else 'clean'}: F1 {F1:.3f} | HF energy treated: " + ", ".join(f"{k} {100 * v:.1f}%" for k, v in sm.items())
                  + " | false alarms (p > 0.5) " + ", ".join(f"{k} {100 * v:.1f}%" for k, v in fa.items()))
    tr = np.where(use)[0][::2]
    m = mlp_fit(X[tr], y[tr], epochs=14)
    hdr = os.path.join(os.path.dirname(__file__), "..", "Source", "DetectorModel.h")
    write_header(hdr, m, len(tr), "all three voices")
    print("wrote", os.path.abspath(hdr))

