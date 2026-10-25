"""Fetch VCTK recordings with their transcripts for the Velvet 3.0 detector (CSTR VCTK Corpus 0.92, Yamagishi, Veaux,
MacDonald, University of Edinburgh, CC BY 4.0, https://doi.org/10.7488/ds/2645) without downloading the 11.7 GB
archive: the zip's central directory is read with HTTP range requests (the same reader as Axell Gate's
vctk_fetch.py), then for every speaker with transcripts the first N utterances (mic1, 48 kHz FLAC) and their text.
Out: ~/axell-velvet-data/vctk/<speaker>/<utt>.wav (48 kHz float WAV) + <utt>.lab (the transcript, for the aligner),
speakers.txt (id, gender, accent). usage: python3 vctk_fetch.py [utterances per speaker = 60]"""
import os, sys, subprocess, threading, zipfile, collections
from concurrent.futures import ThreadPoolExecutor
import io, urllib.request
URL = "https://datashare.ed.ac.uk/server/api/core/bitstreams/535f4286-e54c-4038-838c-a02285e32cb2/content"

class RangeFile(io.RawIOBase):
    """a read-only, seekable view of a remote file through HTTP range requests (blocks cached)"""
    BLOCK = 1 << 20
    def __init__(self, url):
        self.url, self.pos, self.cache = url, 0, collections.OrderedDict()
        req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(req, timeout=120) as r:
            self.size = int(r.headers["Content-Range"].split("/")[1])
    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos
    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos
    def _block(self, k):
        if k not in self.cache:
            a = k * self.BLOCK; b = min(self.size, a + self.BLOCK) - 1
            for attempt in range(5):
                try:
                    req = urllib.request.Request(self.url, headers={"Range": f"bytes={a}-{b}"})
                    with urllib.request.urlopen(req, timeout=120) as r:
                        self.cache[k] = r.read(); break
                except Exception:
                    if attempt == 4: raise
            while len(self.cache) > 64:
                self.cache.popitem(last=False)
        return self.cache[k]
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        out = bytearray()
        while n > 0 and self.pos < self.size:
            k, o = divmod(self.pos, self.BLOCK)
            chunk = self._block(k)[o:o + n]
            out += chunk; self.pos += len(chunk); n -= len(chunk)
        return bytes(out)
    def readinto(self, b):
        d = self.read(len(b)); b[:len(d)] = d; return len(d)

OUT = os.path.expanduser("~/axell-velvet-data/vctk")

def fetch(args):
    z, flac, txt, dst = args
    if os.path.exists(dst + ".wav") and os.path.exists(dst + ".lab"):
        return
    data = z.read(flac); text = z.read(txt).decode("utf-8", "replace").strip()
    r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", "pipe:0", "-c:a", "pcm_f32le", dst + ".wav"], input=data)
    if r.returncode == 0:
        open(dst + ".lab", "w").write(text + "\n")

if __name__ == "__main__":
    nu = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    os.makedirs(OUT, exist_ok=True)
    zf = zipfile.ZipFile(RangeFile(URL))
    names = zf.namelist()
    info = zf.read("speaker-info.txt").decode("utf-8", "replace").splitlines()
    spk = {p[0]: (p[2], p[3]) for p in (l.split() for l in info[1:]) if len(p) >= 4 and p[0].startswith("p")}
    texts = {n.split("/")[-1][:-4]: n for n in names if n.startswith("txt/") and n.endswith(".txt")}
    flacs = collections.defaultdict(list)
    for n in names:
        if n.startswith("wav48_silence_trimmed/") and n.endswith("_mic1.flac"):
            u = n.split("/")[-1][:-len("_mic1.flac")]
            if u in texts:
                flacs[n.split("/")[1]].append(n)
    jobs = []
    with open(f"{OUT}/speakers.txt", "w") as f:
        for s in sorted(flacs):
            if s not in spk:
                continue
            f.write(f"{s} {spk[s][0]} {spk[s][1]}\n")
            os.makedirs(f"{OUT}/{s}", exist_ok=True)
            for n in sorted(flacs[s])[:nu]:
                u = n.split("/")[-1][:-len("_mic1.flac")]
                jobs.append((n, texts[u], f"{OUT}/{s}/{u}"))
    print(len(jobs), "utterances from", len(set(j[2].split("/")[-2] for j in jobs)), "speakers", flush=True)
    # one zip reader per thread (a reader and its block cache are not thread-safe)
    local = threading.local()
    def reader():
        if not hasattr(local, "z"):
            local.z = zipfile.ZipFile(RangeFile(URL))
        return local.z
    with ThreadPoolExecutor(8) as ex:
        for i, _ in enumerate(ex.map(lambda j: fetch((reader(),) + j), jobs)):
            if i % 500 == 0:
                print(i, flush=True)
    print("done", flush=True)
