"""xref.py DUMP STRING... -- where each string sits in a dump of the
internal flash (loaded at 0xE0000), and where its address appears."""
import sys,re
d=open(sys.argv[1],'rb').read(); base=0xe0000
for s in sys.argv[2:]:
    i=d.find(s.encode())
    if i<0: print(s,'not found'); continue
    a=base+i; lo=(a&0xffff).to_bytes(2,'little')
    refs=[hex(base+m.start()) for m in re.finditer(re.escape(lo),d)]
    print(f'{s!r} @ {a:#x}: refs {refs[:12]}')
