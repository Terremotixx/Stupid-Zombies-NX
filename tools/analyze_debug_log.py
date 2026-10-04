#!/usr/bin/env python3
from pathlib import Path
import argparse,re
ap=argparse.ArgumentParser(); ap.add_argument('log'); a=ap.parse_args()
t=Path(a.log).read_text(errors='replace')
checks=[
 ('JNI_OnLoad called','[boot] calling JNI_OnLoad(fake_vm)',3),
 ('JNI_OnLoad returned','[boot] JNI_OnLoad returned',4),
 ('Unity entrypoints resolved','[boot] entry points resolved',5),
 ('First frame rendered','frame 0 rendered',8),
]
print('Stupid Zombies NX bring-up log summary')
last=0
for label,needle,stage in checks:
    ok=needle in t
    print(('[PASS] ' if ok else '[....] ')+label)
    if ok:last=max(last,stage)
crash=[x for x in t.splitlines() if '[xd]' in x or 'crash' in x.lower() or 'abort' in x.lower()]
stubs=[x for x in t.splitlines() if '[stub]' in x]
unimpl=[x for x in t.splitlines() if 'unimpl' in x.lower() or 'unresolved' in x.lower() or 'unable to resolve' in x.lower()]
print(f'\nReached stage {last}/8; stubs={len(stubs)}, unresolved/unimpl hints={len(unimpl)}, crash hints={len(crash)}')
for title,rows in [('First stub hits',stubs[:15]),('Unresolved/JNI hints',unimpl[:20]),('Crash tail',crash[-20:])]:
    if rows:
        print('\n'+title+':')
        for r in rows: print('  '+r[:500])
# Always print a useful tail for the next iteration.
print('\nLast 40 log lines:')
for r in t.splitlines()[-40:]: print(r)
