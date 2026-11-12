/* bp_hw.c --- memory, on-chip peripherals and board for the Book Port.

   The CPU is a Renesas M30245 (M16C/60 core with a USB function block):
   128 KB of flash at 0xE0000-0xFFFFF, internal RAM from 0x400, SFRs below
   that, and external RAM on the bus up to 0xFFFF.  This file supplies the
   mem_* interface the GDB core calls (under rename.h), the timers, UARTs,
   DMA, A/D, ports and interrupt controller, and steps the core.

   Register addresses are from the M30245 Group hardware manual
   (REJ03B0005-0200), SFR map tables 1.6-1.13.  */

#include "rename.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "mem.h"
#include "int.h"
#include "bp.h"
#include "cf.h"
#include "norflash.h"

int bp_log = 0;

/* ------------------------------------------------------------------------ */
/* Things the core expects from the parts of the simulator we do not link.  */

int (*decode_opcode) (void) = 0;
int m32c_use_raw_console = 0;

int
decode_m32c (void)
{
  abort ();
}

void
m32c_sim_restore_console (void)
{
}

void
mem_usage_stats (void)
{
}

int
sign_ext (int v, int bits)
{
  if (bits < 32)
    {
      v &= (1 << bits) - 1;
      if (v & (1 << (bits - 1)))
	v -= (1 << bits);
    }
  return v;
}

/* ------------------------------------------------------------------------ */
/* State.  */

#define MEMSIZE (1 << 20)
#define ROM_BASE 0xE0000
#define CF_BASE 0x06000		/* CompactFlash window, 2 KB */
/* The external flash answers 0x30000-0xAFFFF; the chip decodes A0-A18.  */
#define NOR_LO 0x30000
#define NOR_HI 0xB0000

static uint8_t ram[MEMSIZE];
static uint8_t sfr[0x400];
static bp_hooks hooks;

static uint64_t cyc;		/* main-crystal cycles since reset */
static int waiting;		/* executed WAIT, idle until an interrupt */
static uint32_t cur_pc;		/* PC of the instruction being executed */

/* Port input levels driven by the board.  Index = port number.  */
static uint8_t port_in[11];

/* Keypad matrix: rows P7_4-P7_7 (driven low one at a time by the scanner,
   all low while idle), columns P10_3-P10_7 (key-input pins KI3-KI7, pulled
   up).  Position = row * 5 + column.  */
static uint8_t matrix[BP_MATRIX];
static uint8_t ki_prev = 0xF8;

/* SFR addresses.  */
enum
{
  PM0 = 0x04, PM1 = 0x05, CM0 = 0x06, CM1 = 0x07, CSR = 0x08, PRCR = 0x0A,
  USBC = 0x0C, WDTS = 0x0E, WDC = 0x0F, CSE = 0x1B, USBAD = 0x1F,
  KUPIC = 0x41, ADIC = 0x4B,
  USBIS = 0x284, USBIC = 0x286, USBIE = 0x288,
  FMR0 = 0x2F7,
  IFSR = 0x35F,
  TABSR = 0x380, CPSRF = 0x381, ONSF = 0x382, TRGSR = 0x383, UDF = 0x384,
  AD0 = 0x3C0, ADCON2 = 0x3D4, ADCON0 = 0x3D6, ADCON1 = 0x3D7,
  FSCCR = 0x3DB, FSC = 0x3DC,
  KUPM = 0x3F9,
};

/* Port data/direction register addresses, by port number.  */
static const int port_addr[11] =
  { 0x3E0, 0x3E1, 0x3E4, 0x3E5, 0x3E8, 0x3E9, 0x3EC, 0x3ED, 0x3F0, 0x3F1, 0x3F4 };
static const int pdir_addr[11] =
  { 0x3E2, 0x3E3, 0x3E6, 0x3E7, 0x3EA, 0x3EB, 0x3EE, 0x3EF, 0x3F2, 0x3F3, 0x3F6 };

static void
irq (int icaddr)
{
  sfr[icaddr] |= 0x08;
}

/* ------------------------------------------------------------------------ */
/* Timers A0-A4.  The M30245 has no timer B.  */

typedef struct
{
  int addr;			/* counter register */
  int mr;			/* mode register */
  int ic;			/* interrupt control register */
  int32_t count;
  uint16_t reload;
  uint32_t prescale;		/* Xin cycles accumulated toward one count */
  int running;			/* one-shot armed */
} timer_t_;

static timer_t_ tmr[5] = {
  {0x386, 0x396, 0x54}, {0x388, 0x397, 0x45}, {0x38A, 0x398, 0x47},
  {0x38C, 0x399, 0x57}, {0x38E, 0x39A, 0x59},
};

static uint32_t
count_source_div (int mr)
{
  switch (mr >> 6)
    {
    case 0: return 1;
    case 1: return 8;
    case 2: return 32;
    default: return BP_XIN_HZ / 1024;	/* fc32: 32.768 kHz / 32 */
    }
}

static int
timer_started (int i)
{
  return (sfr[TABSR] >> i) & 1;
}

static void timer_underflow (int i);

/* An event-counter timer counts underflows of another timer.  */
static void
timer_event (int i)
{
  timer_t_ *t = &tmr[i];
  if (!timer_started (i))
    return;
  if (--t->count < 0)
    {
      t->count = t->reload;
      timer_underflow (i);
    }
}

