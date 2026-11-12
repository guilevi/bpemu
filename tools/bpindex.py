#!/usr/bin/env python3
"""bpindex.py [-1|-2] [-o DIR] [-t] [-l] [-u] FILE.txt...

Index text files for the Book Port the way UBT (Brian Buhrow's Unix Book
Transfer, 0.93) does: for each NAME writes NAME._DD (the text, reflowed so
each sentence is one line) and NAME._IX (title, paths and navigation marks)
into DIR (default: the current directory).  Copy both onto the card.

  -1  old-style index (Book Port firmware 2.1 and earlier; also the default
      in UBT), 4-byte marks with the key level folded into the offset
  -2  new-style index (firmware 2.2 and later), 6-byte marks
  -t  use the file name as the title instead of the first line
  -l  index by line instead of by sentence
  -u  byte-for-byte UBT output, including its paragraph-break bug (below)

Unlike UBT, a blank line between paragraphs is dropped instead of becoming a
space at the start of the next line.  The firmware loses one line of its
reading position for every line that starts with a space, so with UBT books
pausing jumped back further the longer you had been listening (one line per
paragraph read since you last pressed play).

._IX layout (all little-endian):
  header, HLEN bytes (14 for v1, 48 for v2):
    0     1
    1     HLEN
    2     2 for v2, else 0
    4     file type: 1 text, 4 MP3
    6     u32 number of marks
    10    u16 bytes up to the marks (header + the three strings + 6)
  u16 n, n bytes: Book Port file name, NUL-terminated
  u16 n, n bytes: title, NUL-terminated
  u16 n, n bytes: host path, NUL-terminated
  marks, one per stop:
    v1: u32 offset | level bits (text: 0x00400000 paragraph, 0x00c00000
        page, 0x03400000 section, 0x05400000 subsection)
    v2: u8 level (0 sentence, 1 paragraph, 3 page, 0x34 section,
        0x54 subsection), u8 0, u32 offset
Text markup: form feed starts a page, byte 1 a section, bytes 2-6 a
subsection.
"""
import os, struct, sys

SENT, PARA, PAGE, SECT, SUBSEC = 1, 2, 3, 4, 5
V1_BITS = {SENT: 0, PARA: 0x00400000, PAGE: 0x00C00000, SECT: 0x03400000, SUBSEC: 0x05400000}
V2_CODE = {SENT: 0, PARA: 1, PAGE: 3, SECT: 0x34, SUBSEC: 0x54}
PAGESIZE, LINESIZE, MAX_LINE = 66, 75, 950


def index_text(data, name, title_is_name=False, lineindex=False):
    """Return (dd_bytes, title, marks) following UBT's index_text()."""
    out = bytearray()
    marks = []
    mark = lambda off, t: marks.append((off, t))
    nl = data.find(b"\n")
    first = data[: nl + 1] if nl >= 0 else data
    rest = data[len(first):]
    if first.endswith(b"\r\n"):
        first = first[:-2] + b"\n"
    if len(first) < 2 or title_is_name:
        first = name.encode("latin1") + b"\0"
    out += first
    title = first[:-1][:80].decode("latin1")
    mark(0, PAGE)
    mark(len(out), SENT)

    linesize = LINESIZE if lineindex else MAX_LINE
    OPEN, NEWPAR = 1, 3
    state, pending, linelen, curline, prev = OPEN, False, 0, 2, 0
    for c in rest:
        if c in (13, 127):
            continue
        if c == 10:
            if lineindex:
                out.append(c); prev = c; curline += 1; linelen = 0
                if curline >= PAGESIZE:
                    mark(len(out), PAGE); curline = 1; state = OPEN
                elif state == NEWPAR:
                    mark(len(out), PARA); state = OPEN
                else:
                    state = NEWPAR; mark(len(out), SENT)
            elif pending:
                out.append(c); prev = c
                if state == NEWPAR:
                    mark(len(out), PARA); state = OPEN
                else:
                    if curline >= PAGESIZE:
                        mark(len(out), PAGE); curline = 0
                    else:
                        mark(len(out), SENT)
                    state = NEWPAR
                pending = False; curline += 1; linelen = 0
            else:
                out.append(32); prev = 32; linelen += 1
        elif c == 34:
            out.append(c); linelen += 1
            if prev in b".?!," and not lineindex:
                pending = True
            prev = c
        elif c in b".?!":
            out.append(c); prev = c; linelen += 1
            if not lineindex:
                pending = True
        elif c in (9, 32):
            if c == 9 and not lineindex and state == NEWPAR:
                c = 10; curline += 1; linelen = 0
                mark(len(out) + 1, PARA); state = OPEN
            linelen += 1
            if not lineindex and (pending or linelen >= linesize):
                mark(len(out) + 1, SENT); pending = False
                c = 10; linelen = 0; curline += 1
            if lineindex and linelen >= linesize:
                c = 10
                if curline >= PAGESIZE:
                    mark(len(out) + 1, PAGE); curline = 0
                else:
                    mark(len(out) + 1, SENT); curline += 1
                linelen = 0
            if prev != 10 or c != 32:
                out.append(c)
            prev = c
        elif c == 12:
            out.append(10); prev = 10; curline = 0; linelen = 0
            mark(len(out), PAGE); state = OPEN
        elif c == 1:
            out.append(10); mark(len(out), SECT); state = OPEN; pending = False
        elif 2 <= c <= 6:
            out.append(10); mark(len(out), SUBSEC); state = OPEN; pending = False
        else:
            out.append(c & 0x7F); prev = c & 0x7F; linelen += 1; pending = False
    mark(len(out) + 1, SENT)
    out += b"\n\0\0"
    return bytes(out), title, marks


