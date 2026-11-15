# Book Port emulator.
#
# Two Renesas M16C CPUs run in one process, both on GDB's m32c simulator as
# patched for librc8650: $(B)/sim/m32c is the pristine gdb-14.2
# sim/m32c in vendor/gdb-sim-m32c with the librc8650 submodule's
# sim/rc865x-sim.patch applied, so the core always matches the library:
#
#   - the RC8650 speech chip, through librc8650 (vendor/librc8650), with the
#     core's original symbol names;
#   - the Book Port's own M30245, with the core recompiled under rename.h and
#     bp_hw.c in place of mem.c.
#
# make            this machine (macOS, Linux, or Windows under MSYS2)
# make windows    cross-build for Windows from here, into dist/windows
#                 (needs x86_64-w64-mingw32-gcc; tools/windeps.sh fetches
#                 SDL3 and libmpg123 for it)
# make mac        a self-contained universal macOS build, into dist/mac
#                 (tools/macdeps.sh fetches SDL3's framework, builds mpg123)

GDBSIM  := vendor/gdb-sim-m32c
LIBRC   := vendor/librc8650

CFLAGS  ?= -O2 -g
HOSTCC  ?= cc
WARNOFF := -Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
           -Wno-int-conversion -Wno-unused-result -Wno-deprecated-non-prototype

UNAME := $(shell uname -s)
ifeq ($(WIN),1)
  # Cross-building for Windows.
  PLAT    := win
  B       := build-win
  OUT     := dist/windows/
  CC      := x86_64-w64-mingw32-gcc
  NM      := x86_64-w64-mingw32-nm
  WDEPS   := deps/windows
  SDL_CFLAGS := -I$(WDEPS)/SDL3/include
  SDL_LIBS   := -L$(WDEPS)/SDL3/lib -lSDL3
  MPG_CFLAGS := -I$(WDEPS)/mpg123
  MPG_LIBS   := $(WDEPS)/mpg123/libmpg123-0.dll
else ifeq ($(MAC),1)
  # Universal, macOS 11 and later, bundling SDL3.framework.
  B       := build-mac
  OUT     := dist/mac/
  MDEPS   := deps/mac
  ARCHS   := -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
  CC      := cc $(ARCHS)
  NM      := nm
  SDL_CFLAGS := -F$(MDEPS)
  SDL_LIBS   := -F$(MDEPS) -framework SDL3 -Wl,-rpath,@executable_path
  MPG_CFLAGS := -I$(MDEPS)/mpg123/include
  MPG_LIBS   := $(MDEPS)/mpg123/lib/libmpg123.a
else
  B       := build
  OUT     :=
  CC      ?= cc
  NM      ?= nm
  SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
  SDL_LIBS   := $(shell pkg-config --libs sdl3 2>/dev/null)
  MPG_CFLAGS := $(shell pkg-config --cflags libmpg123 2>/dev/null)
  MPG_LIBS   := $(shell pkg-config --libs libmpg123 2>/dev/null)
  ifeq ($(UNAME),Darwin)
    PLAT := mac
  else ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME)),)
    PLAT := win
  else
    PLAT := unix
  endif
endif

ifeq ($(MAC),1)
  PLAT := mac
endif

ifeq ($(PLAT),win)
  EXE      := .exe
  GUI_LIBS := -mwindows -lshell32 -luser32
  CLIPSRC  := plat/clip_win.c
else ifeq ($(PLAT),mac)
  EXE      :=
  GUI_LIBS := -framework AppKit
  CLIPSRC  := plat/clip_mac.m
else
  EXE      :=
  GUI_LIBS :=
  CLIPSRC  := plat/clip_sdl.c
endif

SIM     := $(B)/sim/m32c
GEN     := $(B)/gen
# On Windows the core's standalone-console code (still compiled, though
# unused, as librc8650's patch leaves it) gets stand-in POSIX headers.
COREINC := -I$(LIBRC) -I$(SIM) -I$(GEN) \
           $(if $(filter win,$(PLAT)),-Iplat/wincompat -DHAVE_TERMIOS_H=1)
RENAME  := $(B)/rename.h

CORE    := reg srcdest misc int
# The RC8650: the whole patched core plus librc8650.
RCOBJ   := $(addprefix $(B)/rc_,$(addsuffix .o,mem $(CORE) r8c m32c stubs \
             rcdict rcdict_regex rc_glue))
# The Book Port: the CPU half of the core, renamed, plus the board.
BPOBJ   := $(addprefix $(B)/bp_,$(addsuffix .o,$(CORE) r8c)) $(B)/bp_hw.o \
           $(B)/cf.o $(B)/norflash.o
PROGS   := $(OUT)bprun$(EXE) $(OUT)bpemu$(EXE) $(OUT)bpcard$(EXE)

.PHONY: all clean windows mac
ifeq ($(WIN),1)
all: $(PROGS) $(OUT)SDL3.dll $(OUT)libmpg123-0.dll
else ifeq ($(MAC),1)
all: $(PROGS) $(OUT)SDL3.framework
	codesign -f -s - $(PROGS)
else
all: $(PROGS)
endif

windows:
	@test -d deps/windows || sh tools/windeps.sh
	$(MAKE) WIN=1
mac:
	@test -d deps/mac/mpg123 || sh tools/macdeps.sh
	$(MAKE) MAC=1