static void
timer_underflow (int i)
{
  timer_t_ *t = &tmr[i];

  irq (t->ic);
  if ((sfr[t->mr] & 3) == 2)	/* one-shot: stop */
    t->running = 0;

  /* TAj in event-counter mode counts what TRGSR selects for it; 3 = the
     underflow of TA(j-1).  TA0's source (TA4) is in TRGSR too, but no
     firmware path needs it.  */
  for (int a = 1; a < 5; a++)
    {
      int sel = (sfr[TRGSR] >> ((a - 1) * 2)) & 3;
      if ((sfr[tmr[a].mr] & 3) == 1 && sel == 3 && i == a - 1)
	timer_event (a);
    }
}

static void
timers_tick (uint32_t dc)
{
  for (int i = 0; i < 5; i++)
    {
      timer_t_ *t = &tmr[i];
      int mode = sfr[t->mr] & 3;
      if (!timer_started (i) || mode == 1)
	continue;
      if (mode == 2 && !t->running)
	continue;
      uint32_t div = count_source_div (sfr[t->mr]);
      t->prescale += dc;
      while (t->prescale >= div)
	{
	  t->prescale -= div;
	  if (--t->count < 0)
	    {
	      t->count = t->reload;
	      timer_underflow (i);
	    }
	}
    }
}

static int
timer_index (int addr)
{
  for (int i = 0; i < 5; i++)
    if (addr == tmr[i].addr || addr == tmr[i].addr + 1)
      return i;
  return -1;
}

/* ------------------------------------------------------------------------ */
/* UARTs 0-3.  Each block is MR, BRG, TB(2), C0, C1, RB(2) from base.  */

typedef struct
{
  int base;			/* UiMR */
  int tic, ric;
  int tb_full;			/* TB holds a byte not yet moved to the shifter */
  uint8_t tb;
  int shifting;			/* shift register busy */
  uint8_t shift;
  uint64_t done_at;		/* cycle the shift register empties */
  uint8_t rxq[4096];
  int rxh, rxt;
} uart_t;

static uart_t uart[4] = {
  {0x3A8, 0x53, 0x4A}, {0x368, 0x51, 0x48},
  {0x338, 0x4F, 0x42}, {0x328, 0x4D, 0x55},
};

static uint64_t
uart_frame_cycles (uart_t *u)
{
  int mr = sfr[u->base];
  int brg = sfr[u->base + 1];
  static const int clkdiv[4] = { 1, 8, 32, 32 };
  int div = clkdiv[sfr[u->base + 4] & 3];
  int smd = mr & 7;
  if (smd == 1 || smd == 2)	/* clock synchronous, I2C */
    return (uint64_t) div * 2 * (brg + 1) * 9;
  int bits = 1 + (smd == 4 ? 7 : smd == 6 ? 9 : 8)
    + ((mr & 0x40) ? 1 : 0) + ((mr & 0x10) ? 2 : 1);
  return (uint64_t) div * 16 * (brg + 1) * bits;
}

static void
uart_start_shift (int ch)
{
  uart_t *u = &uart[ch];
  if (u->shifting || !u->tb_full)
    return;
  u->shift = u->tb;
  u->tb_full = 0;
  u->shifting = 1;
  u->done_at = cyc + uart_frame_cycles (u);
  sfr[u->base + 5] |= 0x02;	/* TI: TB empty */
  sfr[u->base + 4] &= ~0x08;	/* TXEPT clear */
  if (!(sfr[u->base + 5] & 0x10))	/* UiIRS = 0: interrupt on TB empty */
    irq (u->tic);
}

static void
uart_rx_deliver (int ch)
{
  uart_t *u = &uart[ch];
  if (u->rxh == u->rxt)
    return;
  if (!(sfr[u->base + 5] & 0x04))	/* RE */
    return;
  if (sfr[u->base + 5] & 0x08)	/* RI still set: wait for a read */
    return;
  sfr[u->base + 6] = u->rxq[u->rxh];
  sfr[u->base + 7] = 0;
  u->rxh = (u->rxh + 1) % sizeof u->rxq;
  sfr[u->base + 5] |= 0x08;
  irq (u->ric);
}

static void
uart_tick (void)
{
  for (int ch = 0; ch < 4; ch++)
    {
      uart_t *u = &uart[ch];
      if (u->shifting && cyc >= u->done_at)
	{
	  u->shifting = 0;
	  if (bp_log & BP_LOG_UART)
	    fprintf (stderr, "%06x UART%d TX %02x\n", cur_pc, ch, u->shift);
	  if (hooks.uart_tx)
	    hooks.uart_tx (hooks.ctx, ch, u->shift);
	  if (sfr[u->base + 5] & 0x10)	/* UiIRS = 1: on transmit complete */
	    irq (u->tic);
	  if (u->tb_full)
	    uart_start_shift (ch);
	  else
	    sfr[u->base + 4] |= 0x08;	/* TXEPT */
	}
      uart_rx_deliver (ch);
    }
}

