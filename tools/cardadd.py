#!/usr/bin/env python3
"""cardadd.py IMAGE [-d DIR] [-u] FILE...

Put files on a card image the way the Book Port wants them.  Text files
(.txt) are indexed with bpindex.py and their NAME._DD / NAME._IX copied;
anything else (MP3, WAV, ...) is copied as it is, which the Book Port plays
with fixed 2/10/30/60-second steps instead of index marks.  -d puts the
files in DIR on the card (created if needed); -u passes -u to bpindex
(UBT-exact output).  Needs mtools; the FAT partition is found from the MBR,
or the image is taken as a bare FAT volume if it has none.
"""
import os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bpindex  # noqa: E402


def fat_offset(img):
    with open(img, "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        return 0
    # A FAT boot sector also ends in 55 AA; an MBR has a partition entry
    # with a FAT type and a sane start.
    typ = mbr[446 + 4]
    start = struct.unpack_from("<I", mbr, 446 + 8)[0]
    if typ in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E) and start:
        return start * 512
    return 0


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    img, rest = argv[0], argv[1:]
    folder, ubt, files = "", False, []
    it = iter(rest)
    for a in it:
        if a == "-d":
            folder = next(it).strip("/\\")
        elif a == "-u":
            ubt = True
        elif a.startswith("-"):
            sys.exit(__doc__)
        else:
            files.append(a)
    if not files:
        sys.exit(__doc__)
    target = "%s@@%d" % (img, fat_offset(img))
    dest = "::/" + folder if folder else "::/"
    if folder:
        subprocess.run(["mmd", "-D", "s", "-i", target, dest], capture_output=True)
    tmp = tempfile.mkdtemp(prefix="cardadd")
    try:
        copies = []
        for f in files:
            name = os.path.basename(f)
            if name.lower().endswith(".txt"):
                index = bpindex.index_text if ubt else bpindex.index_text_fixed
                dd, title, marks = index(open(f, "rb").read(), name)
                for ext, data in (("._DD", dd), ("._IX", bpindex.build_ix(name, title, name, marks))):
                    p = os.path.join(tmp, name + ext)
                    open(p, "wb").write(data)
                    copies.append(p)
                print("%s: book %r, %d marks" % (name, title, len(marks)))
            else:
                copies.append(f)
                print("%s: copied as is" % name)
        r = subprocess.run(["mcopy", "-o", "-i", target] + copies + [dest],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("mcopy: " + (r.stderr.strip() or "failed"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main(sys.argv[1:])
