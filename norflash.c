/* norflash.c --- the Book Port's external flash: an AMD Am29LV400BT.

   512 KB, 16-bit, top boot block: seven 64 KB sectors, then 32 KB, 8 KB,
   8 KB and 16 KB.  The chip's address is the CPU address (the firmware's
   driver reads the boot sectors at 0x78000 and 0x7C000), and the CPU can
   see it from 0x30000 up; the lower part is behind internal RAM and the
   other devices.  It holds what the firmware installs from a card: the
   application (BPfwex) at 0x30000, the spoken messages and the Spanish
   rules.

   Commands are the standard AMD set, word-addressed: unlock writes of AA to
   word 0x555 and 55 to word 0x2AA, then A0 program, 90 autoselect, 80 + AA
   55 + 10/30 chip/sector erase, F0 reset.  Programming and erasing finish
   at once, so status polling (DQ7) passes on the first read.

   Contents persist in a file so an update survives restarting the
   emulator.  */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir (p)
#else
#define make_dir(p) mkdir (p, 0777)
#endif
#include <string.h>

#include "norflash.h"

#define SIZE (512 * 1024)

static uint8_t flash[SIZE];
static FILE *backing;
int nor_log = 0;

enum { READ, UNLOCK1, UNLOCK2, CMD_PROGRAM, ERASE1, ERASE2, ERASE3, AUTOSELECT };
static int state = READ;
static int prog_lo = -1;	/* low byte of a word being programmed */

static void
sector_bounds (uint32_t a, uint32_t *start, uint32_t *len)
{
  if (a < 0x70000)
    {
      *start = a & ~0xFFFFu;
      *len = 0x10000;
    }
  else if (a < 0x78000)
    *start = 0x70000, *len = 0x8000;
  else if (a < 0x7A000)
    *start = 0x78000, *len = 0x2000;
  else if (a < 0x7C000)
    *start = 0x7A000, *len = 0x2000;
  else
    *start = 0x7C000, *len = 0x4000;
}

static void
save (uint32_t start, uint32_t len)
{
  if (!backing)
    return;
  fseek (backing, start, SEEK_SET);
  fwrite (flash + start, 1, len, backing);
  fflush (backing);
}

int
nor_open (const char *path)
{
  memset (flash, 0xFF, SIZE);
  backing = fopen (path, "r+b");
  if (!backing)
    {
      char dir[1024];		/* its folders, e.g. state/, if new */
      snprintf (dir, sizeof dir, "%s", path);
      for (char *s = strchr (dir + 1, '/'); s; s = strchr (s + 1, '/'))
	{
	  *s = 0;
	  make_dir (dir);
	  *s = '/';
	}
      backing = fopen (path, "w+b");
      if (!backing)
	{
	  perror (path);
	  return -1;
	}
      fwrite (flash, 1, SIZE, backing);
      fflush (backing);
    }
  else if (fread (flash, 1, SIZE, backing) != SIZE)
    memset (flash, 0xFF, SIZE);
  return 0;
}

uint8_t
nor_read (uint32_t a)
{
  a &= SIZE - 1;
  if (state == AUTOSELECT)
    {
      /* Word 0: manufacturer (AMD), word 1: device (29LV400BT), word 2:
	 sector protection (none).  */
      static const uint16_t id[4] = { 0x0001, 0x22B9, 0x0000, 0x0000 };
      uint16_t w = id[(a >> 1) & 3];
      return (a & 1) ? w >> 8 : w & 0xFF;
    }
  return flash[a];
}

void
nor_write (uint32_t a, uint8_t v)
{
  a &= SIZE - 1;
  uint32_t word = (a >> 1) & 0x7FF;

  if (state == CMD_PROGRAM)
    {
      /* The data is a word: low byte first, then the high byte commits.  */
      if (!(a & 1))
	{
	  prog_lo = v;
	  return;
	}
      if (prog_lo >= 0)
	flash[a - 1] &= prog_lo;
      flash[a] &= v;
      prog_lo = -1;
      save (a - 1, 2);
      state = READ;
      return;
    }

  if (a & 1)
    return;			/* commands are on the low byte lane */

  if (v == 0xF0)
    {
      state = READ;
      return;
    }

  switch (state)
    {
    case READ:
    case AUTOSELECT:
      state = (word == 0x555 && v == 0xAA) ? UNLOCK1 : state;
      return;
    case UNLOCK1:
      state = (word == 0x2AA && v == 0x55) ? UNLOCK2 : READ;
      return;
    case UNLOCK2:
      if (word != 0x555)
	{
	  state = READ;
	  return;
	}
      switch (v)
	{
	case 0xA0: state = CMD_PROGRAM; prog_lo = -1; return;
	case 0x90: state = AUTOSELECT; return;
	case 0x80: state = ERASE1; return;
	default: state = READ; return;
	}
    case ERASE1:
      state = (word == 0x555 && v == 0xAA) ? ERASE2 : READ;
      return;
    case ERASE2:
      state = (word == 0x2AA && v == 0x55) ? ERASE3 : READ;
      return;
    case ERASE3:
      if (v == 0x10 && word == 0x555)
	{
	  memset (flash, 0xFF, SIZE);
	  save (0, SIZE);
	  if (nor_log)
	    fprintf (stderr, "NOR chip erase\n");
	}
      else if (v == 0x30)
	{
	  uint32_t s, n;
	  sector_bounds (a, &s, &n);
	  memset (flash + s, 0xFF, n);
	  save (s, n);
	  if (nor_log)
	    fprintf (stderr, "NOR erase sector %05x (%u KB)\n", s, n / 1024);
	}
      state = READ;
      return;
    }
}
