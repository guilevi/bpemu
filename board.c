/* board.c --- couples the Book Port's M30245 to its RC8650.

   The two run in lock-step slices of SLICE_US.  The M30245's UART0 is the
   RC8650's serial port: bytes it transmits go to the chip's RXD, bytes the
   chip transmits come back on UART0's RXD, and the chip's CTS# is wired to
   P6_0, which the firmware polls before every byte it sends.  UART1 and
   P8_4 are the MP3 decoder's data and demand lines (mas.c).

   Audio: the RC8650's PCM comes out as the chip produces it.  While an MP3
   plays, output is instead paced by emulated time, with the decoder's
   samples and the speech mixed together.  */

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bp.h"
#include "mas.h"
#include "rcglue.h"

int board_echo_tx = 0;

/* Short enough that CTS# round trips do not throttle the 115200-baud link:
   a byte takes ~87 us on the wire.  */
#define SLICE_US 50

/* How far ahead of the chip the host may queue.  A real host waits for
   CTS# after every byte; the slice makes ours see it late, so allow a few
   bytes in flight.  */
#define IN_FLIGHT 8

static rc8650_t *rc;
static unsigned in_flight;

/* P10_2 drives the RC8650's reset: low holds it.  While held, and for the
   length of its boot after release, the chip is silent and CTS# is high.  */
static int rc_held = 1;
static uint64_t rc_boot_left;	/* chip crystal cycles of boot still to pass */

static int
rc_reset_pin (void)
{
  /* P10 = 0x3F4, PD10 = 0x3F6.  An input pin floats high: not reset.  */
  uint8_t pd = bp_peek (0x3F6), p = bp_peek (0x3F4);
  return (pd & 0x04) && !(p & 0x04);
}

static void
echo (uint8_t b)
{
  if (b == '\r')
    fputs ("\\r\n", stdout);
  else if (b < 0x20)
    printf ("^%c", b + '@');
  else if (b < 0x7F)
    putchar (b);
  else
    printf ("<%02x>", b);
  fflush (stdout);
}

static void
uart_tx (void *ctx, int ch, uint8_t b)
{
  (void) ctx;
  if (ch == 1)
    {
      mas_put (b);
      return;
    }
  if (ch != 0)
    {
      if (board_echo_tx)
	printf ("[U%d>%02x]", ch, b);
      return;
    }
  if (board_echo_tx)
    echo (b);
  if (rc_held || rc_boot_left)
    return;			/* lost on the wire: the chip is not listening */
  rcx_put (rc, b);
  in_flight++;
}

static int
rc_cts (void *ctx)
{
  (void) ctx;
  /* CTS# is active low.  */
  if (rc_held || rc_boot_left)
    return 1;
  return !(rcx_cts (rc) && in_flight < IN_FLIGHT);
}

static void
i2c_write (void *ctx, int dev, int idx, uint8_t b)
{
  (void) ctx;
  mas_i2c (dev, idx, b);
}

/* Output rings, at RCX_BOOKPORT_RATE.  speech: what the chip produced;
   mix: time-paced output while an MP3 is playing.  */
#define RING 65536
static int16_t speech[RING], mix[RING];
static size_t sp_h, sp_t, mx_h, mx_t;
static int mixing, speech_flowing;
static uint64_t out_frac;

static size_t
ring_fill (size_t h, size_t t)
{
  return (h - t + RING) % RING;
}

static void
audio_slice (uint64_t us)
{
  int16_t tmp[1024];
  size_t n;
  while ((n = rcx_pcm (rc, tmp, 1024)) > 0)
    for (size_t i = 0; i < n && ring_fill (sp_h, sp_t) < RING - 1; i++)
      {
	speech[sp_h] = tmp[i];
	sp_h = (sp_h + 1) % RING;
      }

  unsigned rate = rcx_sample_rate (rc);
  out_frac += us * rate;
  size_t due = out_frac / 1000000;
  out_frac %= 1000000;

  if (!mixing && !mas_active ())
    return;
  mixing = 1;
  while (due)
    {
      size_t k = due < 1024 ? due : 1024;
      due -= k;
      mas_render (tmp, k, rate);
      for (size_t i = 0; i < k; i++)
	{
	  /* Speech joins once 30 ms of it is queued, so the chip's bursts
	     do not leave gaps.  */
	  size_t q = ring_fill (sp_h, sp_t);
	  if (!q)
	    speech_flowing = 0;
	  else if (q >= rate / 33)
	    speech_flowing = 1;
	  int v = tmp[i];
	  if (speech_flowing)
	    {
	      v += speech[sp_t];
	      sp_t = (sp_t + 1) % RING;
	    }
	  if (ring_fill (mx_h, mx_t) < RING - 1)
	    {
	      mix[mx_h] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
	      mx_h = (mx_h + 1) % RING;
	    }
	}
    }
  if (!mas_active () && !ring_fill (sp_h, sp_t))
    mixing = 0;
}

