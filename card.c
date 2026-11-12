/* card.c --- card images without outside tools: a small FAT16/FAT32
   writer (format, make a folder, write a file under a long name, replace
   one) and the Book Port's text indexer.

   The whole first FAT is held in memory while a card is open and written
   back to every copy on close; directories are read and written whole.
   Long names are stored as VFAT entries with a generated NAME~N.EXT short
   name, as Windows and mtools do; the firmware reads them.  */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "card.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* Open a file to put on the card.  Its path is UTF-8 (from a drop, the
   clipboard or the command line); Windows' fopen wants the ANSI code page,
   so go through the wide-character call there.  */
static FILE *
open_source (const char *path)
{
#ifdef _WIN32
  wchar_t w[1024];
  if (MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1024))
    return _wfopen (w, L"rb");
#endif
  return fopen (path, "rb");
}

#define SECT 512

typedef struct
{
  FILE *f;
  uint64_t base;		/* byte offset of the FAT volume */
  int type;			/* 16 or 32 */
  uint32_t spc, reserved, nfats, rootents, fatsz, totsec;
  uint32_t root_sec, root_secs;	/* FAT16 fixed root directory */
  uint32_t root_clus;		/* FAT32 root directory */
  uint32_t data_sec, nclus;
  uint32_t *fat;		/* entries 0 .. nclus + 1 */
  uint32_t hint;		/* where to look for a free cluster */
} vol_t;

static int
err (const char *what, const char *detail)
{
  fprintf (stderr, "card: %s%s%s\n", what, detail ? ": " : "",
	   detail ? detail : "");
  return -1;
}