/* ------------------------------------------------------------------------ */
/* UART2 in I2C mode (U2MR = 2, U2SMR IICM = 1).  The bus carries the audio
   chips: a Micronas DAC3550A at 0x4D and a MAS3507D MP3 decoder at 0x1D.

   The firmware asks for a start or stop through U2SMR4 (STAREQ/RSTAREQ/
   STPREQ with STSPSEL) and is told the condition happened by the
   start/stop-detect interrupt, S02BCNIC; U2SMR's BBS bit says which.  Each
   byte written to U2TB is clocked out with its 9th bit (the ACK the master
   sends when reading), and the level on that 9th clock picks the interrupt:
   low (ACK) raises S2RIC, high (NACK) S2TIC.  U2RB holds what was on SDA.  */

enum { U2SMR4 = 0x334, U2SMR = 0x337, U2TB = 0x33A, U2RB = 0x33E,
       S02BCNIC = 0x49, S2TIC = 0x4F, S2RIC = 0x42 };

static struct
{
  int phase;			/* 0 address next, 1 writing, 2 reading */
  int dev;			/* 7-bit address of the selected device */
  int xfer;			/* a byte is being clocked */
  uint64_t xfer_at;
  int cond;			/* 1 start, 2 stop pending */
  int idx;			/* bytes written since the address */
  uint64_t cond_at;
} i2c;

static int
i2c_mode (void)
{
  return (sfr[0x338] & 7) == 2 && (sfr[U2SMR] & 1);
}

/* The real-time clock at 0x68: a Dallas DS1337/DS1339-style register file
   (the firmware moves 0x00-0x0F in one burst; 0x0F bit 7 is OSF).
   Registers 0-6 are BCD seconds, minutes, hours, weekday, date, month,
   year; the rest are kept as written.  Time runs from the host clock at
   power-on, then with emulated time, and setting it moves the base.  */
static uint8_t rtc_reg[64];
static int rtc_ptr, rtc_ptr_set;
static long long rtc_base;	/* epoch seconds at cycle 0 */

static int bcd (int v) { return (v / 10) << 4 | (v % 10); }
static int unbcd (int v) { return (v >> 4) * 10 + (v & 15); }

static time_t
rtc_now (void)
{
  return (time_t) (rtc_base + (long long) (cyc / BP_XIN_HZ));
}

static void
rtc_latch (void)
{
  time_t t = rtc_now ();
  struct tm tm;
  localtime_r (&t, &tm);
  rtc_reg[0] = bcd (tm.tm_sec);
  rtc_reg[1] = bcd (tm.tm_min);
  rtc_reg[2] = bcd (tm.tm_hour);
  rtc_reg[3] = tm.tm_wday + 1;
  rtc_reg[4] = bcd (tm.tm_mday);
  rtc_reg[5] = bcd (tm.tm_mon + 1);
  rtc_reg[6] = bcd (tm.tm_year % 100);
}

static void
rtc_store (int r, uint8_t v)
{
  if (r > 6)
    {
      rtc_reg[r & 63] = v;
      return;
    }
  rtc_latch ();
  rtc_reg[r] = v;
  struct tm tm = { 0 };
  tm.tm_sec = unbcd (rtc_reg[0] & 0x7F);
  tm.tm_min = unbcd (rtc_reg[1] & 0x7F);
  tm.tm_hour = unbcd (rtc_reg[2] & 0x3F);
  tm.tm_mday = unbcd (rtc_reg[4] & 0x3F);
  tm.tm_mon = unbcd (rtc_reg[5] & 0x1F) - 1;
  tm.tm_year = unbcd (rtc_reg[6]) + 100;
  tm.tm_isdst = -1;
  rtc_base = (long long) mktime (&tm) - (long long) (cyc / BP_XIN_HZ);
}

static int
i2c_present (int dev)
{
  return dev == 0x4D || dev == 0x1D || dev == 0x68;
}

/* A byte the master wrote to the selected device.  */
static void
i2c_dev_write (int dev, uint8_t b)
{
  if (dev != 0x68)
    {
      if (hooks.i2c_write)
	hooks.i2c_write (hooks.ctx, dev, i2c.idx, b);
      i2c.idx++;
      return;
    }
  if (!rtc_ptr_set)
    {
      rtc_ptr = b & 63;
      rtc_ptr_set = 1;
      return;
    }
  rtc_store (rtc_ptr, b);
  rtc_ptr = (rtc_ptr + 1) & 63;
}

static uint8_t
i2c_dev_read (int dev)
{
  if (dev != 0x68)
    return 0x00;
  if (rtc_ptr <= 6)
    rtc_latch ();
  uint8_t v = rtc_reg[rtc_ptr];
  rtc_ptr = (rtc_ptr + 1) & 63;
  return v;
}

