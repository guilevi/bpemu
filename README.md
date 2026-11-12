# bpemu — an APH Book Port emulator

Runs the Book Port's own firmware (2.3, from `bpx_setup/bpx._zz`) on an
emulated Renesas M30245, with its DoubleTalk RC8650 speech chip emulated by
librc8650 running the chip's own firmware.  Both CPUs are GDB's m32c
simulator core as patched in the doubletalk work.

```
git submodule update --init   # fetch librc8650
make            # builds bpemu, bprun, and a first card image
./bpemu         # the window: keys below, speech on the default audio device
```

Needs SDL3 and libmpg123 (`brew install sdl3 mpg123`).  The firmware goes in
`fw/`: `bpfw._BB` and the update files `BPMsgs._BB`, `BPfwex._BB`,
`Spanish.dix`, plus the RC8650 image as `fw/rc8650.srec` (or point
`RC8650_FIRMWARE` at it).

## Windows

Same program, same keys.  To build it from a Mac (or Linux) with
`x86_64-w64-mingw32-gcc`:

```
make windows    # fetches SDL3 and libmpg123 (tools/windeps.sh), builds dist/windows
```

`dist/windows` then holds `bpemu.exe`, `bprun.exe`, `bpcard.exe`,
`SDL3.dll` and `libmpg123-0.dll`; copy the folder to the PC, add the `fw`
folder next to the programs, and run `bpemu.exe`.  On first start it makes
`cards\card.img` (with the update files from `fw`) and `state\flash.bin`
next to itself.  On Windows itself, under MSYS2 (UCRT64:
`pacman -S make mingw-w64-ucrt-x86_64-{gcc,SDL3,mpg123,pkgconf}`), plain
`make` builds it the same way as on a Mac.

## Keys

By physical position (scancodes), so any keyboard layout works:

| Keyboard | Book Port |
|---|---|
| `1 2 3` / `q w e` / `a s d` / `z x c` | `1 2 3` / `4 5 6` / `7 8 9` / `* 0 #` |
| `4 5 6` / `r t y` | `A B C` / `D E F` |

Hold several at once for chord commands (A+C version, D+F battery,
2+B reset, …).  Escape quits.

## First run

