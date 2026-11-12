/* mas.c --- the Book Port's MP3 path: a Micronas MAS3507D decoder fed over
   UART1, into a DAC3550A D/A converter.

   As the firmware drives it (bpfw 2.3): UART1 runs clock-synchronous and
   carries the raw MP3 bitstream into the MAS's serial input.  The MAS's
   demand line is P8_4: while it is high the firmware's pump (0xE1BE0, also
   the UART1 transmit interrupt) copies bytes from its two 4 KB buffers at
   0xC808/0xD808 into U1TB, stopping when it drops; its rising edge raises
   interrupt 0x5E (vector 30) to start the pump again.  The DAC3550A's
   AVOL register (subaddress 0xC2, left then right, 0x2C at the default
   volume of 44) sets the level, and is zeroed while a file starts.

   Decoding is libmpg123's.  Bytes are kept here and handed to it only as
   playback needs them, so the demand line tracks real consumption.  */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <mpg123.h>

#include "bp.h"
#include "mas.h"

#define FIFO_CAP 16384
#define DEMAND_BELOW 512	/* bytes buffered under which the MAS asks;
				   below the pump's 1 KB burst, so it ends on a
				   falling edge and the next rise restarts it */
#define PCM_CAP 16384

static mpg123_handle *mh;
static uint8_t fifo[FIFO_CAP];
static size_t fhead, ftail;	/* ring: head = write, tail = read */

static int16_t pcm[PCM_CAP];	/* decoded mono samples at src_rate */
static size_t pcm_n, pcm_pos;
static long src_rate = 44100;
static double phase;		/* fractional position between pcm_pos and +1 */

static int avol = 0x2C;
static uint64_t idle_samples;	/* output samples since the last decoded one */
static int started;

int mas_log = 0;
static unsigned long mas_bytes_in, out_count, played;
static unsigned chunk;		/* bytes taken since demand last rose */
static int demand;

static size_t
fifo_fill (void)
{
  return (fhead - ftail + FIFO_CAP) % FIFO_CAP;
}

static void
open_feed (void)
{
  if (mh)
    {
      mpg123_close (mh);
      mpg123_delete (mh);
    }
  mh = mpg123_new (NULL, NULL);
  if (!mh)
    return;
  mpg123_param (mh, MPG123_FLAGS, MPG123_MONO_MIX | MPG123_QUIET, 0);
  mpg123_param (mh, MPG123_ADD_FLAGS, MPG123_FORCE_MONO, 0);
  mpg123_format_none (mh);
  static const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000,
				32000, 44100, 48000 };
  for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++)
    mpg123_format (mh, rates[i], MPG123_MONO, MPG123_ENC_SIGNED_16);
  mpg123_open_feed (mh);
}

void
mas_init (void)
{
  static int lib;
  if (!lib)
    {
      mas_log = getenv ("BP_MASLOG") != NULL;
      mpg123_init ();
      lib = 1;
    }
  mas_reset ();
}

void
mas_reset (void)
{
  fhead = ftail = 0;
  demand = 0;
  chunk = 0;
  pcm_n = pcm_pos = 0;
  phase = 0;
  started = 0;
  open_feed ();
}

void
mas_put (uint8_t b)
{
  if (fifo_fill () == FIFO_CAP - 1)
    return;			/* overrun: the real chip would drop it too */
  static unsigned long total;
  mas_bytes_in++;
  chunk++;
  fifo[fhead] = b;
  fhead = (fhead + 1) % FIFO_CAP;
  if (!started && mas_log)
    fprintf (stderr, "MAS: data starts\n");
  started = 1;
}

/* The line rises while the input buffer is low and drops once a chunk of
   DEMAND_BELOW bytes has come in (or the buffer is full again), so every
   chunk the firmware sends ends on a falling edge and the next request is
   a fresh rising one: its pump only restarts on an edge.  Without the
   chunk limit, a decoder that swallows bytes while it looks for a frame
   (as when playback resumes mid-file) would hold the line high and the
   pump would never run again.  */