static void
i2c_byte_done (void)
{
  uint8_t out = sfr[U2TB];
  int bit9 = sfr[U2TB + 1] & 1;
  uint8_t sda;
  int ack;

  switch (i2c.phase)
    {
    case 0:
      i2c.dev = out >> 1;
      i2c.phase = (out & 1) ? 2 : 1;
      ack = i2c_present (i2c.dev);
      if (i2c.phase == 1)
	rtc_ptr_set = 0;	/* a write starts with the register pointer */
      i2c.idx = 0;
      sda = out;
      break;
    case 1:
      ack = i2c_present (i2c.dev);
      i2c_dev_write (i2c.dev, out);
      sda = out;
      break;
    default:
      /* The device drives SDA; the master's own bit 9 is the ACK.  */
      sda = i2c_dev_read (i2c.dev);
      ack = !bit9;
      break;
    }
  if (bp_log & BP_LOG_UART)
    fprintf (stderr, "%06x I2C %s %02x dev %02x %s\n", cur_pc,
	     i2c.phase == 2 ? "rd" : "wr", i2c.phase == 2 ? sda : out, i2c.dev,
	     ack ? "ACK" : "NACK");
  sfr[U2RB] = sda;
  sfr[U2RB + 1] = ack ? 0 : 1;
  sfr[0x33D] |= 0x0A;		/* TI, RI */
  irq (ack ? S2RIC : S2TIC);
}

static void
i2c_tick (void)
{
  if (i2c.xfer && cyc >= i2c.xfer_at)
    {
      i2c.xfer = 0;
      i2c_byte_done ();
    }
  if (i2c.cond && cyc >= i2c.cond_at)
    {
      if (i2c.cond == 1)
	{
	  sfr[U2SMR] |= 0x04;	/* BBS */
	  i2c.phase = 0;
	}
      else
	sfr[U2SMR] &= ~0x04;
      i2c.cond = 0;
      irq (S02BCNIC);
    }
}

static void
i2c_smr4_write (uint8_t v)
{
  sfr[U2SMR4] = v;
  if (!(v & 0x08))		/* STSPSEL: conditions come from the requests */
    return;
  if (v & 0x03)
    i2c.cond = 1;
  else if (v & 0x04)
    i2c.cond = 2;
  else
    return;
  i2c.cond_at = cyc + uart_frame_cycles (&uart[2]) / 9;
}

void
bp_uart_rx (int ch, uint8_t b)
{
  uart_t *u = &uart[ch];
  int n = (u->rxt + 1) % sizeof u->rxq;
  if (n == u->rxh)
    return;
  u->rxq[u->rxt] = b;
  u->rxt = n;
}

static int
uart_reg (int a, int *off)
{
  for (int ch = 0; ch < 4; ch++)
    if (a >= uart[ch].base && a < uart[ch].base + 8)
      {
	*off = a - uart[ch].base;
	return ch;
      }
  return -1;
}

/* ------------------------------------------------------------------------ */
/* Logging helpers.  */

static uint8_t sfr_seen[0x400];

static void
log_sfr (int addr, int val, const char *what)
{
  if (!(bp_log & BP_LOG_SFR))
    return;
  fprintf (stderr, "%06x SFR %s %03x = %02x\n", cur_pc, what, addr, val);
}

static uint8_t ext_seen[MEMSIZE / 8];

/* BP_WATCH=addr,addr,... (hex): log every write to those bytes.
   BP_RWATCH=...: the same for reads.  lo-hi gives a range.  */
static uint8_t watch[MEMSIZE / 8], rwatch[MEMSIZE / 8];
static int watch_init;

static void
watch_parse (uint8_t *set, const char *s)
{
  while (s && *s)
    {
      char *e;
      unsigned long lo = strtoul (s, &e, 16), hi = lo;
      if (*e == '-')
	hi = strtoul (e + 1, &e, 16);
      for (unsigned long w = lo; w <= hi && w < MEMSIZE; w++)
	set[w >> 3] |= 1 << (w & 7);
      s = *e ? e + 1 : e;
    }
}

static void
log_watch (int a, uint8_t v, int write)
{
  if (!watch_init)
    {
      watch_init = 1;
      watch_parse (watch, getenv ("BP_WATCH"));
      watch_parse (rwatch, getenv ("BP_RWATCH"));
    }
  if ((write ? watch : rwatch)[a >> 3] & (1 << (a & 7)))
    {
      if (write)
	fprintf (stderr, "%06x %.4f WATCH %05x <= %02x (was %02x)\n", cur_pc,
		 (double) cyc / BP_XIN_HZ, a, v, ram[a]);
      else
	fprintf (stderr, "%06x %.4f RWATCH %05x => %02x\n", cur_pc,
		 (double) cyc / BP_XIN_HZ, a, v);
    }
}

static void
log_ext (int addr, int val, int write)
{
  if (!(bp_log & BP_LOG_EXT))
    return;
  if ((addr & 0xFF800) != CF_BASE && (ext_seen[addr >> 3] & (1 << (addr & 7))))
    return;
  ext_seen[addr >> 3] |= 1 << (addr & 7);
  fprintf (stderr, "%06x EXT %s %05x %02x\n", cur_pc, write ? "W" : "R",
	   addr, val);
}

/* ------------------------------------------------------------------------ */
/* Ports.  */

static int
port_index (int addr, int *is_dir)
{
  for (int i = 0; i < 11; i++)
    {
      if (addr == port_addr[i])
	{
	  *is_dir = 0;
	  return i;
	}
      if (addr == pdir_addr[i])
	{
	  *is_dir = 1;
	  return i;
	}
    }
  return -1;
}

static uint8_t
port_read (int p)
{
  uint8_t dir = sfr[pdir_addr[p]];
  /* P6_0 is the RC8650's CTS#.  */
  if (p == 6 && hooks.rc_cts)
    port_in[6] = (port_in[6] & ~1) | (hooks.rc_cts (hooks.ctx) ? 1 : 0);
  return (sfr[port_addr[p]] & dir) | (port_in[p] & ~dir);
}

