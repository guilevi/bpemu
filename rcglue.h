/* rcglue.h --- the RC8650 as a device on a serial line.

   librc8650's public API is shaped for a text-to-speech caller: it runs the
   chip only as far as an utterance needs, and interprets what the chip sends
   back.  A host CPU on the other end of the wire needs the chip to keep time
   with it instead, and to see every byte.  These calls give that, built over
   the same engine (rc_glue.c compiles rc8650.c into itself to reach its
   internals).  */

#ifndef RCGLUE_H
#define RCGLUE_H

#include <stddef.h>
#include <stdint.h>

#include "rc8650.h"

/* The chip's main crystal, the clock rcx_run is measured in.  */
#define RCX_F1_HZ 7372800u

/* Boot the chip; NULL with *err set on failure.  */
rc8650_t *rcx_new (const char *srec_path, char **err);

/* Cold-boot the chip, as releasing its reset pin does.  Returns the crystal
   cycles the boot took, which the caller should let pass before expecting
   the chip to answer.  */
uint64_t rcx_reset (rc8650_t *e);

/* A byte arriving on the chip's RXD.  */
void rcx_put (rc8650_t *e, uint8_t b);

/* Non-zero when the chip's CTS# is asserted and it has taken everything it
   was given: a host may send the next byte.  */
int rcx_ready (rc8650_t *e);

/* Level of the chip's CTS# pin alone: non-zero when asserted.  */
int rcx_cts (rc8650_t *e);

/* Run the chip for this many crystal cycles.  */
void rcx_run (rc8650_t *e, uint64_t f1_cycles);

/* Bytes the chip has transmitted since the last call.  */
size_t rcx_serial (rc8650_t *e, uint8_t *buf, size_t max);

/* The output rate of the BookPort mode below.  */
#define RCX_BOOKPORT_RATE 48000u

/* Give rcx_pcm the output stage measured from a real BookPort: the D/A's
   sample-and-hold staircase, images and all, at 10,488 Hz under a
   first-order 5 kHz low-pass, delivered at RCX_BOOKPORT_RATE.  Survives
   rcx_reset.  */
void rcx_bookport_output (rc8650_t *e);

/* The rate rcx_pcm delivers: rc8650_sample_rate() until
   rcx_bookport_output, RCX_BOOKPORT_RATE after.  */
unsigned rcx_sample_rate (rc8650_t *e);

/* Audio produced since the last call: signed 16-bit at rcx_sample_rate(),
   through the modelled output stage.  */
size_t rcx_pcm (rc8650_t *e, int16_t *out, size_t max);

#endif
