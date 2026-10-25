"""Velvet 3.0 training and test voices: CMU ARCTIC slt / bdl / jmk (the release's phone labels, data.py) plus the CSTR
VCTK Corpus 0.92 (University of Edinburgh, CC BY 4.0; mic1, 48 kHz; vctk_fetch.py) phone-labelled by the Montreal
Forced Aligner with its English (US) ARPA acoustic model and dictionary v3.0.0 (CC BY 4.0). On ARCTIC, MFA's sibilant
frames agree with the release labels at F1 0.956-0.978 away from boundaries (mfa_arctic_check.py, DECISIONS V19).

TEST_VCTK: 12 VCTK voices that no 3.0 model is trained on (6 female, 6 male, 12 different accents where possible),
chosen by a fixed rule before any result was seen.
usage: import; voices() -> {voice: corpus}; utts(voice) -> [(name, load())]; load -> (fs, x, phones)"""
import os, re, wave, zlib, numpy as np
import data as A

VCTK = os.path.expanduser("~/axell-velvet-data/vctk")
ALIGNED = os.path.expanduser("~/axell-velvet-data/vctk_aligned")
ARPA = {"S": "s", "Z": "z", "SH": "sh", "ZH": "zh", "CH": "ch", "JH": "jh"}

def vctk_speakers():
    out = []
    p = f"{VCTK}/speakers.txt"
    if os.path.exists(p):
        for l in open(p):
            s = l.split()
            if len(s) >= 3:
                out.append((s[0], s[1], s[2]))
    return out

def _test_rule():
    """per gender: one voice per accent, accents in order of how rare they are in VCTK, ties by a hash of the id"""
    sp = vctk_speakers(); count = {}
    for s, g, a in sp:
        count[a] = count.get(a, 0) + 1
    pick = []
    for g in ("F", "M"):
        used = set()
        for s, gg, a in sorted(sp, key=lambda r: (count[r[2]], r[2], zlib.crc32(r[0].encode()))):
            if gg == g and a not in used and len([p for p in pick if p[1] == g]) < 6:
                pick.append((s, g, a)); used.add(a)
    return [p[0] for p in pick]

TEST_VCTK = _test_rule()

def read_textgrid(p):
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

def arpa(q):
    q = re.sub(r"\d", "", q)
    return ARPA.get(q, "pau" if q in ("", "sil", "spn", "sp") else q.lower())

def read_float_wav(p):
    import struct
    with open(p, "rb") as f:
        d = f.read()
    # RIFF float32 written by ffmpeg (pcm_f32le): find 'fmt ' and 'data'
    i = 12; fs = 48000; ch = 1; data = None
    while i < len(d) - 8:
        cid = d[i:i + 4]; n = struct.unpack("<I", d[i + 4:i + 8])[0]
        if cid == b"fmt ":
            ch = struct.unpack("<H", d[i + 10:i + 12])[0]; fs = struct.unpack("<I", d[i + 12:i + 16])[0]
        elif cid == b"data":
            data = np.frombuffer(d[i + 8:i + 8 + n], "<f4"); break
        i += 8 + n + (n & 1)
    return fs, data.reshape(-1, ch)[:, 0].astype(np.float64)

def voices():
    v = {s: "arctic" for s in ("slt", "bdl", "jmk")}
    for s, g, a in vctk_speakers():
        if os.path.isdir(f"{ALIGNED}/{s}"):
            v[s] = "vctk"
    return v

def utts(voice):
    """[(name, loader)] - loader() -> (fs, x, phones[(t0, t1, phone)])"""
    if voice in ("slt", "bdl", "jmk"):
        out = []
        for name, w, l in A.utterances(voice):
            out.append((name, (lambda w=w, l=l: (lambda r: (r[0], r[1], r[3]))(A.load(voice, w, l)))))
        return out
    out = []
    for f in sorted(os.listdir(f"{ALIGNED}/{voice}")):
        if f.endswith(".TextGrid"):
            name = f[:-9]; wavp = f"{VCTK}/{voice}/{name}.wav"
            if os.path.exists(wavp):
                def ld(wavp=wavp, g=f"{ALIGNED}/{voice}/{f}"):
                    fs, x = read_float_wav(wavp)
                    return fs, x, [(a, b, arpa(q)) for a, b, q in read_textgrid(g)]
                out.append((name, ld))
    return out

def load_utt(voice, name):
    """(fs, x, phones) of one utterance by name (picklable for worker processes)"""
    if voice in ("slt", "bdl", "jmk"):
        d = os.path.join(A.ROOT, f"cmu_us_{voice}_arctic")
        r = A.load(voice, os.path.join(d, "orig", name + ".wav"), os.path.join(d, "lab", name + ".lab"))
        return r[0], r[1], r[3]
    fs, x = read_float_wav(f"{VCTK}/{voice}/{name}.wav")
    return fs, x, [(a, b, arpa(q)) for a, b, q in read_textgrid(f"{ALIGNED}/{voice}/{name}.TextGrid")]