/* ------------------------------------------------------------------------ */
/* SFR access.  */

static uint8_t
sfr_read (int a)
{
  int is_dir, p, i, off;

  if ((p = port_index (a, &is_dir)) >= 0 && !is_dir)
    return port_read (p);

  if ((i = timer_index (a)) >= 0)
    {
      uint16_t v = tmr[i].count;
      return (a == tmr[i].addr) ? (v & 0xff) : (v >> 8);
    }

  if ((i = uart_reg (a, &off)) >= 0 && off == 6)
    {
      /* Reading the receive buffer clears RI.  */
      sfr[uart[i].base + 5] &= ~0x08;
      return sfr[a];
    }

  switch (a)
    {
    case FSC:
      /* The synthesizer locks as soon as it is enabled.  */
      return (sfr[a] & 1) ? (sfr[a] | 0x80) : sfr[a];
    }

  return sfr[a];
}

static void
sfr_write (int a, uint8_t v)
{
  int is_dir, p, i, off;

  if (!sfr_seen[a])
    {
      sfr_seen[a] = 1;
      log_sfr (a, v, "first");
    }

  if ((p = port_index (a, &is_dir)) >= 0)
    {
      if (bp_log & BP_LOG_SFR && sfr[a] != v)
	fprintf (stderr, "%06x %s%d = %02x\n", cur_pc, is_dir ? "PD" : "P",
		 p, v);
      sfr[a] = v;
      return;
    }

  if ((i = timer_index (a)) >= 0)
    {
      timer_t_ *t = &tmr[i];
      sfr[a] = v;
      t->reload = sfr[t->addr] | (sfr[t->addr + 1] << 8);
      /* A stopped timer's counter follows the reload register.  */
      if (!timer_started (i))
	t->count = t->reload;
      return;
    }

  if ((i = uart_reg (a, &off)) >= 0)
    {
      uart_t *u = &uart[i];
      switch (off)
	{
	case 0:
	  /* Leaving I2C mode lets go of the bus.  The firmware's bus reset
	     (after talking to the MAS3507D) does this: U2SMR2 SWC2/SDHI,
	     U2MR = 0, back to 2.  The lines float high, nothing holds the
	     bus, and the next start must find BBS clear.  */
	  if (i == 2 && (v & 7) != 2)
	    {
	      sfr[U2SMR] &= ~0x04;
	      i2c.xfer = i2c.cond = 0;
	      i2c.phase = 0;
	    }
	  sfr[a] = v;
	  return;
	case 2:
	  if (i == 2 && i2c_mode ())
	    {
	      /* Bit 9 is the next write, to U2TB+1; read it when done.  */
	      sfr[a] = v;
	      sfr[u->base + 5] &= ~0x02;
	      i2c.xfer = 1;
	      i2c.xfer_at = cyc + uart_frame_cycles (u);
	      return;
	    }
	  sfr[a] = v;
	  u->tb = v;
	  u->tb_full = 1;
	  sfr[u->base + 5] &= ~0x02;	/* TI clear */
	  if (sfr[u->base + 5] & 0x01)	/* TE */
	    uart_start_shift (i);
	  return;
	case 4:
	  sfr[a] = (sfr[a] & 0x08) | (v & ~0x08);	/* TXEPT read-only */
	  return;
	case 5:
	  sfr[a] = (sfr[a] & 0x0A) | (v & ~0x0A);	/* TI, RI read-only */
	  if ((v & 1) && u->tb_full)
	    uart_start_shift (i);
	  return;
	}
      sfr[a] = v;
      return;
    }

  switch (a)
    {
    case TABSR:
      {
	uint8_t old = sfr[a];
	sfr[a] = v;
	/* Starting a timer loads its counter from the reload register.  */
	for (i = 0; i < 5; i++)
	  if (!((old >> i) & 1) && ((v >> i) & 1))
	    {
	      tmr[i].count = tmr[i].reload;
	      tmr[i].prescale = 0;
	    }
	return;
      }
    case ONSF:
      sfr[a] = v & 0xE0;
      for (i = 0; i < 5; i++)
	if (v & (1 << i))
	  {
	    tmr[i].running = 1;
	    tmr[i].count = tmr[i].reload;
	    tmr[i].prescale = 0;
	  }
      return;
    case ADCON0:
      sfr[a] = v;
      if (v & 0x40)		/* ADST: conversion is instant */
	{
	  for (i = 0; i < 8; i++)
	    {
	      /* Near full scale: batteries good.  */
	      sfr[AD0 + 2 * i] = 0xE0;
	      sfr[AD0 + 2 * i + 1] = 0x03;
	    }
	  sfr[a] &= ~0x40;
	  irq (ADIC);
	}
      return;
    case WDTS:
      return;
    case U2SMR4:
      i2c_smr4_write (v);
      return;
    case U2SMR:
      /* BBS (bit 2) is the bus's, not the program's.  */
      sfr[a] = (v & ~0x04) | (sfr[a] & 0x04);
      return;
    }

  sfr[a] = v;
}