The external flash starts erased, so the firmware has no application ("application
Missing" on every key).  `make` creates `cards/card.img` holding
`BPMsgs._BB`, `BPfwex._BB` and `Spanish.dix`; at power-on the Book Port
installs them exactly as the real one does ("please wait while messages are
updated … application done … Spanish done") and deletes them from the card.
The flash is kept in `state/flash.bin`, so this happens once.  To redo it:
`rm state/flash.bin cards/card.img && make` (or just start `bpemu`, which
makes the card itself when there is none).

Every power-on says "defaults restored" until the settings have been saved
once: the firmware keeps them in RAM and writes them to the external flash
(sector 0x78000, after a 0x9669 marker) only on D+E+F, "preserve your
settings before changing batteries".  Quitting the emulator is a battery
change.

## Putting books and audio on the card

Drop files on the window, paste them (copy them in Finder or Explorer,
then Cmd+V, or Ctrl+V on Windows), or give them with `-a` (both `bpemu`
and `bprun`; `-D folder` puts them in a folder).  Pasting text instead of
files makes a book of that text.

```
./bpemu -a book.txt -a talk.mp3
```

Text files become indexed books, as UBT (the Unix Book Transfer tool)
makes them: `book.txt._DD`, the text reflowed one sentence per line, and
`book.txt._IX`, the index (the format is in `tools/bpindex.py`'s
docstring; `card.c` does the same in C).
Plain `.txt` copied as it is would be refused ("playing unfiltered files not
supported").  Unlike UBT it never starts a line with a space (the firmware
loses a line of its reading position on each, so pausing UBT books jumped
back further the longer you listened) and marks every paragraph.  MP3s are
copied as they are and navigate in fixed 2/10/30/60-second steps, as on
the real unit.

A drop takes the card out, adds the files and power-cycles the Book Port:
the firmware lists a card's files when it mounts it, at power-on (or at an
auto-off that finds the card gone), so a running unit would not see them.
`bprun -A t:file` does the same at t seconds.

`bpcard` does it from the command line, with no other tools needed (the
FAT16/FAT32 writing is `card.c`'s own):

```
bpcard new IMAGE [MB] [FILE…]         # a fresh card, MBR + FAT, default 128 MB
bpcard add IMAGE [-d FOLDER] FILE…    # add books and audio
bpcard index book.txt [OUTDIR]        # just write book.txt._DD/._IX
```

Names keep their case and length (long-name entries), and get a short
name the firmware can find them by: it looks update files such as
`BPMsgs._BB` up by their 8.3 name.

## The hardware, as found

| | |
|---|---|
| CPU | Renesas M30245 (M16C/60 + USB), 14.7456 MHz crystal |
| Internal flash | 0xE0000–0xFFFFF: `bpfw._BB` (header = load address << 12) |
| External flash | AMD Am29LV400BT, 512 KB, CPU address = chip address, from 0x30000; application at 0x30000 (magic 0xF1A5C0DE), messages at 0x7C000 |
| CompactFlash | memory-mapped ATA at 0x6000: task file 0x6000–0x600F, data 0x6400–0x67FF; card detect P8_3 (low = present) |
| RC8650 | UART0 at 115200 (U0BRG = 7), CTS# on P6_0, RESET on P10_2 |
| I2C (UART2) | DAC3550A at 0x4D, MAS3507D MP3 decoder at 0x1D, RTC at 0x68 |
| MP3 data | UART1, clock-synchronous, into the MAS3507D's serial input; its demand line on P8_4 raises interrupt 0x5E (vector 30) on a rising edge; the pump at 0xE1BE0 sends from two 4 KB buffers at 0xC808/0xD808 while it is high |
| DAC3550A | AVOL (subaddress 0xC2, left/right) = 0x2C at volume 44 |
| Card detect | P8_3 also raises 0x5F (vector 31) on insertion |
| RTC | Dallas DS1337/DS1339-style: the firmware reads and writes 0x00-0x0F in one burst; BCD time in 0-6 (year = 2000 + reg 6), 0x0F bit 7 (OSF) set means "date and time not set" |
| Keypad | 4 × 5 matrix, rows P7_4–P7_7, columns P10_3–P10_7 (key-input interrupt); map in `bp_hw.c` |
| USB VBUS sense | P9_1 (held low: unplugged) |
| Battery | A/D channel AN1 (reads full) |

## Not emulated yet

WAV and the other audio formats (only MP3 goes through the MAS), the
MAS3507D's memory and registers (reads return 0, which the firmware
accepts), TA4's event input (the firmware counts it into 0x428 during
playback; nothing drives it yet), USB, the microphone, rewriting the
internal flash (a card with `bpfw._BB` on it takes the code-update path,
which needs CPU rewrite mode), power-off/standby beyond what the firmware
does by itself.

The last partial 4 KB of an MP3 is not played: when the read for the next
buffer finds end of file, the firmware zeroes the count of the buffer still
playing (0xE4360-0xE43A5).  As far as the code shows, the real unit does
the same.

## Debugging

`bprun` is the same machine without a window: it prints the RC8650 stream
(`^A` = command char, `{73@t}` = chip replies) and can write a WAV.

```
./bprun -s 30 -c card.img -o out.wav -k "8:kA+kC,12:k2"   # scripted keys
```

Environment: `BP_TIME=epoch` pins the RTC (reproducible runs),
`BP_MASLOG=1` logs the MP3 path (I2C to the audio chips, bytes in, time played),
`BP_WATCH=addr,…` logs writes and `BP_RWATCH=…` reads (`lo-hi` ranges allowed), `BP_BREAK=pc[,n]` dumps registers and stack,
`BP_PINS=port:mask:value` forces input pins.  `-l` takes a mask from `bp.h`
(SFR, external bus, interrupts, UART/I2C, PC samples, full trace).

`vendor/librc8650` is a git submodule (github.com/guilevi/librc8650);
after cloning run `git submodule update --init`.  `vendor/gdb-sim-m32c` is a
pristine copy of `gdb-14.2/sim/m32c`; `make` copies it to `build/sim/m32c`
and applies the submodule's `sim/rc865x-sim.patch`, so the core always
matches the librc8650 revision checked out.  To move to a newer librc8650:
`git -C vendor/librc8650 pull && make && git add vendor/librc8650`.  GPL-3.

## Audio

Speech comes out through librc8650's BookPort output mode, measured from a
real unit: the D/A's sample-and-hold staircase at 10,488 Hz, images and
all, under a first-order 5 kHz low-pass, delivered at 48 kHz
(`rcx_bookport_output` in `rc_glue.c`).  `bprun -o` writes 48 kHz WAVs.

MP3s are decoded by libmpg123 (`mas.c`), fed only as fast as playback
consumes them so the demand line behaves, resampled to 48 kHz and scaled by
the DAC's AVOL.  While one plays, output is paced by emulated time and the
speech is mixed in; otherwise speech comes out as the chip produces it.
