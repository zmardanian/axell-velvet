"""How well do MFA alignments (english_us_arpa, CC BY 4.0) agree with the CMU ARCTIC labels the 1.0 detector was trained
on? Both are automatic aligners; if they agree on sibilant frames as well as ARCTIC agrees with itself across
boundaries, MFA labels can extend the training data to VCTK's voices (Velvet 3.0, DECISIONS V19).
  prep:     python3 mfa_arctic_check.py prep   -> ~/axell-velvet-data/align_arctic/corpus/<spk>/<utt>.wav/.lab
  (align:   mfa align corpus english_us_arpa english_us_arpa aligned)
  compare:  python3 mfa_arctic_check.py compare
Compare: 2 ms frames, sibilant (s z sh zh ch jh) vs not, frames within 10 ms of either label set's sibilant boundaries
ignored (as in training)."""
import os, sys, re, wave, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from data import utterances, load, SIBILANT, ROOT
W = os.path.expanduser("~/axell-velvet-data/align_arctic")
SPK = ("slt", "bdl", "jmk")
ARPA = {"S": "s", "Z": "z", "SH": "sh", "ZH": "zh", "CH": "ch", "JH": "jh"}

def texts(spk):
    t = {}
    for l in open(f"{ROOT}/cmu_us_{spk}_arctic/etc/txt.done.data"):
        m = re.match(r'\(\s*(\S+)\s+"(.*)"\s*\)', l.strip())
        if m:
            t[m.group(1)] = m.group(2)
    return t

def read_textgrid(p):
    """phone intervals [(start, end, label)] of the 'phones' tier"""
    L = open(p).read().splitlines(); out = []; tier = None; i = 0
    while i < len(L):
        s = L[i].strip()
        if s.startswith("name ="):
            tier = s.split("=")[1].strip().strip('"')
        if tier == "phones" and s.startswith("xmin =") and i + 2 < len(L) and L[i + 2].strip().startswith("text ="):
            a = float(s.split("=")[1]); b = float(L[i + 1].split("=")[1]); q = L[i + 2].split("=", 1)[1].strip().strip('"')
            out.append((a, b, q)); i += 3; continue
        i += 1
    return out

def mfa_class(q):
    q = re.sub(r"\d", "", q)
    return ARPA.get(q, "pau" if q in ("", "sil", "spn", "sp") else q.lower())

if __name__ == "__main__":
    if sys.argv[1] == "prep":
        for spk in SPK:
            os.makedirs(f"{W}/corpus/{spk}", exist_ok=True); T = texts(spk)
            for k, (name, w, l) in enumerate(utterances(spk)):
                if k % 6 or name not in T:
                    continue
                fs, x, egg, ph = load(spk, w, l)
                y = sg.resample_poly(x, 1, 2)                     # 32 -> 16 kHz (MFA's rate)
                ww = wave.open(f"{W}/corpus/{spk}/{name}.wav", "wb"); ww.setnchannels(1); ww.setsampwidth(2); ww.setframerate(16000)
                ww.writeframes((np.clip(y, -1, 1) * 32767).astype("<i2").tobytes()); ww.close()
                open(f"{W}/corpus/{spk}/{name}.lab", "w").write(T[name] + "\n")
    else:
        H = 0.002; tot = {}
        for spk in SPK:
            tp = fp = fn = 0
            for name, w, l in utterances(spk):
                g = f"{W}/aligned/{spk}/{name}.TextGrid"
                if not os.path.exists(g):
                    continue
                fs, x, egg, ph = load(spk, w, l); n = int(len(x) / fs / H)
                t = (np.arange(n) + 0.5) * H
                a = np.zeros(n, bool); b = np.zeros(n, bool); near = np.zeros(n, bool)
                for s0, s1, q in ph:
                    if q in SIBILANT:
                        a |= (t >= s0) & (t < s1); near |= (np.abs(t - s0) < 0.01) | (np.abs(t - s1) < 0.01)
                for s0, s1, q in read_textgrid(g):
                    if mfa_class(q) in SIBILANT:
                        b |= (t >= s0) & (t < s1); near |= (np.abs(t - s0) < 0.01) | (np.abs(t - s1) < 0.01)
                k = ~near; tp += (a & b & k).sum(); fp += (~a & b & k).sum(); fn += (a & ~b & k).sum()
            P, R = tp / max(tp + fp, 1), tp / max(tp + fn, 1)
            print(f"{spk}: MFA sibilant frames vs ARCTIC labels (away from boundaries): precision {P:.3f} recall {R:.3f} F1 {2*P*R/(P+R):.3f}")