/* ------------------------------------------------------------------------ */
/* The mem_* interface the core uses.  */

static uint8_t
get_byte (int a)
{
  a &= MEMSIZE - 1;
  if (a < 0x400)
    {
      uint8_t v = sfr_read (a);
      if (bp_log & BP_LOG_SFRALL)
	fprintf (stderr, "%06x RD %03x %02x\n", cur_pc, a, v);
      return v;
    }
  if ((a & 0xFF800) == CF_BASE)
    {
      uint8_t v = cf_read (a & 0x7FF);
      log_ext (a, v, 0);
      return v;
    }
  if (a >= NOR_LO && a < NOR_HI)
    {
      uint8_t v = nor_read (a);
      log_ext (a, v, 0);
      log_watch (a, v, 0);
      return v;
    }
  if (a >= 0x2C00 && a < ROM_BASE)
    log_ext (a, ram[a], 0);
  log_watch (a, ram[a], 0);
  return ram[a];
}

static void
put_byte (int a, uint8_t v)
{
  a &= MEMSIZE - 1;
  if (a < 0x400)
    {
      if (bp_log & BP_LOG_SFRALL)
	fprintf (stderr, "%06x WR %03x %02x\n", cur_pc, a, v);
      sfr_write (a, v);
      return;
    }
  if (a >= ROM_BASE)
    {
      if (bp_log & BP_LOG_EXT)
	fprintf (stderr, "%06x ROM write %05x %02x\n", cur_pc, a, v);
      return;
    }
  if ((a & 0xFF800) == CF_BASE)
    {
      log_ext (a, v, 1);
      cf_write (a & 0x7FF, v);
      return;
    }
  if (a >= NOR_LO && a < NOR_HI)
    {
      log_ext (a, v, 1);
      nor_write (a, v);
      return;
    }
  if (a >= 0x2C00)
    log_ext (a, v, 1);
  log_watch (a, v, 1);
  ram[a] = v;
}

void init_mem (void) { }

/* Instruction fetch.  Code runs from internal flash and RAM, and also from
   the external flash, where the application is installed.  */
static uint8_t
fetch (uint32_t a)
{
  a &= MEMSIZE - 1;
  if (a >= NOR_LO && a < NOR_HI)
    return nor_read (a);
  return ram[a];
}

unsigned char mem_get_pc (void) { return fetch (regs.r_pc); }

unsigned char mem_get_qi (int a) { return get_byte (a); }
unsigned short mem_get_hi (int a) { return get_byte (a) | get_byte (a + 1) << 8; }
unsigned long mem_get_psi (int a)
{
  return get_byte (a) | get_byte (a + 1) << 8 | (unsigned long) get_byte (a + 2) << 16;
}
unsigned long mem_get_si (int a)
{
  return mem_get_psi (a) | (unsigned long) get_byte (a + 3) << 24;
}

void mem_put_qi (int a, unsigned char v) { put_byte (a, v); }
void mem_put_hi (int a, unsigned short v) { put_byte (a, v); put_byte (a + 1, v >> 8); }
void mem_put_psi (int a, unsigned long v)
{
  put_byte (a, v); put_byte (a + 1, v >> 8); put_byte (a + 2, v >> 16);
}
void mem_put_si (int a, unsigned long v)
{
  mem_put_psi (a, v); put_byte (a + 3, v >> 24);
}

void
mem_put_blk (int a, const void *buf, int n)
{
  const uint8_t *b = buf;
  while (n--)
    put_byte (a++, *b++);
}

void
mem_get_blk (int a, void *buf, int n)
{
  uint8_t *b = buf;
  while (n--)
    *b++ = get_byte (a++);
}

/* ------------------------------------------------------------------------ */
/* Interrupt controller.  IC register = 0x40 + vector, 0x41..0x5F.  */