RANK = {SENT: 0, PARA: 1, PAGE: 2, SUBSEC: 3, SECT: 4}


def index_text_fixed(data, name, title_is_name=False, lineindex=False):
    """Like index_text(), with UBT's paragraph handling fixed: no line ever
    starts with a space (the firmware loses its place on those), and every
    blank line or tab-indented line starts a paragraph (UBT marked only every
    other one).  Other rules (sentence splitting, pages every 66 lines, form
    feed and bytes 1-6, the title line, the end of the file) are UBT's."""
    out = bytearray()
    marks = []
    nl = data.find(b"\n")
    first = data[: nl + 1] if nl >= 0 else data
    rest = data[len(first):]
    if first.endswith(b"\r\n"):
        first = first[:-2] + b"\n"
    if len(first) < 2 or title_is_name:
        first = name.encode("latin1") + b"\0"
    out += first
    title = first[:-1][:80].decode("latin1")
    marks += [(0, PAGE), (len(out), SENT)]

    linesize = LINESIZE if lineindex else MAX_LINE
    st = dict(pending=False, linelen=0, curline=2, nls=1, para=False)

    def brk(kind):
        """End the current line (turning a trailing space into the newline)
        and mark the start of the next one, keeping the strongest kind."""
        if out[-1:] == b" ":
            out[-1] = 10
        elif out[-1:] != b"\n":
            out.append(10)
            st["curline"] += 1
        if kind == SENT and st["curline"] >= PAGESIZE:
            kind = PAGE
        if kind == PAGE:
            st["curline"] = 0
        if marks[-1][0] == len(out):
            if RANK[kind] > RANK[marks[-1][1]]:
                marks[-1] = (len(out), kind)
        else:
            marks.append((len(out), kind))
        st["linelen"] = 0
        st["pending"] = False

    def text(c):
        if st["nls"] >= 2 or st["para"]:
            brk(PARA)
        st["nls"] = 0
        st["para"] = False
        out.append(c & 0x7F)
        st["linelen"] += 1

    prev = 0
    for c in rest:
        if c in (13, 127):
            continue
        at_start = out[-1:] == b"\n"
        if c == 10:
            st["nls"] += 1
            if lineindex or st["pending"]:
                if not (at_start and st["nls"] >= 2):
                    brk(SENT)
            elif not at_start and out[-1:] != b" ":
                out.append(32)
                st["linelen"] += 1
        elif c in (9, 32):
            if at_start or out[-1:] == b" " and st["nls"]:
                if c == 9 and st["nls"]:
                    st["para"] = True
            elif st["pending"] or st["linelen"] + 1 >= linesize:
                brk(SENT)
            else:
                out.append(c)
                st["linelen"] += 1
        elif c == 12:
            brk(PAGE)
        elif c == 1:
            brk(SECT)
        elif 2 <= c <= 6:
            brk(SUBSEC)
        else:
            if st["pending"] and lineindex is False and c not in b".?!\"":
                st["pending"] = False
            text(c)
            if c in b".?!" and not lineindex:
                st["pending"] = True
            elif c == 34 and prev in b".?!," and not lineindex:
                st["pending"] = True
        prev = c
    marks.append((len(out) + 1, SENT))
    out += b"\n\0\0"
    return bytes(out), title, marks


def build_ix(bpath, title, hpath, marks, ftype=1, version=1):
    s = lambda t: t.encode("latin1")[:256] + b"\0"
    bp, ti, hp = s(bpath), s(title), s(hpath)
    hlen = 48 if version == 2 else 14
    hdr = bytearray(hlen)
    hdr[0], hdr[1], hdr[4] = 1, hlen, ftype
    if version == 2:
        hdr[2] = 2
    struct.pack_into("<IH", hdr, 6, len(marks), hlen + 6 + len(bp) + len(ti) + len(hp))
    ix = bytes(hdr) + struct.pack("<H", len(bp)) + bp + struct.pack("<H", len(ti)) + ti \
        + struct.pack("<H", len(hp)) + hp
    for off, t in marks:
        if version == 2:
            ix += struct.pack("<BBI", V2_CODE[t], 0, off)
        else:
            ix += struct.pack("<I", off | (V1_BITS[t] if ftype == 1 else 0))
    return ix


def main(argv):
    version, outdir, tin, lineindex, fix, files = 1, ".", False, False, True, []
    it = iter(argv)
    for a in it:
        if a in ("-1", "-2"):
            version = int(a[1])
        elif a == "-o":
            outdir = next(it)
        elif a == "-t":
            tin = True
        elif a == "-l":
            lineindex = True
        elif a == "-u":
            fix = False
        elif a.startswith("-"):
            sys.exit(__doc__)
        else:
            files.append(a)
    if not files:
        sys.exit(__doc__)
    for f in files:
        name = os.path.basename(f)
        dd, title, marks = (index_text_fixed if fix else index_text)(
            open(f, "rb").read(), name, tin, lineindex)
        open(os.path.join(outdir, name + "._DD"), "wb").write(dd)
        open(os.path.join(outdir, name + "._IX"), "wb").write(
            build_ix(name, title, name, marks, 1, version))
        print("%s: %r, %d marks" % (name, title, len(marks)))


if __name__ == "__main__":
    main(sys.argv[1:])
