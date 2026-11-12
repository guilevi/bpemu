#!/bin/sh
# mkcard.sh IMAGE [SIZE_MB] [FILE...]
#
# Make a CompactFlash card image the way a PC would leave one: an MBR with a
# single FAT partition at sector 63 (FAT16 up to 2 GB, FAT32 above), then
# copy FILEs into its root.  Needs mtools.
set -e
img=$1
mb=${2:-128}
[ -n "$img" ] || { echo "usage: mkcard.sh IMAGE [SIZE_MB] [FILE...]" >&2; exit 2; }
shift; [ $# -gt 0 ] && shift

sectors=$((mb * 2048))
start=63
psect=$((sectors - start))
if [ "$mb" -gt 2000 ]; then type=0x0C; fat=-F; else type=0x06; fat=; fi

rm -f "$img"
dd if=/dev/zero of="$img" bs=1048576 count=0 seek="$mb" 2>/dev/null

# MBR: one active partition.  CHS fields are left at the LBA-only values.
python3 -I - "$img" "$start" "$psect" "$type" <<'EOF'
import struct, sys
img, start, n, typ = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4], 0)
mbr = bytearray(512)
entry = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', typ, b'\xfe\xff\xff', start, n)
mbr[446:462] = entry
mbr[510:512] = b'\x55\xaa'
with open(img, 'r+b') as f:
    f.write(mbr)
EOF

mformat -i "$img@@$((start * 512))" $fat -T "$psect" -h 16 -s 63 -H "$start" -v BOOKPORT ::
for f in "$@"; do
  mcopy -i "$img@@$((start * 512))" "$f" ::
done
mdir -i "$img@@$((start * 512))" ::