static int
interrupts (void)
{
  if (!(regs.r_flags & FLAGBIT_I))
    return 0;
  int best = -1, best_pri = (regs.r_flags >> 12) & 7;
  for (int a = 0x41; a <= 0x5F; a++)
    {
      uint8_t ic = sfr[a];
      if ((ic & 0x08) && (ic & 7) > best_pri)
	{
	  best_pri = ic & 7;
	  best = a;
	}
    }
  if (best < 0)
    return 0;
  if (bp_log & BP_LOG_INT)
    {
      uint32_t va = ((uint32_t) regs.r_intbh << 16 | regs.r_intbl)
	+ (best - 0x40) * 4;
      uint32_t h = ram[va & (MEMSIZE - 1)] | ram[(va + 1) & (MEMSIZE - 1)] << 8
	| (uint32_t) ram[(va + 2) & (MEMSIZE - 1)] << 16;
      fprintf (stderr, "%06lx INT vector %d -> %05x\n",
	       (unsigned long) regs.r_pc, best - 0x40, h);
    }
  waiting = 0;
  trigger_peripheral_interrupt (best - 0x40, best);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Board: keypad.  */

static int any_key;

/* External interrupts on port 8, as the firmware uses them: P8_3, the
   card-detect switch, raises 0x5F (vector 31); P8_4, the MAS3507D's demand
   line, raises 0x5E (vector 30).  Bit 4 of each IC register picks the
   edge: 1 rising, 0 falling.  */
static void
int_pin_edges (uint8_t old, uint8_t now)
{
  static const struct { uint8_t bit; int ic; } pin[] = {
    {0x08, 0x5F}, {0x10, 0x5E},
  };
  for (size_t i = 0; i < sizeof pin / sizeof pin[0]; i++)
    if ((old ^ now) & pin[i].bit)
      {
	int rising = (now & pin[i].bit) != 0;
	if (rising == ((sfr[pin[i].ic] & 0x10) != 0))
	  irq (pin[i].ic);
      }
}

static void
board_update (void)
{
  /* P8_3: card detect, low with a card in the slot.  */
  uint8_t p8 = port_in[8];
  port_in[8] = (port_in[8] & ~0x08) | (cf_present () ? 0 : 0x08);
  int_pin_edges (p8, port_in[8]);

  /* Columns read low where a pressed key meets a row driven low.  */
  uint8_t cols = 0xF8;
  if (any_key)
    {
      uint8_t pd7 = sfr[pdir_addr[7]], p7 = sfr[port_addr[7]];
      for (int r = 0; r < 4; r++)
	{
	  uint8_t bit = 0x10 << r;
	  if (!(pd7 & bit) || (p7 & bit))
	    continue;
	  for (int c = 0; c < 5; c++)
	    if (matrix[r * 5 + c])
	      cols &= ~(0x08 << c);
	}
    }
  port_in[10] = (port_in[10] & 0x07) | cols;
  /* KI3-KI7: a falling edge requests the key-input interrupt.  */
  if (ki_prev & ~cols & 0xF8)
    irq (KUPIC);
  ki_prev = cols;
}

void
bp_set_matrix (int pos, int down)
{
  if (pos < 0 || pos >= BP_MATRIX)
    return;
  matrix[pos] = down != 0;
  any_key = 0;
  for (int i = 0; i < BP_MATRIX; i++)
    any_key |= matrix[i];
}

void
bp_set_pins (int p, uint8_t mask, uint8_t value)
{
  if (p < 0 || p >= 11)
    return;
  uint8_t old = port_in[p];
  port_in[p] = (port_in[p] & ~mask) | (value & mask);
  if (p == 8)
    int_pin_edges (old, port_in[8]);
}

void
bp_set_key (int key, int down)
{
  if (key >= 0 && key < BP_NKEYS)
    bp_set_matrix (bp_key_pos[key], down);
}

/* ------------------------------------------------------------------------ */
/* Stepping.  */

/* Main-crystal cycles per instruction.  The M16C averages roughly 3-4 BCLK
   cycles per instruction; BCLK is Xin divided per CM0/CM1.  */
static uint32_t
cycles_per_insn (void)
{
  uint32_t cpi = 4;
  if (sfr[CM0] & 0x80)		/* running from the 32 kHz sub-clock */
    return cpi * (BP_XIN_HZ / 32768);
  if (sfr[CM0] & 0x40)		/* CM06: divide by 8 */
    return cpi * 8;
  static const int div[4] = { 1, 2, 4, 16 };
  return cpi * div[(sfr[CM1] >> 6) & 3];
}

static void
step (void)
{
  uint32_t dc = cycles_per_insn ();

  if (!waiting)
    {
      uint32_t pc = regs.r_pc & (MEMSIZE - 1);
      cur_pc = pc;
      {
	/* BP_BREAK=pc[,count] (hex pc): dump registers and stack there.  */
	static long brk_pc = -2, brk_n = 1;
	if (brk_pc == -2)
	  {
	    const char *b = getenv ("BP_BREAK");
	    char *e;
	    brk_pc = b ? (long) strtoul (b, &e, 16) : -1;
	    if (b && *e == ',')
	      brk_n = strtol (e + 1, NULL, 0);
	  }
	if ((long) pc == brk_pc && brk_n > 0)
	  {
	    brk_n--;
	    fprintf (stderr, "BREAK %06x t=%.4f r0=%04x r1=%04x r2=%04x r3=%04x"
		     " a0=%04lx a1=%04lx fb=%04lx sb=%04lx sp=%04x flg=%04x\n",
		     pc, (double) cyc / BP_XIN_HZ, get_reg (r0), get_reg (r1),
		     get_reg (r2), get_reg (r3), (long) get_reg (a0),
		     (long) get_reg (a1), (long) get_reg (fb), (long) get_reg (sb),
		     get_reg (sp), get_reg (flags));
	    unsigned stk = get_reg (sp);
	    for (int i = 0; i < 64; i++)
	      fprintf (stderr, "%s%02x", i % 16 ? " " : i ? "\n  " : "  ",
		       ram[(stk + i) & (MEMSIZE - 1)]);
	    fprintf (stderr, "\n");
	  }
      }
      if (bp_log & BP_LOG_TRACE)
	fprintf (stderr, "T %06x\n", pc);
      if (fetch (pc) == 0x7D && fetch (pc + 1) == 0xF3)
	{
	  regs.r_pc += 2;
	  waiting = 1;
	}
      else
	{
	  int r = decode_opcode ();
	  if (M32C_HIT_BREAK (r))
	    {
	      fprintf (stderr, "%06x BRK\n", pc);
	      regs.r_pc++;
	      trigger_fixed_interrupt (0xFFFE4);
	    }
	  else if (!M32C_STEPPED (r))
	    fprintf (stderr, "%06x step result %x\n", pc, r);
	}
    }

  cyc += dc;
  timers_tick (dc);
  uart_tick ();
  i2c_tick ();
  board_update ();
  interrupts ();
}

void
bp_run (uint64_t n)
{
  uint64_t end = cyc + n;
  while (cyc < end)
    step ();
}

uint64_t bp_cycles (void) { return cyc; }
uint32_t bp_pc (void) { return regs.r_pc; }

uint8_t
bp_peek (uint32_t a)
{
  a &= MEMSIZE - 1;
  return a < 0x400 ? sfr[a] : ram[a];
}

/* ------------------------------------------------------------------------ */
/* Loading and reset.  */

static int
load_file (const char *dir, const char *name, uint8_t **out, size_t *len)
{
  char path[1024];
  snprintf (path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen (path, "rb");
  if (!f)
    return -1;
  fseek (f, 0, SEEK_END);
  *len = ftell (f);
  fseek (f, 0, SEEK_SET);
  *out = malloc (*len);
  if (fread (*out, 1, *len, f) != *len)
    {
      fclose (f);
      free (*out);
      return -1;
    }
  fclose (f);
  return 0;
}

int
bp_load (const char *fwdir, const bp_hooks *h)
{
  uint8_t *img;
  size_t len;

  if (h)
    hooks = *h;
  if (load_file (fwdir, "bpfw._BB", &img, &len) < 0)
    {
      fprintf (stderr, "cannot read %s/bpfw._BB\n", fwdir);
      return -1;
    }
  /* A little-endian load address, then the image.  The header reads
     0xE0000000 for an image at 0xE0000: the address is in the top 20 bits.  */
  uint32_t base = img[0] | img[1] << 8 | img[2] << 16 | (uint32_t) img[3] << 24;
  if (base >= MEMSIZE)
    base >>= 12;
  if (base + (len - 4) > MEMSIZE)
    {
      fprintf (stderr, "bpfw._BB: bad load address %x\n", base);
      free (img);
      return -1;
    }
  memset (ram, 0xFF, MEMSIZE);
  memcpy (ram + base, img + 4, len - 4);
  free (img);
  bp_reset ();
  return 0;
}

void
bp_reset (void)
{
  memset (ram + 0x400, 0, ROM_BASE - 0x400);
  memset (sfr, 0, sizeof sfr);
  sfr[CM0] = 0x48;		/* CM06 set: BCLK = Xin / 8 */
  sfr[CM1] = 0x20;
  sfr[CSR] = 0x01;
  sfr[USBIE] = 0xFF;
  sfr[USBIE + 1] = 0x01;
  sfr[FSC] = 0x60;
  for (int ch = 0; ch < 4; ch++)
    {
      sfr[uart[ch].base + 5] = 0x02;	/* TI */
      sfr[uart[ch].base + 4] = 0x08;	/* TXEPT */
      uart[ch].tb_full = uart[ch].shifting = 0;
      uart[ch].rxh = uart[ch].rxt = 0;
    }
  for (int i = 0; i < 5; i++)
    tmr[i].count = tmr[i].reload = 0, tmr[i].running = 0;
  memset (port_in, 0xFF, sizeof port_in);
  port_in[9] &= ~0x02;		/* P9_1: USB VBUS sense, low = unplugged */
  /* The clock chip has its own battery: on a later reset it keeps time.
     At first power-on, BP_TIME=epoch pins it, for reproducible runs.  */
  static int powered;
  if (powered)
    rtc_base = (long long) rtc_now ();
  else
    {
      rtc_base = getenv ("BP_TIME") ? atoll (getenv ("BP_TIME"))
	: (long long) time (NULL);
      rtc_reg[7] = 0;
      powered = 1;
    }
  cyc = 0;
  waiting = 0;

  m32c_set_cpu (CPU_M16C);
  init_regs ();
  regs.r_pc = mem_get_psi (0xFFFFC);
}

/* Which matrix position each key is wired to (see bp.h), established by
   pressing each position and from the firmware's own decode table at
   0xE0214, which maps scan bits to key codes 1-9, A-F = 0x0A-0x0F, 0 = 0x10,
   * = 0x11, # = 0x12.  The scanner packs row 3 at bits 14-18 rather than
   15-19, so position 15 would alias C; it is not wired, nor is 19.

	     col0  col1  col2  col3  col4      (P10_3 .. P10_7)
     row0     F     B     *     6     2         P7_4
     row1     E     A     9     5     1         P7_5
     row2     D     #     8     4     C         P7_6
     row3     -     0     7     3     -         P7_7  */
const int bp_key_pos[BP_NKEYS] = {
  [BP_K1] = 9, [BP_K2] = 4, [BP_K3] = 18,
  [BP_K4] = 13, [BP_K5] = 8, [BP_K6] = 3,
  [BP_K7] = 17, [BP_K8] = 12, [BP_K9] = 7,
  [BP_KSTAR] = 2, [BP_K0] = 16, [BP_KHASH] = 11,
  [BP_KA] = 6, [BP_KB] = 1, [BP_KC] = 14,
  [BP_KD] = 10, [BP_KE] = 5, [BP_KF] = 0,
};
