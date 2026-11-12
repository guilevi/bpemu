# Book Port emulator.
#
# Two Renesas M16C CPUs run in one process, both on GDB's m32c simulator as
# patched in the doubletalk work: build/sim/m32c is the pristine gdb-14.2
# sim/m32c in vendor/gdb-sim-m32c with the librc8650 submodule's
# sim/rc865x-sim.patch applied, so the core always matches the library:
#
#   - the RC8650 speech chip, through librc8650 (vendor/librc8650), with the
#     core's original symbol names;
#   - the Book Port's own M30245, with the core recompiled under rename.h and
#     bp_hw.c in place of mem.c.

GDBSIM  := vendor/gdb-sim-m32c
LIBRC   := vendor/librc8650
SIM     := build/sim/m32c
GEN     := build/gen

CC      ?= cc
CFLAGS  ?= -O2 -g
WARNOFF := -Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
           -Wno-int-conversion -Wno-unused-result -Wno-deprecated-non-prototype
COREINC := -I$(LIBRC) -I$(SIM) -I$(GEN)

SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl3 2>/dev/null)
MPG_CFLAGS := $(shell pkg-config --cflags libmpg123 2>/dev/null)
MPG_LIBS   := $(shell pkg-config --libs libmpg123 2>/dev/null)

CORE    := reg srcdest misc int
# The RC8650: the whole patched core plus librc8650.
RCOBJ   := $(addprefix build/rc_,$(addsuffix .o,mem $(CORE) r8c m32c stubs \
             rcdict rcdict_regex rc_glue))
# The Book Port: the CPU half of the core, renamed, plus the board.
BPOBJ   := $(addprefix build/bp_,$(addsuffix .o,$(CORE) r8c)) build/bp_hw.o build/cf.o build/norflash.o

.PHONY: all clean
all: bprun bpemu cards/card.img state

# A card to start from: the firmware's update files, which a Book Port with
# an empty external flash installs at power-on (and then deletes).
UPDATE := fw/BPMsgs._BB fw/BPfwex._BB fw/Spanish.dix
cards/card.img: | $(UPDATE)
	@mkdir -p cards
	sh tools/mkcard.sh $@ 128 $(UPDATE)
state:
	@mkdir -p $@

build build/gen:
	@mkdir -p $@

$(LIBRC)/rc8650.c $(LIBRC)/sim/rc865x-sim.patch:
	@echo "$(LIBRC) is empty: run  git submodule update --init" >&2; exit 1

# The patched core.  The sources are touched after the stamp so the empty
# rule below never makes them look out of date.
SIMSRC  := $(addprefix $(SIM)/,mem.c reg.c srcdest.c misc.c int.c opc2c.c \
             r8c.opc m32c.opc)
$(SIM)/.stamp: $(LIBRC)/sim/rc865x-sim.patch $(wildcard $(GDBSIM)/*) | build
	rm -rf build/sim && mkdir -p build/sim && cp -R $(GDBSIM) $(SIM)
	cd build && patch -s -p1 < ../$(LIBRC)/sim/rc865x-sim.patch
	touch $@ && touch $(SIM)/*
$(SIMSRC): $(SIM)/.stamp ;

# opc2c turns the .opc instruction descriptions into the decoders.
build/opc2c: $(SIM)/opc2c.c | build
	$(CC) -O2 -w -I$(LIBRC) -o $@ $<
$(GEN)/%.c: $(SIM)/%.opc build/opc2c | build/gen
	build/opc2c -l $@.log $< > $@.tmp && mv $@.tmp $@

# RC8650 side.
build/rc_%.o: $(SIM)/%.c | build
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
build/rc_r8c.o: $(GEN)/r8c.c | build
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
build/rc_m32c.o: $(GEN)/m32c.c | build
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
build/rc_stubs.o: $(LIBRC)/stubs.c $(SIM)/.stamp | build
	$(CC) $(CFLAGS) $(COREINC) $(WARNOFF) -c $< -o $@
build/rc_rcdict.o: $(LIBRC)/rcdict/rcdict.c | build
	$(CC) $(CFLAGS) -I$(LIBRC)/rcdict -c $< -o $@
build/rc_rcdict_regex.o: $(LIBRC)/rcdict/rcdict_regex.c | build
	$(CC) $(CFLAGS) -w -I$(LIBRC)/rcdict -c $< -o $@
build/rc_rc_glue.o: rc_glue.c rcglue.h $(LIBRC)/rc8650.c $(SIM)/.stamp | build
	$(CC) $(CFLAGS) $(COREINC) -I. $(WARNOFF) -c $< -o $@

# Book Port side.  rename.h comes from the RC copies' symbol tables.
rename.h: build/rc_reg.o build/rc_srcdest.o build/rc_misc.o build/rc_int.o \
          build/rc_r8c.o gen_rename.sh
	sh gen_rename.sh $(filter %.o,$^) > $@.tmp && mv $@.tmp $@

build/bp_%.o: $(SIM)/%.c rename.h | build
	$(CC) $(CFLAGS) -include rename.h $(COREINC) $(WARNOFF) -c $< -o $@
build/bp_r8c.o: $(GEN)/r8c.c rename.h | build
	$(CC) $(CFLAGS) -include rename.h $(COREINC) $(WARNOFF) -c $< -o $@
build/bp_hw.o: bp_hw.c bp.h cf.h norflash.h rename.h $(SIM)/.stamp | build
	$(CC) $(CFLAGS) -include rename.h $(COREINC) $(WARNOFF) -c $< -o $@

# Programs.
build/%.o: %.c bp.h rcglue.h cf.h norflash.h board.h mas.h cardadd.h | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I$(LIBRC) -c $< -o $@

build/mas.o: mas.c mas.h | build
	$(CC) $(CFLAGS) $(MPG_CFLAGS) -c $< -o $@

bprun: build/bprun.o build/board.o build/mas.o build/cardadd.o $(BPOBJ) $(RCOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(MPG_LIBS) -lm

bpemu: build/main.o build/board.o build/mas.o build/cardadd.o $(BPOBJ) $(RCOBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) $(MPG_LIBS) -lm

build/main.o: main.c bp.h board.h cf.h norflash.h cardadd.h | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

clean:
	rm -rf build bprun bpemu rename.h
