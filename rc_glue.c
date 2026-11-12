/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rc_glue.c --- see rcglue.h.  Includes librc8650's implementation so the
   raw-device calls can use its engine state, rings and output filter.  */

#include "rc8650.c"
#include "rcglue.h"

rc8650_t *
rcx_new (const char *srec_path, char **err)
{
  return rc8650_new (srec_path, err);
}

uint64_t
rcx_reset (rc8650_t *e)
{
  /* Booting zeroes the simulator's clock, so what it reads afterwards is
     the boot's own length.  */
  rc8650_reset (e);
  return m32c_f1_clock;
}

void
rcx_put (rc8650_t *e, uint8_t b)
{
  in_put (e, b);
}

int
rcx_ready (rc8650_t *e)
{
  return chip_ready () && RING_EMPTY (e, in);
}

int
rcx_cts (rc8650_t *e)
{
  (void) e;
  return chip_ready ();
}

void
rcx_run (rc8650_t *e, uint64_t f1_cycles)
{
  unsigned long long end = m32c_f1_clock + f1_cycles;
  while (m32c_f1_clock < end)
    step (e);
}

size_t
rcx_serial (rc8650_t *e, uint8_t *buf, size_t max)
{
  return ring_get (e->ser, SER_CAP, e->ser_head, &e->ser_tail, buf, max);
}

void
rcx_bookport_output (rc8650_t *e)
{
  /* librc8650's measured BookPort output stage (README, "The output
     stage"): the D/A held as a staircase at 10,488 Hz, rendered at 48 kHz,
     under a first-order 5 kHz low-pass.  The order of the calls follows
     rc8650say --bookport: the corner last, designed at the output rate.  */
  rc8650_set_lowpass_order (e, 1);
  rc8650_set_output_rate (e, RCX_BOOKPORT_RATE);
  rc8650_set_dac_rate (e, 10488);
  rc8650_set_lowpass_hz (e, 5000);
}

unsigned
rcx_sample_rate (rc8650_t *e)
{
  return rc8650_get_output_rate (e);
}

size_t
rcx_pcm (rc8650_t *e, int16_t *out, size_t max)
{
  uint8_t tmp[1024];
  size_t total = 0;

  if (e->out_rate)
    {
      /* The staircase path of rc8650_read_s16, fed from the ring the chip
	 has already filled rather than by running it.  Levels a full buffer
	 cannot take wait in the library's own queue for the next call, so
	 pull only as many chip samples as are sure to fit.  */
      double per = (double) e->out_rate / e->dac_rate;
      double per_in = (double) e->out_rate / RC8650_SAMPLE_RATE;

      while (total < max)
	{
	  size_t want, n;
	  double room;

	  total = stair_hold (e, out, total, max);
	  if (total >= max)
	    break;
	  room = (double) (max - total) - per - 2.0;
	  want = room > per_in ? (size_t) (room / per_in) : 1;
	  if (want > sizeof tmp)
	    want = sizeof tmp;
	  n = ring_get (e->pcm, PCM_CAP, e->pcm_head, &e->pcm_tail, tmp, want);
	  for (size_t i = 0; i < n; i++)
	    {
	      stair_push (e, (tmp[i] - 128) * 256.0 * OUT_GAIN);
	      total = stair_hold (e, out, total, max);
	    }
	  e->samples_read += n;
	  if (n < want)
	    break;
	}
      return total;
    }

  while (total < max)
    {
      size_t want = max - total, n;
      if (want > sizeof tmp)
	want = sizeof tmp;
      n = ring_get (e->pcm, PCM_CAP, e->pcm_head, &e->pcm_tail, tmp, want);
      for (size_t i = 0; i < n; i++)
	out[total + i] = out_sample (e, tmp[i]);
      total += n;
      e->samples_read += n;
      if (n < want)
	break;
    }
  return total;
}
