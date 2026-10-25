"""The artist's own takes for the Velvet research scripts. The list is personal, so it lives outside the repository in a
local JSON manifest: $AXELL_VELVET_TAKES, default ~/axell-velvet-data/takes.json, with
  "romanian":   {name: [path, "train" | "test"]}    (romanian_data.py, eval_v2.py)
  "real_takes": [path | {"glob": pattern, "min_bytes": n}]   (deplosive_proto.py, pop_candidates.py)
Paths may start with ~."""
import glob, json, os

PATH = os.path.expanduser(os.environ.get("AXELL_VELVET_TAKES", "~/axell-velvet-data/takes.json"))

def _manifest():
    if not os.path.exists(PATH):
        raise SystemExit(f"no takes manifest at {PATH} (see research/takes.py)")
    with open(PATH, encoding="utf-8") as f:
        return json.load(f)

def romanian():
    return {k: (os.path.expanduser(p), role) for k, (p, role) in _manifest()["romanian"].items()}

def real_takes():
    out = []
    for e in _manifest()["real_takes"]:
        if isinstance(e, str):
            out.append(os.path.expanduser(e))
        else:
            out += [f for f in sorted(glob.glob(os.path.expanduser(e["glob"]))) if os.path.getsize(f) > e.get("min_bytes", 0)]
    return out
