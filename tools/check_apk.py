#!/usr/bin/env python3
"""Bring-up preflight for Stupid Zombies 3.4.5 Android -> Switch wrapper."""
from pathlib import Path
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile, zipfile

EXPECTED_APK="b35b463f07479baf2f1b5a7260aa6ddf5399882d25ff7b82edaa5b0c7a24e31b"
EXPECTED={
 "libmain.so":"f172319d1045e9baa23052e47c78a560d6cf0b85e92ab1201f71f768c2f3a051",
 "libunity.so":"d86adb00b1cd1794bb564f9c04bc24627e4cac0b1d91e6be961c6d852bb81fe2",
 "libil2cpp.so":"687dcbef0bfad0fb3fec545f2a5106e53357c1a477605328c4489096ecab4572",
}
EXPECTED_NDK={
 "ALooper_acquire","ALooper_forThread","ALooper_pollAll","ALooper_prepare","ALooper_release","ALooper_wake",
 "ANativeWindow_acquire","ANativeWindow_fromSurface","ANativeWindow_getHeight","ANativeWindow_getWidth",
 "ANativeWindow_release","ANativeWindow_setBuffersGeometry",
}
HERE=Path(__file__).resolve().parent

def run(cmd): return subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True).stdout

def sha_file(p):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for b in iter(lambda:f.read(1<<20),b''): h.update(b)
    return h.hexdigest()

def pass_(s): print(f"[PASS] {s}")
def warn(s): print(f"[WARN] {s}")
def fail(s): print(f"[FAIL] {s}"); return 1

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("apk"); args=ap.parse_args(); apk=Path(args.apk)
    bad=0
    if not apk.is_file(): raise SystemExit("APK not found")
    got=sha_file(apk)
    (pass_ if got==EXPECTED_APK else warn)(f"APK SHA256 {got}" + (" (exact 3.4.5 target)" if got==EXPECTED_APK else " (different package build)"))
    with tempfile.TemporaryDirectory() as td, zipfile.ZipFile(apk) as z:
        td=Path(td); names=set(z.namelist())
        for fn in EXPECTED:
            zp=f"lib/arm64-v8a/{fn}"
            if zp not in names: bad+=fail(f"missing ARM64 {fn}"); continue
            p=td/fn; p.write_bytes(z.read(zp)); h=sha_file(p)
            (pass_ if h==EXPECTED[fn] else warn)(f"{fn} {p.stat().st_size} bytes sha={h}")
        if bad: return bad
        data=(td/"libmain.so").read_bytes()
        vers=sorted(set(re.findall(rb"2022\.3\.\d+f\d+(?:_[0-9a-f]+)?",data)))
        if vers: pass_("Unity " + ", ".join(v.decode() for v in vers))
        else: bad+=fail("Unity version not found in libmain.so")
        if b"libil2cpp.so" in data: pass_("IL2CPP loader reference present")
        else: bad+=fail("libmain.so does not reference libil2cpp.so")
        # Basic ELF dependency/import audit when readelf is installed.
        if shutil.which("readelf"):
            dyn=run(["readelf","-dW",str(td/"libunity.so")])
            needed=re.findall(r"Shared library: \[(.*?)\]",dyn)
            print("[INFO] libunity DT_NEEDED:",", ".join(needed))
            syms=run(["readelf","-Ws",str(td/"libunity.so")])
            undef=set()
            for line in syms.splitlines():
                if " UND " in line:
                    name=line.split()[-1].split('@')[0]; undef.add(name)
            missing=EXPECTED_NDK-undef
            if not missing: pass_("classic Unity Android NDK surface present, including ALooper_pollAll")
            else: warn("expected NDK imports changed: missing " + ", ".join(sorted(missing)))
            if any(x.startswith("AChoreographer_") for x in undef): warn("AChoreographer NDK imports detected")
            else: pass_("no AChoreographer_* NDK import")
        else: warn("readelf not installed; skipped ELF import audit")
        # Run exact JNI/allocator scanners included with the kit.
        ep=run([sys.executable,str(HERE/"extract_entrypoints.py"),str(td/"libunity.so")])
        if "0x6018fc" in ep and "(30 methods)" in ep: pass_("exact 30-entry UnityPlayer JNI table / JNI_OnLoad recovered")
        else: warn("JNI entrypoint layout differs; regenerate unity_entrypoints.h before building")
        sc=run([sys.executable,str(HERE/"scan_alloc_patches.py"),str(td/"libunity.so")])
        if "0x3a88cc" in sc and "0x3b149c" in sc: pass_("allocator transform cluster matches bring-up table")
        else: warn("allocator cluster differs; do NOT apply bundled patch table")
        # APK data integrity.
        req=["assets/bin/Data/boot.config","assets/bin/Data/data.unity3d","assets/bin/Data/Managed/Metadata/global-metadata.dat"]
        for n in req:
            if n in names: pass_(f"{n} present")
            else: bad+=fail(f"missing {n}")
        if "AndroidManifest.xml" in names:
            mp=td/"AndroidManifest.xml"; mp.write_bytes(z.read("AndroidManifest.xml"))
            try:
                man=run([sys.executable,str(HERE/"parse_axml.py"),str(mp)])
                if "package='com.gameresort.stupidzombies'" in man: pass_("package com.gameresort.stupidzombies")
                else: warn("package differs from target")
                if "UnityPlayerActivity" in man: pass_("UnityPlayerActivity launcher surface found")
            except Exception as e: warn(f"manifest parser failed: {e}")
    print("\nPreflight result:", "READY FOR BRING-UP" if bad==0 else f"{bad} hard failure(s)")
    return 1 if bad else 0

if __name__=="__main__": raise SystemExit(main())
