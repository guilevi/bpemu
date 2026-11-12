/* mas.h --- MAS3507D MP3 decoder and DAC3550A, as the Book Port uses them.  */

#ifndef MAS_H
#define MAS_H

#include <stddef.h>
#include <stdint.h>

void mas_init (void);
void mas_reset (void);		/* drop buffered data (the stream stopped) */

/* A byte of MP3 bitstream from UART1.  */
void mas_put (uint8_t b);

/* Level of the demand line (P8_4): non-zero when the MAS wants data.  */
int mas_demand (void);

/* A byte written over I2C to dev (7-bit); idx 0 is the subaddress.  */
void mas_i2c (int dev, int idx, uint8_t b);

/* Produce n samples at rate.  Returns non-zero if any were decoded.  */
int mas_render (int16_t *out, size_t n, unsigned rate);

/* Non-zero while audio is playing or has just stopped.  */
int mas_active (void);

extern int mas_log;

#endif