static uint16_t rd16 (const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32 (const uint8_t *p) { return rd16 (p) | (uint32_t) rd16 (p + 2) << 16; }
static void wr16 (uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32 (uint8_t *p, uint32_t v) { wr16 (p, v); wr16 (p + 2, v >> 16); }

static int
io (vol_t *v, uint64_t sec, void *buf, size_t nsec, int write)
{
  if (fseeko (v->f, (off_t) (v->base + sec * SECT), SEEK_SET))
    return -1;
  size_t n = write ? fwrite (buf, SECT, nsec, v->f) : fread (buf, SECT, nsec, v->f);
  return n == nsec ? 0 : -1;
}

static uint64_t
clus_sec (vol_t *v, uint32_t c)
{
  return v->data_sec + (uint64_t) (c - 2) * v->spc;
}

static uint32_t
eoc (vol_t *v)
{
  return v->type == 32 ? 0x0FFFFFFF : 0xFFFF;
}

static int
is_eoc (vol_t *v, uint32_t x)
{
  return v->type == 32 ? (x & 0x0FFFFFFF) >= 0x0FFFFFF8 : x >= 0xFFF8;
}

/* Open the FAT volume in an image: the first FAT partition of an MBR, or
   the whole image when sector 0 is itself a FAT boot sector.  */
static int
vol_open (vol_t *v, const char *path)
{
  memset (v, 0, sizeof *v);
  v->f = fopen (path, "r+b");
  if (!v->f)
    return err ("cannot open", path);
  uint8_t s[SECT];
  if (io (v, 0, s, 1, 0) || rd16 (s + 510) != 0xAA55)
    return err ("not a card image", path);
  for (int i = 0; i < 4 && !v->base; i++)
    {
      uint8_t *e = s + 446 + 16 * i;
      if (e[4] && strchr ("\x01\x04\x06\x0B\x0C\x0E", e[4]) && rd32 (e + 8))
	v->base = (uint64_t) rd32 (e + 8) * SECT;
    }
  if (v->base && (io (v, 0, s, 1, 0) || rd16 (s + 510) != 0xAA55))
    return err ("bad FAT partition", path);
  if (rd16 (s + 11) != SECT)
    return err ("unsupported sector size", path);
  v->spc = s[13];
  v->reserved = rd16 (s + 14);
  v->nfats = s[16];
  v->rootents = rd16 (s + 17);
  v->totsec = rd16 (s + 19) ? rd16 (s + 19) : rd32 (s + 32);
  v->fatsz = rd16 (s + 22) ? rd16 (s + 22) : rd32 (s + 36);
  v->root_secs = (v->rootents * 32 + SECT - 1) / SECT;
  v->root_sec = v->reserved + v->nfats * v->fatsz;
  v->data_sec = v->root_sec + v->root_secs;
  v->nclus = (v->totsec - v->data_sec) / v->spc;
  if (v->nclus < 4085)
    return err ("FAT12 is not supported", path);
  v->type = v->nclus < 65525 ? 16 : 32;
  v->root_clus = v->type == 32 ? rd32 (s + 44) : 0;

  uint8_t *raw = malloc ((size_t) v->fatsz * SECT);
  v->fat = malloc ((v->nclus + 2) * sizeof *v->fat);
  if (!raw || !v->fat || io (v, v->reserved, raw, v->fatsz, 0))
    return err ("cannot read the FAT", path);
  for (uint32_t c = 0; c < v->nclus + 2; c++)
    v->fat[c] = v->type == 32 ? rd32 (raw + 4 * c) & 0x0FFFFFFF : rd16 (raw + 2 * c);
  free (raw);
  v->hint = 2;
  return 0;
}

static int
vol_close (vol_t *v)
{
  int r = 0;
  if (v->fat)
    {
      uint8_t *raw = calloc (v->fatsz, SECT);
      if (raw && io (v, v->reserved, raw, v->fatsz, 0) == 0)
	{
	  for (uint32_t c = 0; c < v->nclus + 2; c++)
	    if (v->type == 32)
	      wr32 (raw + 4 * c, (rd32 (raw + 4 * c) & 0xF0000000) | v->fat[c]);
	    else
	      wr16 (raw + 2 * c, v->fat[c]);
	  for (uint32_t i = 0; i < v->nfats; i++)
	    if (io (v, v->reserved + i * v->fatsz, raw, v->fatsz, 1))
	      r = -1;
	}
      else
	r = -1;
      free (raw);
      if (v->type == 32)
	{
	  /* FSInfo: free count and where to look next.  */
	  uint8_t s[SECT];
	  uint32_t nfree = 0;
	  for (uint32_t c = 2; c < v->nclus + 2; c++)
	    nfree += !v->fat[c];
	  if (io (v, 1, s, 1, 0) == 0 && rd32 (s) == 0x41615252)
	    {
	      wr32 (s + 488, nfree);
	      wr32 (s + 492, v->hint);
	      io (v, 1, s, 1, 1);
	    }
	}
      free (v->fat);
    }
  if (v->f && fclose (v->f))
    r = -1;
  return r ? err ("write failed", NULL) : 0;
}

static uint32_t
alloc_clus (vol_t *v, uint32_t prev)
{
  for (uint32_t i = 0; i < v->nclus; i++)
    {
      uint32_t c = 2 + (v->hint - 2 + i) % v->nclus;
      if (!v->fat[c])
	{
	  v->fat[c] = eoc (v);
	  if (prev)
	    v->fat[prev] = c;
	  v->hint = c + 1 < v->nclus + 2 ? c + 1 : 2;
	  return c;
	}
    }
  return 0;
}

static void
free_chain (vol_t *v, uint32_t c)
{
  while (c >= 2 && c < v->nclus + 2)
    {
      uint32_t n = v->fat[c];
      v->fat[c] = 0;
      if (is_eoc (v, n))
	break;
      c = n;
    }
}

/* ---- Directories, held whole in memory.  first == 0: the FAT16 root.  */

typedef struct
{
  uint32_t first;
  uint8_t *e;			/* 32-byte entries */
  size_t n;			/* entries */
} dir_t;

static size_t
chain_len (vol_t *v, uint32_t c)
{
  size_t n = 0;
  while (c >= 2 && c < v->nclus + 2 && n <= v->nclus)
    {
      n++;
      if (is_eoc (v, v->fat[c]))
	break;
      c = v->fat[c];
    }
  return n;
}

static int
dir_load (vol_t *v, dir_t *d, uint32_t first)
{
  d->first = first;
  if (!first)
    {
      d->n = v->rootents;
      d->e = malloc (v->root_secs * SECT);
      return d->e ? io (v, v->root_sec, d->e, v->root_secs, 0) : -1;
    }
  size_t nc = chain_len (v, first), bpc = v->spc * SECT;
  d->n = nc * bpc / 32;
  d->e = malloc (nc * bpc);
  if (!d->e)
    return -1;
  uint32_t c = first;
  for (size_t i = 0; i < nc; i++, c = v->fat[c])
    if (io (v, clus_sec (v, c), d->e + i * bpc, v->spc, 0))
      return -1;
  return 0;
}

static int
dir_store (vol_t *v, dir_t *d)
{
  if (!d->first)
    return io (v, v->root_sec, d->e, v->root_secs, 1);
  size_t bpc = v->spc * SECT, nc = d->n * 32 / bpc;
  uint32_t c = d->first, prev = 0;
  for (size_t i = 0; i < nc; i++)
    {
      if (!c || c >= v->nclus + 2)
	{
	  c = alloc_clus (v, prev);
	  if (!c)
	    return -1;
	}
      if (io (v, clus_sec (v, c), d->e + i * bpc, v->spc, 1))
	return -1;
      prev = c;
      c = is_eoc (v, v->fat[c]) ? 0 : v->fat[c];
    }
  return 0;
}

/* Room for k consecutive entries; grows a cluster directory.  */
static long
dir_room (vol_t *v, dir_t *d, size_t k)
{
  size_t run = 0;
  for (size_t i = 0; i < d->n; i++)
    {
      uint8_t b = d->e[32 * i];
      if (b == 0x00 || b == 0xE5)
	{
	  if (++run == k)
	    return (long) (i + 1 - k);
	}
      else
	run = 0;
    }
  if (!d->first)
    return -1;			/* the FAT16 root is full */
  size_t add = (k * 32 + v->spc * SECT - 1) / (v->spc * SECT) * v->spc * SECT;
  uint8_t *e = realloc (d->e, d->n * 32 + add);
  if (!e)
    return -1;
  memset (e + d->n * 32, 0, add);
  d->e = e;
  size_t at = d->n - run;
  d->n += add / 32;
  return (long) at;
}

static uint8_t
lfn_sum (const uint8_t *sfn)
{
  uint8_t s = 0;
  for (int i = 0; i < 11; i++)
    s = (uint8_t) (((s & 1) << 7) + (s >> 1) + sfn[i]);
  return s;
}

/* UTF-8 to UTF-16 (BMP; anything else becomes '_').  */
static size_t
utf16 (const char *s, uint16_t *out, size_t max)
{
  size_t n = 0;
  const unsigned char *p = (const unsigned char *) s;
  while (*p && n < max)
    {
      uint32_t c = *p++;
      int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
      if (more)
	c &= 0x3F >> more;
      while (more-- && (*p & 0xC0) == 0x80)
	c = c << 6 | (*p++ & 0x3F);
      out[n++] = c > 0xFFFF ? '_' : (uint16_t) c;
    }
  return n;
}

static int
ieq16 (const uint16_t *a, size_t na, const uint16_t *b, size_t nb)
{
  if (na != nb)
    return 0;
  for (size_t i = 0; i < na; i++)
    {
      uint16_t x = a[i] < 128 ? (uint16_t) tolower (a[i]) : a[i];
      uint16_t y = b[i] < 128 ? (uint16_t) tolower (b[i]) : b[i];
      if (x != y)
	return 0;
    }
  return 1;
}

/* An entry's names: long (if any) and short, as UTF-16.  */
typedef struct
{
  size_t at, first;		/* SFN index, first LFN index */
  uint16_t lname[256], sname[13];
  size_t llen, slen;
} found_t;

/* Find name in d.  Returns 1 and fills *f when found.  */
static int
dir_find (dir_t *d, const char *name, found_t *f)
{
  uint16_t want[256];
  size_t nwant = utf16 (name, want, 255);
  uint16_t lfn[260];
  int lfn_ok = 0;
  size_t lfn_first = 0;
  uint8_t lfn_csum = 0;

  for (size_t i = 0; i < d->n; i++)
    {
      uint8_t *e = d->e + 32 * i;
      if (e[0] == 0x00)
	break;
      if (e[0] == 0xE5)
	{
	  lfn_ok = 0;
	  continue;
	}
      if (e[11] == 0x0F)
	{
	  int seq = e[0] & 0x1F;
	  if (e[0] & 0x40)
	    {
	      lfn_ok = seq;
	      lfn_first = i;
	      lfn_csum = e[13];
	      for (int k = 0; k < 260; k++)
		lfn[k] = 0xFFFF;
	    }
	  if (!lfn_ok || seq < 1 || seq > 20 || e[13] != lfn_csum)
	    {
	      lfn_ok = 0;
	      continue;
	    }
	  static const int off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	  for (int k = 0; k < 13; k++)
	    lfn[(seq - 1) * 13 + k] = rd16 (e + off[k]);
	  continue;
	}
      if (e[11] & 0x08)
	{
	  lfn_ok = 0;
	  continue;			/* volume label */
	}
      f->at = i;
      f->first = i;
      f->llen = 0;
      if (lfn_ok && lfn_sum (e) == lfn_csum)
	{
	  f->first = lfn_first;
	  while (f->llen < 255 && lfn[f->llen] && lfn[f->llen] != 0xFFFF)
	    {
	      f->lname[f->llen] = lfn[f->llen];
	      f->llen++;
	    }
	}
      lfn_ok = 0;
      f->slen = 0;
      for (int k = 0; k < 8 && e[k] != ' '; k++)
	f->sname[f->slen++] = e[k];
      if (e[8] != ' ')
	{
	  f->sname[f->slen++] = '.';
	  for (int k = 8; k < 11 && e[k] != ' '; k++)
	    f->sname[f->slen++] = e[k];
	}
      if ((f->llen && ieq16 (f->lname, f->llen, want, nwant))
	  || ieq16 (f->sname, f->slen, want, nwant))
	return 1;
    }
  return 0;
}

static int
sfn_char (int c)
{
  return isalnum (c) || (c && strchr ("$%'-_@~`!(){}^#&", c));
}

/* Can name be stored as a short name alone?  It can when it is 8.3 and
   each part is all one case; *nt gets the lower-case flags (byte 12:
   0x08 base, 0x10 extension) that keep a lower-case part's case.  */
static int
plain_83 (const char *name, uint8_t sfn[11], uint8_t *nt)
{
  const char *dot = strrchr (name, '.');
  size_t nb = dot ? (size_t) (dot - name) : strlen (name);
  size_t ne = dot ? strlen (dot + 1) : 0;
  if (!nb || nb > 8 || ne > 3 || (dot && !ne))
    return 0;
  memset (sfn, ' ', 11);
  *nt = 0;
  for (int part = 0; part < 2; part++)
    {
      const char *p = part ? (dot ? dot + 1 : "") : name;
      size_t n = part ? ne : nb;
      int lo = 0, up = 0;
      for (size_t i = 0; i < n; i++)
	{
	  int c = (unsigned char) p[i];
	  if (!sfn_char (c))
	    return 0;
	  lo |= islower (c) != 0;
	  up |= isupper (c) != 0;
	  sfn[(part ? 8 : 0) + i] = (uint8_t) toupper (c);
	}
      if (lo && up)
	return 0;
      if (lo)
	*nt |= part ? 0x10 : 0x08;
    }
  return 1;
}

static int
sfn_taken (dir_t *d, const uint8_t sfn[11])
{
  for (size_t i = 0; i < d->n; i++)
    {
      uint8_t *e = d->e + 32 * i;
      if (e[0] == 0x00)
	break;
      if (e[0] != 0xE5 && e[11] != 0x0F && !memcmp (e, sfn, 11))
	return 1;
    }
  return 0;
}

/* NAME~N.EXT from a long name.  */
static void
make_sfn (dir_t *d, const char *name, uint8_t sfn[11])
{
  char base[9] = "", ext[4] = "";
  const char *dot = strrchr (name, '.');
  size_t nb = 0, ne = 0;
  int lossy = 0;		/* something was dropped or replaced */
  for (const char *p = name; *p && p != dot; p++)
    if (nb == 8)
      lossy = 1;
    else if (sfn_char ((unsigned char) *p))
      base[nb++] = toupper ((unsigned char) *p);
    else if (*p != '.' && *p != ' ')
      base[nb++] = '_', lossy = 1;
    else
      lossy = 1;
  if (dot)
    {
      for (const char *p = dot + 1; *p; p++)
	if (ne < 3 && sfn_char ((unsigned char) *p))
	  ext[ne++] = toupper ((unsigned char) *p);
	else
	  lossy = 1;
    }
  if (!nb)
    base[nb++] = '_', lossy = 1;
  /* The name itself, upper-cased, when nothing was lost making it: the
     firmware finds files such as BPMsgs._BB by their short name.  */
  if (!lossy)
    {
      memset (sfn, ' ', 11);
      memcpy (sfn, base, nb);
      memcpy (sfn + 8, ext, ne);
      if (!sfn_taken (d, sfn))
	return;
    }
  for (int n = 1; n < 1000000; n++)
    {
      char tail[10];
      int tl = snprintf (tail, sizeof tail, "~%d", n);
      int keep = (int) nb < 8 - tl ? (int) nb : 8 - tl;
      memset (sfn, ' ', 11);
      memcpy (sfn, base, keep);
      memcpy (sfn + keep, tail, tl);
      memcpy (sfn + 8, ext, ne);
      if (!sfn_taken (d, sfn))
	return;
    }
}

static void
dos_time (uint8_t *e)
{
  time_t t = time (NULL);
  struct tm *tm = localtime (&t);
  uint16_t tim = tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2;
  uint16_t dat = (tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday;
  wr16 (e + 14, tim);
  wr16 (e + 16, dat);
  wr16 (e + 18, dat);
  wr16 (e + 22, tim);
  wr16 (e + 24, dat);
}

/* Add an entry; returns its SFN index or -1.  */
static long
dir_add (vol_t *v, dir_t *d, const char *name, uint8_t attr,
	 uint32_t clus, uint32_t size)
{
  uint8_t sfn[11], nt = 0;
  uint16_t u[256];
  size_t nu = 0, nl = 0;
  if (!plain_83 (name, sfn, &nt) || sfn_taken (d, sfn))
    {
      nt = 0;
      make_sfn (d, name, sfn);
      nu = utf16 (name, u, 255);
      nl = (nu + 12) / 13;
    }
  long at = dir_room (v, d, nl + 1);
  if (at < 0)
    return -1;
  uint8_t sum = lfn_sum (sfn);
  static const int off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
  for (size_t k = 0; k < nl; k++)
    {
      size_t seq = nl - k;
      uint8_t *e = d->e + 32 * (at + k);
      memset (e, 0, 32);
      e[0] = (uint8_t) (seq | (k == 0 ? 0x40 : 0));
      e[11] = 0x0F;
      e[13] = sum;
      for (int j = 0; j < 13; j++)
	{
	  size_t idx = (seq - 1) * 13 + j;
	  wr16 (e + off[j], idx < nu ? u[idx] : idx == nu ? 0 : 0xFFFF);
	}
    }
  uint8_t *e = d->e + 32 * (at + nl);
  memset (e, 0, 32);
  memcpy (e, sfn, 11);
  e[11] = attr;
  e[12] = nt;
  dos_time (e);
  wr16 (e + 20, clus >> 16);
  wr16 (e + 26, clus);
  wr32 (e + 28, size);
  return at + (long) nl;
}

static uint32_t
entry_clus (vol_t *v, const uint8_t *e)
{
  return rd16 (e + 26) | (v->type == 32 ? (uint32_t) rd16 (e + 20) << 16 : 0);
}

/* The folder's first cluster (0 = FAT16 root), made if missing.  */
static int
open_folder (vol_t *v, const char *folder, uint32_t *out)
{
  uint32_t cur = v->type == 32 ? v->root_clus : 0;
  char path[1024];
  snprintf (path, sizeof path, "%s", folder ? folder : "");
  for (char *part = strtok (path, "/\\"); part; part = strtok (NULL, "/\\"))
    {
      dir_t d;
      found_t f;
      if (dir_load (v, &d, cur))
	return err ("cannot read a folder", part);
      if (dir_find (&d, part, &f))
	{
	  uint8_t *e = d.e + 32 * f.at;
	  if (!(e[11] & 0x10))
	    {
	      free (d.e);
	      return err ("not a folder", part);
	    }
	  cur = entry_clus (v, e);
	  free (d.e);
	  continue;
	}
      uint32_t c = alloc_clus (v, 0);
      if (!c || dir_add (v, &d, part, 0x10, c, 0) < 0 || dir_store (v, &d))
	{
	  free (d.e);
	  return err ("cannot make folder", part);
	}
      free (d.e);
      /* The new folder: "." and "..", the rest empty.  */
      size_t bpc = v->spc * SECT;
      uint8_t *blk = calloc (1, bpc);
      memset (blk, ' ', 11);
      blk[0] = '.';
      blk[11] = 0x10;
      dos_time (blk);
      wr16 (blk + 20, c >> 16);
      wr16 (blk + 26, c);
      memcpy (blk + 32, blk, 32);
      blk[33] = '.';
      uint32_t parent = cur == (v->type == 32 ? v->root_clus : 0) ? 0 : cur;
      wr16 (blk + 32 + 20, parent >> 16);
      wr16 (blk + 32 + 26, parent);
      int r = io (v, clus_sec (v, c), blk, v->spc, 1);
      free (blk);
      if (r)
	return err ("cannot write folder", part);
      cur = c;
    }
  *out = cur;
  return 0;
}

static int
write_file (vol_t *v, uint32_t dirc, const char *name, const void *data,
	    size_t len)
{
  dir_t d;
  found_t f;
  if (dir_load (v, &d, dirc))
    return err ("cannot read folder", NULL);
  if (dir_find (&d, name, &f))
    {
      uint8_t *e = d.e + 32 * f.at;
      if (e[11] & 0x10)
	{
	  free (d.e);
	  return err ("a folder has that name", name);
	}
      free_chain (v, entry_clus (v, e));
      for (size_t i = f.first; i <= f.at; i++)
	d.e[32 * i] = 0xE5;
    }

  size_t bpc = v->spc * SECT;
  uint32_t first = 0, prev = 0;
  uint8_t *blk = malloc (bpc);
  for (size_t off = 0; off < len; off += bpc)
    {
      uint32_t c = alloc_clus (v, prev);
      if (!c)
	{
	  free (blk);
	  free (d.e);
	  if (first)
	    free_chain (v, first);
	  return err ("card full", name);
	}
      if (!first)
	first = c;
      size_t k = len - off < bpc ? len - off : bpc;
      memcpy (blk, (const uint8_t *) data + off, k);
      memset (blk + k, 0, bpc - k);
      if (io (v, clus_sec (v, c), blk, v->spc, 1))
	{
	  free (blk);
	  free (d.e);
	  return err ("write failed", name);
	}
      prev = c;
    }
  free (blk);
  int r = dir_add (v, &d, name, 0x20, first, (uint32_t) len) < 0
    || dir_store (v, &d);
  free (d.e);
  return r ? err ("folder full", name) : 0;
}

/* ---- Making a card.  */

int
card_create (const char *path, unsigned mb)
{
  if (mb < 8)
    return err ("cards start at 8 MB", NULL);
  FILE *f = fopen (path, "w+b");
  if (!f)
    return err ("cannot create", path);
  uint64_t sectors = (uint64_t) mb * 2048;
  const uint32_t start = 63;
  uint32_t psect = (uint32_t) (sectors - start);
  int fat32 = mb > 2000;

  /* Cluster size: the smallest that keeps the count in the type's range. */
  uint32_t spc = fat32 ? 8 : 1, reserved = fat32 ? 32 : 1;
  uint32_t rootsecs = fat32 ? 0 : 32, fatsz = 0, nclus = 0;
  for (;; spc *= 2)
    {
      fatsz = 1;
      for (int it = 0; it < 8; it++)
	{
	  nclus = (psect - reserved - 2 * fatsz - rootsecs) / spc;
	  fatsz = ((nclus + 2) * (fat32 ? 4 : 2) + SECT - 1) / SECT;
	}
      if (fat32 ? nclus < 0x0FFFFFF5 : nclus < 65525)
	break;
      if (spc >= 128)
	{
	  fclose (f);
	  return err ("card too large", NULL);
	}
    }

  uint8_t s[SECT];
  /* MBR.  */
  memset (s, 0, SECT);
  uint8_t *e = s + 446;
  e[0] = 0x80;
  e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
  e[4] = fat32 ? 0x0C : 0x06;
  e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
  wr32 (e + 8, start);
  wr32 (e + 12, psect);
  wr16 (s + 510, 0xAA55);
  int bad = fwrite (s, SECT, 1, f) != 1;

  /* Boot sector.  */
  memset (s, 0, SECT);
  memcpy (s, fat32 ? "\xEB\x58\x90" : "\xEB\x3C\x90", 3);
  memcpy (s + 3, "BPEMU   ", 8);
  wr16 (s + 11, SECT);
  s[13] = (uint8_t) spc;
  wr16 (s + 14, reserved);
  s[16] = 2;
  wr16 (s + 17, fat32 ? 0 : 512);
  if (psect < 65536)
    wr16 (s + 19, psect);
  else
    wr32 (s + 32, psect);
  s[21] = 0xF8;
  wr16 (s + 24, 63);
  wr16 (s + 26, 16);
  wr32 (s + 28, start);
  uint32_t serial = (uint32_t) time (NULL);
  uint8_t *x = s + 36;
  if (fat32)
    {
      wr32 (s + 36, fatsz);
      wr32 (s + 44, 2);		/* root cluster */
      wr16 (s + 48, 1);		/* FSInfo */
      wr16 (s + 50, 6);		/* backup boot sector */
      x = s + 64;
    }
  else
    wr16 (s + 22, fatsz);
  x[0] = 0x80;
  x[2] = 0x29;
  wr32 (x + 3, serial);
  memcpy (x + 7, "BOOKPORT   ", 11);
  memcpy (x + 18, fat32 ? "FAT32   " : "FAT16   ", 8);
  wr16 (s + 510, 0xAA55);

  uint64_t vb = (uint64_t) start * SECT;
  bad |= fseeko (f, (off_t) vb, SEEK_SET) || fwrite (s, SECT, 1, f) != 1;
  if (fat32)
    {
      bad |= fseeko (f, (off_t) (vb + 6 * SECT), SEEK_SET)
	|| fwrite (s, SECT, 1, f) != 1;
      uint8_t fi[SECT] = { 0 };
      wr32 (fi, 0x41615252);
      wr32 (fi + 484, 0x61417272);
      wr32 (fi + 488, nclus - 1);
      wr32 (fi + 492, 3);
      wr16 (fi + 510, 0xAA55);
      bad |= fseeko (f, (off_t) (vb + SECT), SEEK_SET)
	|| fwrite (fi, SECT, 1, f) != 1;
    }

  /* FATs, root directory with the volume label, then the size.  */
  uint8_t *fat = calloc (fatsz, SECT);
  if (fat32)
    {
      wr32 (fat, 0x0FFFFFF8);
      wr32 (fat + 4, 0x0FFFFFFF);
      wr32 (fat + 8, 0x0FFFFFFF);
    }
  else
    {
      wr16 (fat, 0xFFF8);
      wr16 (fat + 2, 0xFFFF);
    }
  for (int i = 0; i < 2; i++)
    bad |= fseeko (f, (off_t) (vb + (uint64_t) (reserved + i * fatsz) * SECT), SEEK_SET)
      || fwrite (fat, SECT, fatsz, f) != fatsz;
  free (fat);
  uint32_t dirsecs = fat32 ? spc : rootsecs;
  uint8_t *root = calloc (dirsecs, SECT);
  memcpy (root, "BOOKPORT   ", 11);
  root[11] = 0x08;
  dos_time (root);
  bad |= fseeko (f, (off_t) (vb + (uint64_t) (reserved + 2 * fatsz) * SECT), SEEK_SET)
    || fwrite (root, SECT, dirsecs, f) != dirsecs;
  free (root);
  uint8_t z = 0;
  bad |= fseeko (f, (off_t) (sectors * SECT - 1), SEEK_SET) || fwrite (&z, 1, 1, f) != 1;
  bad |= fclose (f) != 0;
  return bad ? err ("write failed", path) : 0;
}

/* ---- The text indexer: tools/bpindex.py's index_text_fixed and
   build_ix (old-style index, which every firmware reads).  */

enum { SENT, PARA, PAGE, SUBSEC, SECTN };	/* in rank order */
static const uint32_t v1_bits[] = {
  [SENT] = 0, [PARA] = 0x00400000, [PAGE] = 0x00C00000,
  [SUBSEC] = 0x05400000, [SECTN] = 0x03400000,
};
#define PAGESIZE 66
#define MAX_LINE 950

typedef struct
{
  uint8_t *b;
  size_t n, cap;
} buf_t;

static void
put (buf_t *o, uint8_t c)
{
  if (o->n == o->cap)
    {
      o->cap = o->cap ? o->cap * 2 : 4096;
      o->b = realloc (o->b, o->cap);
    }
  o->b[o->n++] = c;
}

typedef struct
{
  uint32_t off;
  int kind;
} mark_t;

static int
in_set (int c, const char *set)
{
  return c && strchr (set, c);
}

typedef struct
{
  buf_t out;
  mark_t *m;
  size_t nm, capm;
  int pending, linelen, curline, nls, para;
} ix_t;

static void
mark (ix_t *x, uint32_t off, int kind)
{
  if (x->nm == x->capm)
    {
      x->capm = x->capm ? x->capm * 2 : 256;
      x->m = realloc (x->m, x->capm * sizeof *x->m);
    }
  x->m[x->nm].off = off;
  x->m[x->nm++].kind = kind;
}

static int
last (ix_t *x)
{
  return x->out.n ? x->out.b[x->out.n - 1] : -1;
}

static void
brk (ix_t *x, int kind)
{
  if (last (x) == ' ')
    x->out.b[x->out.n - 1] = '\n';
  else if (last (x) != '\n')
    {
      put (&x->out, '\n');
      x->curline++;
    }
  if (kind == SENT && x->curline >= PAGESIZE)
    kind = PAGE;
  if (kind == PAGE)
    x->curline = 0;
  if (x->m[x->nm - 1].off == x->out.n)
    {
      if (kind > x->m[x->nm - 1].kind)
	x->m[x->nm - 1].kind = kind;
    }
  else
    mark (x, (uint32_t) x->out.n, kind);
  x->linelen = 0;
  x->pending = 0;
}

int
card_index_text (const void *text, size_t len, const char *name,
		 unsigned char **dd, size_t *ddlen,
		 unsigned char **ixout, size_t *ixlen)
{
  const uint8_t *data = text;
  ix_t x = { 0 };
  const uint8_t *nl = memchr (data, '\n', len);
  size_t flen = nl ? (size_t) (nl - data) + 1 : len;

  /* The first line is the title (CR LF becomes LF).  */
  uint8_t *first = malloc (flen + strlen (name) + 2);
  size_t fn = flen;
  memcpy (first, data, flen);
  if (fn >= 2 && first[fn - 2] == '\r' && first[fn - 1] == '\n')
    first[--fn - 1] = '\n';
  if (fn < 2)
    {
      fn = strlen (name) + 1;
      memcpy (first, name, fn);	/* with its NUL */
    }
  for (size_t i = 0; i < fn; i++)
    put (&x.out, first[i]);
  size_t tlen = fn ? fn - 1 : 0;
  if (tlen > 80)
    tlen = 80;
  mark (&x, 0, PAGE);
  mark (&x, (uint32_t) x.out.n, SENT);
  x.curline = 2;
  x.nls = 1;

  int prev = 0;
  for (size_t i = flen; i < len; i++)
    {
      int c = data[i];
      if (c == 13 || c == 127)
	continue;
      int at_start = last (&x) == '\n';
      if (c == 10)
	{
	  x.nls++;
	  if (x.pending)
	    {
	      if (!(at_start && x.nls >= 2))
		brk (&x, SENT);
	    }
	  else if (!at_start && last (&x) != ' ')
	    {
	      put (&x.out, ' ');
	      x.linelen++;
	    }
	}
      else if (c == 9 || c == 32)
	{
	  if (at_start || (last (&x) == ' ' && x.nls))
	    {
	      if (c == 9 && x.nls)
		x.para = 1;
	    }
	  else if (x.pending || x.linelen + 1 >= MAX_LINE)
	    brk (&x, SENT);
	  else
	    {
	      put (&x.out, (uint8_t) c);
	      x.linelen++;
	    }
	}
      else if (c == 12)
	brk (&x, PAGE);
      else if (c == 1)
	brk (&x, SECTN);
      else if (c >= 2 && c <= 6)
	brk (&x, SUBSEC);
      else
	{
	  if (x.pending && !in_set (c, ".?!\""))
	    x.pending = 0;
	  if (x.nls >= 2 || x.para)
	    brk (&x, PARA);
	  x.nls = 0;
	  x.para = 0;
	  put (&x.out, c & 0x7F);
	  x.linelen++;
	  if (in_set (c, ".?!"))
	    x.pending = 1;
	  else if (c == '"' && in_set (prev, ".?!,"))
	    x.pending = 1;
	}
      prev = c;
    }
  mark (&x, (uint32_t) x.out.n + 1, SENT);
  put (&x.out, '\n');
  put (&x.out, 0);
  put (&x.out, 0);

  /* The index: header, the three strings, the marks.  */
  buf_t ix = { 0 };
  size_t nlen = strlen (name);
  if (nlen > 256)
    nlen = 256;
  uint8_t hdr[14] = { 1, 14, 0, 0, 1 };
  /* Bytes up to the marks: header, three u16 lengths, the strings.  */
  uint32_t strs = (uint32_t) (nlen + 1) * 2 + (uint32_t) tlen + 1;
  wr32 (hdr + 6, (uint32_t) x.nm);
  wr16 (hdr + 10, (uint16_t) (14 + 6 + strs));
  for (int i = 0; i < 14; i++)
    put (&ix, hdr[i]);
  const uint8_t *s3[3] = { (const uint8_t *) name, first, (const uint8_t *) name };
  size_t l3[3] = { nlen, tlen, nlen };
  for (int k = 0; k < 3; k++)
    {
      put (&ix, (uint8_t) (l3[k] + 1));
      put (&ix, (uint8_t) ((l3[k] + 1) >> 8));
      for (size_t i = 0; i < l3[k]; i++)
	put (&ix, s3[k][i]);
      put (&ix, 0);
    }
  for (size_t i = 0; i < x.nm; i++)
    {
      uint32_t v = x.m[i].off | v1_bits[x.m[i].kind];
      for (int k = 0; k < 4; k++)
	put (&ix, (uint8_t) (v >> (8 * k)));
    }
  free (first);
  free (x.m);
  *dd = x.out.b;
  *ddlen = x.out.n;
  *ixout = ix.b;
  *ixlen = ix.n;
  return 0;
}

/* ---- Putting files on a card.  */

static int
ends_with_txt (const char *name)
{
  size_t n = strlen (name);
  return n > 4 && (name[n - 4] == '.') && tolower ((unsigned char) name[n - 3]) == 't'
    && tolower ((unsigned char) name[n - 2]) == 'x'
    && tolower ((unsigned char) name[n - 1]) == 't';
}

static int
add_one (vol_t *v, uint32_t dirc, const char *name, const void *data,
	 size_t len)
{
  if (!ends_with_txt (name))
    {
      printf ("%s: copied as is\n", name);
      return write_file (v, dirc, name, data, len);
    }
  unsigned char *dd, *ix;
  size_t ddlen, ixlen;
  card_index_text (data, len, name, &dd, &ddlen, &ix, &ixlen);
  char n1[1100], n2[1100];
  snprintf (n1, sizeof n1, "%s._DD", name);
  snprintf (n2, sizeof n2, "%s._IX", name);
  int r = write_file (v, dirc, n1, dd, ddlen) || write_file (v, dirc, n2, ix, ixlen);
  printf ("%s: book, %u marks\n", name, (unsigned) rd32 (ix + 6));
  free (dd);
  free (ix);
  return r ? -1 : 0;
}

static const char *
base_name (const char *p)
{
  const char *b = p;
  for (; *p; p++)
    if (*p == '/' || *p == '\\')
      b = p + 1;
  return b;
}

int
card_add (const char *img, const char *folder, int n, const char *const *files)
{
  vol_t v;
  uint32_t dirc;
  if (vol_open (&v, img) || open_folder (&v, folder, &dirc))
    {
      if (v.f)
	vol_close (&v);
      return -1;
    }
  int r = 0;
  for (int i = 0; i < n; i++)
    {
      FILE *f = open_source (files[i]);
      if (!f)
	{
	  r = err ("cannot read", files[i]);
	  continue;
	}
      fseeko (f, 0, SEEK_END);
      size_t len = (size_t) ftello (f);
      fseeko (f, 0, SEEK_SET);
      uint8_t *data = malloc (len ? len : 1);
      if (fread (data, 1, len, f) != len)
	r = err ("cannot read", files[i]);
      else if (add_one (&v, dirc, base_name (files[i]), data, len))
	r = -1;
      free (data);
      fclose (f);
    }
  if (vol_close (&v))
    r = -1;
  return r;
}

int
card_add_data (const char *img, const char *folder, const char *name,
	       const void *data, size_t len)
{
  vol_t v;
  uint32_t dirc;
  if (vol_open (&v, img) || open_folder (&v, folder, &dirc))
    {
      if (v.f)
	vol_close (&v);
      return -1;
    }
  int r = add_one (&v, dirc, name, data, len);
  return vol_close (&v) || r ? -1 : 0;
}