const char *
board_find_srec (const char *fwdir)
{
  static char path[1024];
  snprintf (path, sizeof path, "%s/rc8650.srec", fwdir);
  return path;
}

int
board_init (const char *fwdir, const char *srec)
{
  char *err = NULL;

  rc = rcx_new (srec, &err);
  if (!rc)
    {
      fprintf (stderr, "RC8650: %s\n", err ? err : "failed to start");
      return -1;
    }
  rcx_bookport_output (rc);
  mas_init ();
  bp_hooks h = { uart_tx, rc_cts, i2c_write, NULL };
  if (bp_load (fwdir, &h) < 0)
    return -1;
  return 0;
}

void
board_reset (void)
{
  bp_reset ();
  mas_reset ();
  mixing = speech_flowing = 0;
  sp_h = sp_t = mx_h = mx_t = 0;
}

void
board_run_us (uint64_t us)
{
  static uint64_t bp_frac, rc_frac;

  while (us)
    {
      uint64_t s = us < SLICE_US ? us : SLICE_US;
      us -= s;

      /* The MAS asks for data only once UART1 is clock-synchronous with
	 its transmitter enabled (U1MR = 0x368, U1C1 = 0x36D).  */
      int u1 = (bp_peek (0x368) & 7) == 1 && (bp_peek (0x36D) & 1);
      bp_set_pins (8, 0x10, u1 && mas_demand () ? 0x10 : 0);
      bp_frac += s * BP_XIN_HZ;
      bp_run (bp_frac / 1000000);
      bp_frac %= 1000000;

      rc_frac += s * RCX_F1_HZ;
      uint64_t f1 = rc_frac / 1000000;
      rc_frac %= 1000000;

      int held = rc_reset_pin ();
      if (held && !rc_held)
	rc_held = 1;
      else if (!held && rc_held)
	{
	  rc_held = 0;
	  in_flight = 0;
	  rc_boot_left = rcx_reset (rc);
	  if (board_echo_tx)
	    printf ("[RC8650 reset released at %.3f, boot %.3f s]", (double) bp_cycles () / BP_XIN_HZ, (double) rc_boot_left / RCX_F1_HZ);
	}
      if (rc_held)
	{
	  audio_slice (s);
	  continue;
	}
      if (rc_boot_left)
	{
	  uint64_t d = f1 < rc_boot_left ? f1 : rc_boot_left;
	  rc_boot_left -= d;
	  f1 -= d;
	}
      rcx_run (rc, f1);

      if (rcx_ready (rc))
	in_flight = 0;

      uint8_t buf[64];
      size_t n = rcx_serial (rc, buf, sizeof buf);
      for (size_t i = 0; i < n; i++)
	{
	  if (board_echo_tx)
	    printf ("{%02x@%.3f}", buf[i], (double) bp_cycles () / BP_XIN_HZ);
	  bp_uart_rx (0, buf[i]);
	}
      audio_slice (s);
    }
}

size_t
board_audio (int16_t *out, size_t max)
{
  size_t n = 0;
  /* Whatever was mixed goes first; outside an MP3, speech as produced.  */
  while (n < max && ring_fill (mx_h, mx_t))
    {
      out[n++] = mix[mx_t];
      mx_t = (mx_t + 1) % RING;
    }
  while (!mixing && n < max && ring_fill (sp_h, sp_t))
    {
      out[n++] = speech[sp_t];
      sp_t = (sp_t + 1) % RING;
    }
  return n;
}

unsigned
board_sample_rate (void)
{
  return rcx_sample_rate (rc);
}