int
mas_demand (void)
{
  if (demand)
    {
      if (fifo_fill () >= DEMAND_BELOW || chunk >= DEMAND_BELOW)
	demand = 0;
    }
  else if (fifo_fill () < DEMAND_BELOW)
    {
      demand = 1;
      chunk = 0;
    }
  return demand;
}

void
mas_i2c (int dev, int idx, uint8_t b)
{
  static uint8_t sub, lr;
  if (mas_log)
    fprintf (stderr, "I2C dev %02x [%d] %02x\n", dev, idx, b);
  if (dev != 0x4D)		/* the DAC3550A */
    return;
  if (idx == 0)
    {
      sub = b;
      lr = 0;
      return;
    }
  if (sub == 0xC2 && lr++ == 0)
    avol = b & 0x3F;
}

/* Refill pcm[] from the decoder.  Returns 0 when it needs bytes we lack.  */
static int
decode_more (void)
{
  if (!mh)
    return 0;
  for (;;)
    {
      size_t got = 0;
      if (pcm_pos > 0)
	{
	  memmove (pcm, pcm + pcm_pos, (pcm_n - pcm_pos) * sizeof pcm[0]);
	  pcm_n -= pcm_pos;
	  pcm_pos = 0;
	}
      int r = mpg123_read (mh, (unsigned char *) (pcm + pcm_n),
			   (PCM_CAP - pcm_n) * sizeof pcm[0], &got);
      pcm_n += got / sizeof pcm[0];
      if (r == MPG123_NEW_FORMAT)
	{
	  int ch, enc;
	  mpg123_getformat (mh, &src_rate, &ch, &enc);
	  if (mas_log)
	    fprintf (stderr, "MAS: %ld Hz\n", src_rate);
	  continue;
	}
      if (got)
	return 1;
      if (r == MPG123_NEED_MORE || r == MPG123_OK || r == MPG123_DONE)
	{
	  size_t n = fifo_fill ();
	  if (!n)
	    return 0;
	  if (n > 256)
	    n = 256;
	  uint8_t tmp[1024];
	  for (size_t i = 0; i < n; i++)
	    {
	      tmp[i] = fifo[ftail];
	      ftail = (ftail + 1) % FIFO_CAP;
	    }
	  mpg123_feed (mh, tmp, n);
	  continue;
	}
      /* A decoding error: resynchronise on what follows.  */
      open_feed ();
      return 0;
    }
}

int
mas_render (int16_t *out, size_t n, unsigned rate)
{
  double step = (double) src_rate / rate;
  double gain = avol ? pow (10.0, (avol - 0x2C) * 1.5 / 20.0) : 0.0;
  int any = 0;

  for (size_t i = 0; i < n; i++)
    {
      if (pcm_pos + 1 >= pcm_n && !decode_more ())
	{
	  out[i] = 0;
	  idle_samples++;
	  continue;
	}
      double a = pcm[pcm_pos], b = pcm[pcm_pos + 1];
      double v = (a + (b - a) * phase) * gain;
      out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) v;
      phase += step;
      while (phase >= 1.0)
	{
	  phase -= 1.0;
	  pcm_pos++;
	}
      idle_samples = 0;
      played++;
      any = 1;
    }
  out_count += n;
  if (mas_log && out_count >= rate)
    {
      out_count -= rate;
      fprintf (stderr, "MAS t=%.2f: %lu bytes in, %zu buffered, %.2f s "
	       "played, avol %02x\n", (double) bp_cycles () / BP_XIN_HZ,
	       mas_bytes_in, fifo_fill (),
	       (double) played / rate, avol);
    }
  return any;
}

int
mas_active (void)
{
  /* Playing, or stopped for less than half a second.  */
  return started && (fifo_fill () || pcm_n > pcm_pos + 1
		     || idle_samples < 24000);
}
