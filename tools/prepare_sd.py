#!/usr/bin/env python3
"""Extract only the files needed by the Switch wrapper from a legally obtained APK."""
from pathlib import Path
import argparse, hashlib, shutil, zipfile, sys

EXPECTED = {
 "lib/arm64-v8a/libmain.so": "f172319d1045e9baa23052e47c78a560d6cf0b85e92ab1201f71f768c2f3a051",
 "lib/arm64-v8a/libunity.so": "d86adb00b1cd1794bb564f9c04bc24627e4cac0b1d91e6be961c6d852bb81fe2",
 "lib/arm64-v8a/libil2cpp.so": "687dcbef0bfad0fb3fec545f2a5106e53357c1a477605328c4489096ecab4572",
}

def sha(b): return hashlib.sha256(b).hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("sd_root", help="mounted SD root or an empty staging directory")
    ap.add_argument("--nro", help="optional compiled stupidzombies_nx.nro")
    args=ap.parse_args()
    apk=Path(args.apk); out=Path(args.sd_root)/"switch"/"stupidzombies"
    out.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        names=set(z.namelist())
        for src,expected in EXPECTED.items():
            if src not in names: raise SystemExit(f"missing {src}")
            b=z.read(src); got=sha(b)
            if got!=expected:
                raise SystemExit(f"{src}: SHA mismatch. This kit targets exact Stupid Zombies 3.4.5.\nexpected {expected}\n     got {got}")
            (out/Path(src).name).write_bytes(b)
        # Preserve the APK's assets path exactly: Unity expects assets/bin/Data/...
        for n in sorted(names):
            if n.startswith("assets/") and not n.endswith("/"):
                dst=out/n; dst.parent.mkdir(parents=True,exist_ok=True); dst.write_bytes(z.read(n))
    # Cursor files shipped by this repository.
    repo_root = Path(__file__).resolve().parent.parent
    for name in ("cursor_pointer.png", "cursor_grab.png"):
        src = repo_root / name
        if not src.is_file():
            raise SystemExit(f"missing {src}")
        shutil.copy2(src, out / name)
    if args.nro:
        shutil.copy2(args.nro,out/"stupidzombies_nx.nro")
    print(f"Prepared {out}")
    print("Verified all three ARM64 library hashes for 3.4.5.")

if __name__=="__main__": main()
