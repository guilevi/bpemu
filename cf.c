/* cf.c --- a CompactFlash card in memory-mapped (common memory) mode.

   The Book Port reaches its card through a 16-register window at 0x6000:
   0 data, 1 error/features, 2 sector count, 3-5 LBA 7:0, 15:8, 23:16 (or
   sector/cylinder), 6 drive/head, 7 status/command, 8/9 duplicate data,
   D duplicate error, E alternate status/device control, F drive address.
   0x400-0x7FF is the data register again at every address, which is how the
   firmware block-moves sectors.  The firmware moves data as 16-bit words, so a read of the data register's
   low byte fetches the next word and the high byte returns its other half.

   Backed by a raw disk image (no partition table is required; the firmware
   reads sector 0 itself).  Writes go straight to the file.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cf.h"

enum
{
  ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80,
  ER_ABRT = 0x04, ER_IDNF = 0x10,
};

static FILE *img;
static uint32_t nsect;		/* card size in 512-byte sectors */
static int heads = 16, spt = 63;

static uint8_t reg[16];		/* task file as last written */
static uint8_t status = ST_DRDY | ST_DSC, error;
static uint8_t devctl;

static uint8_t buf[512];
static int bufpos = -1;		/* byte offset into buf while DRQ, else -1 */
static int writing;		/* buf is being filled by the host */
static uint32_t xfer_lba;
static unsigned xfer_left;	/* sectors still to move, including buf's */
static uint8_t data_hi;		/* high half of the word being read */
static uint8_t wr_lo;		/* low half of the word being written */
static int multiple = 1;

int cf_log = 0;

static uint32_t
cur_lba (void)
{
  if (reg[6] & 0x40)
    return reg[3] | reg[4] << 8 | reg[5] << 16 | (uint32_t) (reg[6] & 0x0F) << 24;
  /* CHS */
  unsigned cyl = reg[4] | reg[5] << 8, head = reg[6] & 0x0F, sec = reg[3];
  return (cyl * heads + head) * spt + (sec ? sec - 1 : 0);
}

static void
set_lba (uint32_t lba)
{
  if (reg[6] & 0x40)
    {
      reg[3] = lba;
      reg[4] = lba >> 8;
      reg[5] = lba >> 16;
      reg[6] = (reg[6] & 0xF0) | ((lba >> 24) & 0x0F);
    }
  else
    {
      unsigned sec = lba % spt + 1, t = lba / spt;
      reg[3] = sec;
      reg[6] = (reg[6] & 0xF0) | (t % heads);
      reg[4] = (t / heads);
      reg[5] = (t / heads) >> 8;
    }
}

static void
abort_cmd (uint8_t why)
{
  error = why;
  status = ST_DRDY | ST_DSC | ST_ERR;
  bufpos = -1;
}

static int
load_sector (uint32_t lba)
{
  if (!img || lba >= nsect)
    return -1;
  if (fseeko (img, (off_t) lba * 512, SEEK_SET) != 0
      || fread (buf, 1, 512, img) != 512)
    memset (buf, 0, 512);
  return 0;
}

static void
store_sector (uint32_t lba)
{
  if (!img || lba >= nsect)
    return;
  fseeko (img, (off_t) lba * 512, SEEK_SET);
  fwrite (buf, 1, 512, img);
  fflush (img);
}

static void
put_string (uint16_t *w, int first, int words, const char *s)
{
  /* ATA strings: two characters per word, first in the high byte.  */
  for (int i = 0; i < words; i++)
    {
      char a = *s ? *s++ : ' ';
      char b = *s ? *s++ : ' ';
      w[first + i] = (uint16_t) ((uint8_t) a << 8 | (uint8_t) b);
    }
}

static void
identify (void)
{
  uint16_t w[256];
  memset (w, 0, sizeof w);
  unsigned cyls = nsect / (heads * spt);
  if (cyls > 65535)
    cyls = 65535;
  w[0] = 0x848A;		/* CompactFlash signature */
  w[1] = cyls;
  w[3] = heads;
  w[4] = 512 * spt;
  w[5] = 512;
  w[6] = spt;
  w[7] = nsect >> 16;
  w[8] = nsect;
  put_string (w, 10, 10, "BPEMU0000001");
  w[20] = 2;
  w[21] = 2;
  w[22] = 4;
  put_string (w, 23, 4, "1.0");
  put_string (w, 27, 20, "Book Port emulator CF card");
  w[47] = 0x8001;		/* READ/WRITE MULTIPLE: 1 sector */
  w[49] = 0x0200;		/* LBA */
  w[51] = 0x0200;
  w[53] = 0x0001;
  w[54] = cyls;
  w[55] = heads;
  w[56] = spt;
  uint32_t chs = (uint32_t) cyls * heads * spt;
  w[57] = chs;
  w[58] = chs >> 16;
  w[59] = 0x0100 | multiple;
  w[60] = nsect;
  w[61] = nsect >> 16;
  for (int i = 0; i < 256; i++)
    {
      buf[2 * i] = w[i];
      buf[2 * i + 1] = w[i] >> 8;
    }
}

