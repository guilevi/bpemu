/* board.h --- the Book Port and its RC8650, running together.  */

#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <stdint.h>

/* Boot both chips.  fwdir holds bpfw._BB; srec is the RC8650 image.  */
int board_init (const char *fwdir, const char *srec);

/* Power-cycle the Book Port, as taking its batteries out does: RAM (and
   so any settings not saved with D+E+F) is lost; the card, the external
   flash and the clock keep theirs.  */
void board_reset (void);

/* Advance both chips by this much emulated time.  */
void board_run_us (uint64_t us);

/* Audio produced so far, signed 16-bit mono at board_sample_rate().  */
size_t board_audio (int16_t *out, size_t max);
unsigned board_sample_rate (void);

/* Show what the Book Port sends the speech chip on stdout.  */
extern int board_echo_tx;

#endif