$(B) $(GEN):
	@mkdir -p $@
ifneq ($(OUT),)
$(OUT):
	@mkdir -p $@
$(OUT)SDL3.dll: | $(OUT)
	cp $(WDEPS)/SDL3/bin/SDL3.dll $@
$(OUT)libmpg123-0.dll: | $(OUT)
	cp $(WDEPS)/mpg123/libmpg123-0.dll $@
$(OUT)SDL3.framework: | $(OUT)
	rm -rf $@ && ditto $(MDEPS)/SDL3.framework $@
endif

$(LIBRC)/rc8650.c $(LIBRC)/sim/rc865x-sim.patch:
	@echo "$(LIBRC) is empty: run  git submodule update --init" >&2; exit 1

# The patched core.  The sources are touched after the stamp so the empty
# rule below never makes them look out of date.
SIMSRC  := $(addprefix $(SIM)/,mem.c reg.c srcdest.c misc.c int.c opc2c.c \
             r8c.opc m32c.opc)
$(SIM)/.stamp: $(LIBRC)/sim/rc865x-sim.patch $(wildcard $(GDBSIM)/*) | $(B)
	rm -rf $(B)/sim && mkdir -p $(B)/sim && cp -R $(GDBSIM) $(SIM)
	cd $(B) && patch -s -p1 < ../$(LIBRC)/sim/rc865x-sim.patch
	touch $@ && touch $(SIM)/*
$(SIMSRC): $(SIM)/.stamp ;

# opc2c turns the .opc instruction descriptions into the decoders; it runs
# here, so it is built for here.
$(B)/opc2c: $(SIM)/opc2c.c | $(B)
	$(HOSTCC) -O2 -w -I$(LIBRC) -o $@ $<
$(GEN)/%.c: $(SIM)/%.opc $(B)/opc2c | $(GEN)
	$(B)/opc2c -l $@.log $< > $@.tmp && mv $@.tmp $@

# RC8650 side.
$(B)/rc_%.o: $(SIM)/%.c | $(B)
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/rc_r8c.o: $(GEN)/r8c.c | $(B)
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/rc_m32c.o: $(GEN)/m32c.c | $(B)
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/rc_stubs.o: $(LIBRC)/stubs.c $(SIM)/.stamp | $(B)
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/rc_rcdict.o: $(LIBRC)/rcdict/rcdict.c | $(B)
	$(CC) $(CFLAGS) -I$(LIBRC)/rcdict -c $< -o $@
$(B)/rc_rcdict_regex.o: $(LIBRC)/rcdict/rcdict_regex.c | $(B)
	$(CC) $(CFLAGS) -w -I$(LIBRC)/rcdict -c $< -o $@
$(B)/rc_rc_glue.o: rc_glue.c rcglue.h $(LIBRC)/rc8650.c $(SIM)/.stamp | $(B)
	$(CC) $(CFLAGS) $(COREINC) -I. $(WARNOFF) -c $< -o $@

# Book Port side.  rename.h comes from the RC copies' symbol tables.
$(RENAME): $(B)/rc_reg.o $(B)/rc_srcdest.o $(B)/rc_misc.o $(B)/rc_int.o \
          $(B)/rc_r8c.o gen_rename.sh
	NM=$(NM) sh gen_rename.sh $(filter %.o,$^) > $@.tmp && mv $@.tmp $@

$(B)/bp_%.o: $(SIM)/%.c $(RENAME) | $(B)
	$(CC) $(CFLAGS) -include $(RENAME) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/bp_r8c.o: $(GEN)/r8c.c $(RENAME) | $(B)
	$(CC) $(CFLAGS) -include $(RENAME) $(COREINC) $(WARNOFF) -c $< -o $@
$(B)/bp_hw.o: bp_hw.c bp.h cf.h norflash.h $(RENAME) $(SIM)/.stamp | $(B)
	$(CC) $(CFLAGS) -include $(RENAME) $(COREINC) $(WARNOFF) -c $< -o $@

# Programs.
$(B)/%.o: %.c bp.h rcglue.h cf.h norflash.h board.h mas.h card.h | $(B)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I$(LIBRC) -c $< -o $@
$(B)/mas.o: mas.c mas.h bp.h | $(B)
	$(CC) $(CFLAGS) $(MPG_CFLAGS) -c $< -o $@
$(B)/main.o: main.c bp.h board.h cf.h norflash.h card.h plat/clip.h | $(B)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@
# The clipboard's files: NSPasteboard on a Mac, CF_HDROP on Windows, SDL's
# text/uri-list elsewhere.
$(B)/clip.o: $(CLIPSRC) plat/clip.h | $(B)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(OUT)bprun$(EXE): $(B)/bprun.o $(B)/board.o $(B)/mas.o $(B)/card.o $(BPOBJ) $(RCOBJ) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ $(MPG_LIBS) -lm

$(OUT)bpemu$(EXE): $(B)/main.o $(B)/board.o $(B)/mas.o $(B)/card.o $(B)/clip.o $(BPOBJ) $(RCOBJ) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) $(MPG_LIBS) $(GUI_LIBS) -lm

$(OUT)bpcard$(EXE): $(B)/bpcard.o $(B)/card.o | $(OUT)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf build build-win build-mac dist bprun bpemu bpcard rename.h