static void
start_read (void)
{
  if (load_sector (xfer_lba) < 0)
    {
      abort_cmd (ER_IDNF);
      return;
    }
  bufpos = 0;
  writing = 0;
  status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void
command (uint8_t c)
{
  if (cf_log)
    fprintf (stderr, "CF cmd %02x count %u lba %u\n", c, reg[2], cur_lba ());
  error = 0;
  switch (c)
    {
    case 0x90:			/* EXECUTE DEVICE DIAGNOSTIC */
      error = 0x01;		/* device 0 passed */
      reg[2] = reg[3] = 1;
      reg[4] = reg[5] = 0;
      reg[6] &= 0xF0;
      status = ST_DRDY | ST_DSC;
      return;
    case 0xEC:			/* IDENTIFY DEVICE */
      identify ();
      bufpos = 0;
      writing = 0;
      xfer_left = 1;
      status = ST_DRDY | ST_DSC | ST_DRQ;
      return;
    case 0x20: case 0x21: case 0xC4:	/* READ SECTOR(S), READ MULTIPLE */
      xfer_lba = cur_lba ();
      xfer_left = reg[2] ? reg[2] : 256;
      start_read ();
      return;
    case 0x30: case 0x31: case 0xC5:	/* WRITE SECTOR(S), WRITE MULTIPLE */
      xfer_lba = cur_lba ();
      xfer_left = reg[2] ? reg[2] : 256;
      if (xfer_lba >= nsect)
	{
	  abort_cmd (ER_IDNF);
	  return;
	}
      bufpos = 0;
      writing = 1;
      status = ST_DRDY | ST_DSC | ST_DRQ;
      return;
    case 0x40: case 0x41:	/* READ VERIFY */
      status = ST_DRDY | ST_DSC;
      return;
    case 0xC6:			/* SET MULTIPLE MODE */
      multiple = reg[2] ? reg[2] : 1;
      status = ST_DRDY | ST_DSC;
      return;
    case 0x91:			/* INITIALIZE DEVICE PARAMETERS */
      heads = (reg[6] & 0x0F) + 1;
      spt = reg[2] ? reg[2] : spt;
      status = ST_DRDY | ST_DSC;
      return;
    case 0xE5: case 0x98:	/* CHECK POWER MODE */
      reg[2] = 0xFF;
      status = ST_DRDY | ST_DSC;
      return;
    case 0xEF:			/* SET FEATURES */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
    case 0xE7:			/* FLUSH CACHE */
    case 0x10: case 0x70:	/* RECALIBRATE, SEEK */
      status = ST_DRDY | ST_DSC;
      return;
    default:
      if (cf_log)
	fprintf (stderr, "CF unsupported command %02x\n", c);
      abort_cmd (ER_ABRT);
    }
}

static void
reset (void)
{
  status = ST_DRDY | ST_DSC;
  error = 0x01;
  reg[2] = reg[3] = 1;
  reg[4] = reg[5] = reg[6] = 0;
  bufpos = -1;
}

static void
sector_done (void)
{
  if (writing)
    store_sector (xfer_lba);
  xfer_lba++;
  set_lba (xfer_lba);
  if (--xfer_left == 0)
    {
      bufpos = -1;
      status = ST_DRDY | ST_DSC;
      return;
    }
  if (writing)
    bufpos = 0;
  else
    start_read ();
}

static uint8_t
data_read_lo (void)
{
  if (bufpos < 0 || writing)
    return 0xFF;
  uint8_t lo = buf[bufpos];
  data_hi = buf[bufpos + 1];
  bufpos += 2;
  if (bufpos >= 512)
    sector_done ();
  return lo;
}

static void
data_write_word (uint8_t lo, uint8_t hi)
{
  if (bufpos < 0 || !writing)
    return;
  buf[bufpos] = lo;
  buf[bufpos + 1] = hi;
  bufpos += 2;
  if (bufpos >= 512)
    sector_done ();
}

uint8_t
cf_read (unsigned off)
{
  if (!img)
    return 0xFF;
  /* 0x400-0x7FF: the data register, at every address.  */
  if (off >= 0x400)
    return (off & 1) ? data_hi : data_read_lo ();
  switch (off & 15)
    {
    case 0: case 8:
      return data_read_lo ();
    case 1: case 9:
      return data_hi;
    case 2: case 3: case 4: case 5: case 6:
      return reg[off & 15];
    case 7: case 14:
      return status;
    case 13:
      return error;
    case 15:
      return 0xFE | !(reg[6] & 0x10);
    default:
      return 0xFF;
    }
}

void
cf_write (unsigned off, uint8_t v)
{
  if (!img)
    return;
  if (off >= 0x400)
    off = (off & 1) ? 9 : 8;
  switch (off & 15)
    {
    case 0: case 8:
      wr_lo = v;
      return;
    case 1: case 9:
      if (bufpos >= 0 && writing)
	data_write_word (wr_lo, v);
      else
	reg[1] = v;		/* features */
      return;
    case 2: case 3: case 4: case 5: case 6:
      reg[off & 15] = v;
      return;
    case 7:
      command (v);
      return;
    case 13:
      reg[1] = v;
      return;
    case 14:
      if ((devctl & 0x04) && !(v & 0x04))
	reset ();
      devctl = v;
      return;
    }
}

int
cf_insert (const char *path)
{
  img = fopen (path, "r+b");
  if (!img)
    {
      perror (path);
      return -1;
    }
  fseeko (img, 0, SEEK_END);
  nsect = ftello (img) / 512;
  reset ();
  return 0;
}

void
cf_eject (void)
{
  if (img)
    fclose (img);
  img = NULL;
  nsect = 0;
}

int
cf_present (void)
{
  return img != NULL;
}
